// The publishers of the viewports (Publishing.h)
#include "Publishing.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif

struct Publishing::Publication {
    PublishSettings applied;
    bool init = false;
    std::unique_ptr<GpuPublisher> gpu[kPublishKindCount];
    std::shared_ptr<CpuPublisher> cpu[kPublishKindCount], tap;
    std::unique_ptr<CpuSendThread> sender;
    PublishState states[kPublishKindCount]; // guarded by m_stateMutex
    RenderTarget readback;
    GLuint pbo[2] = {0, 0};
    GLsync fence[2] = {nullptr, nullptr}; // the read back into each buffer, done
    int pboIndex = 0, pboW = 0, pboH = 0;
    bool pboPending = false;
    double pboTime = 0;
    std::optional<Timecode> pboTc; // the timecode of the picture in the pending PBO
    int announcedRate = 0;
    double rateSince = 0;
};

Publishing::Publishing(std::function<void()> drawQuad) : m_drawQuad(std::move(drawQuad)) { m_clock.start(); }

Publishing::~Publishing() = default;

bool Publishing::initGl(QString *log)
{
    // Vertical flip for readback (NDI / OMT expect top-to-bottom rows)
    m_flipProgram = compileProgram("#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                                   "void main(){ v_uv = vec2(a_pos.x*0.5+0.5, 0.5-a_pos.y*0.5); gl_Position = vec4(a_pos,0.0,1.0); }\n",
                                   "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
                                   log);
    if (!m_flipProgram) return false;
    m_flipTexLoc = gl()->glGetUniformLocation(m_flipProgram, "u_tex");
    return true;
}

void Publishing::release()
{
    // GPU publishers need the context, the send threads are stopped before the CPU publishers go
    for (auto &[id, pub] : m_pubs) releasePublication(*pub);
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_pubs.clear();
    }
    if (m_flipProgram && QOpenGLContext::currentContext()) gl()->glDeleteProgram(m_flipProgram);
    m_flipProgram = 0;
}

void Publishing::setTap(std::function<void(const CpuFrame &)> fn)
{
    std::lock_guard<std::mutex> lk(m_tapMutex);
    m_tap = std::move(fn);
    m_tapDirty = true;
}

PublishState Publishing::state(quint64 viewport, PublishKind k) const
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    auto it = m_pubs.find(viewport);
    return it == m_pubs.end() ? PublishState() : it->second->states[int(k)];
}

void Publishing::setState(Publication &pub, PublishKind k, PublishState st)
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    pub.states[int(k)] = std::move(st);
}

static bool isGpuKind(PublishKind k) { return k == PublishKind::Syphon || k == PublishKind::Spout; }

void Publishing::follow(const std::vector<std::pair<quint64, PublishSettings>> &viewports)
{
    std::function<void(const CpuFrame &)> tap;
    bool tapDirty;
    {
        std::lock_guard<std::mutex> lk(m_tapMutex);
        tapDirty = std::exchange(m_tapDirty, false);
        if (tapDirty) tap = m_tap;
    }
    bool changed = tapDirty || viewports.size() != m_pubs.size();
    for (const auto &[id, s] : viewports) {
        if (changed) break;
        const auto it = m_pubs.find(id);
        changed = it == m_pubs.end() || !(it->second->applied == s);
    }
    if (!changed) return;

    for (auto it = m_pubs.begin(); it != m_pubs.end();) {
        const bool alive = std::any_of(viewports.begin(), viewports.end(), [&](const auto &w) { return w.first == it->first; });
        if (alive) {
            ++it;
            continue;
        }
        releasePublication(*it->second);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        it = m_pubs.erase(it);
    }
    for (size_t k = 0; k < viewports.size(); ++k) {
        auto &slot = m_pubs[viewports[k].first];
        if (!slot) {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            slot = std::make_unique<Publication>();
        }
        Publication &pub = *slot;
        // The test tap follows the first viewport
        if (k == 0 && tapDirty) {
            pub.tap = tap ? std::shared_ptr<CpuPublisher>(createTapPublisher(tap)) : nullptr;
            pub.init = false; // the send thread is rebuilt below
        } else if (k != 0 && pub.tap) {
            pub.tap.reset();
            pub.init = false;
        }
        apply(pub, viewports[k].second);
    }
}

void Publishing::apply(Publication &pub, const PublishSettings &s)
{
    const bool first = !pub.init;
    pub.init = true;
    bool cpuChanged = first;
    for (int ki = 0; ki < kPublishKindCount; ++ki) {
        const PublishKind k = PublishKind(ki);
        const PublishTarget &want = s.targets[ki];
        const PublishTarget &had = pub.applied.targets[ki];
        const bool optionsChanged = s.libraryFolder != pub.applied.libraryFolder
                                    || (k == PublishKind::Omt && s.omtQuality != pub.applied.omtQuality)
                                    || (k == PublishKind::DeckLink && s.decklinkMode != pub.applied.decklinkMode);
        if (!first && want == had && !(want.enabled && optionsChanged)) continue;

        if (isGpuKind(k)) {
            pub.gpu[ki].reset();
        } else if (pub.cpu[ki]) {
            pub.cpu[ki].reset();
            cpuChanged = true;
        }
        if (!want.enabled) {
            setState(pub, k, {PublishState::Off, QStringLiteral("Disabled"), -1});
            continue;
        }
        if (!publishCompiledIn(k)) {
            setState(pub, k,
                     {PublishState::Unavailable,
                      k == PublishKind::Syphon   ? QStringLiteral("Syphon is only available on macOS")
                      : k == PublishKind::Spout  ? QStringLiteral("Spout is only available on Windows")
                                                 : QStringLiteral("Built without DeckLink support"),
                      -1});
            continue;
        }
        QString err;
        if (isGpuKind(k)) {
            auto p = k == PublishKind::Syphon ? createSyphonPublisher() : createSpoutPublisher();
            if (p && p->start(want.name, &err)) {
                pub.gpu[ki] = std::move(p);
                setState(pub, k, {PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), -1});
            } else {
                setState(pub, k, {PublishState::Error, err.isEmpty() ? QStringLiteral("Failed to start") : err, -1});
            }
        } else {
            std::shared_ptr<CpuPublisher> p(k == PublishKind::Ndi   ? createNdiPublisher()
                                            : k == PublishKind::Omt ? createOmtPublisher()
                                                                    : createDeckLinkPublisher());
            if (p && p->start(want.name, s, &err)) {
                pub.cpu[ki] = p;
                cpuChanged = true;
                PublishState st{PublishState::Ok, QStringLiteral("Active: \"%1\"").arg(want.name), 0};
                p->liveState(&st);
                setState(pub, k, st);
            } else {
                setState(pub, k, {PublishState::Error, err, -1});
            }
        }
    }
    pub.applied = s;
    if (cpuChanged) {
        std::vector<std::shared_ptr<CpuPublisher>> pubs;
        for (auto &p : pub.cpu)
            if (p) pubs.push_back(p);
        if (pub.tap) pubs.push_back(pub.tap);
        if (!pubs.empty() && !pub.sender) pub.sender = std::make_unique<CpuSendThread>();
        if (pub.sender) pub.sender->setPublishers(std::move(pubs));
        pub.pboPending = false;
    }
}

void Publishing::releasePublication(Publication &pub)
{
    for (auto &p : pub.gpu) p.reset();
    if (pub.sender) pub.sender->setPublishers({});
    pub.sender.reset();
    for (auto &p : pub.cpu) p.reset();
    pub.tap.reset();
    pub.readback.destroy();
    for (GLsync &s : pub.fence)
        if (s) {
            gl()->glDeleteSync(s);
            s = nullptr;
        }
    if (pub.pbo[0]) gl()->glDeleteBuffers(2, pub.pbo);
    pub.pbo[0] = pub.pbo[1] = 0;
}

bool Publishing::sharing() const
{
    for (const auto &[id, pub] : m_pubs)
        for (int ki : {int(PublishKind::Syphon), int(PublishKind::Spout)})
            if (pub->gpu[ki]) return true;
    return false;
}

void Publishing::share(quint64 viewport, const RenderTarget &picture)
{
    const auto it = m_pubs.find(viewport);
    if (it == m_pubs.end()) return;
    for (int ki : {int(PublishKind::Syphon), int(PublishKind::Spout)})
        if (it->second->gpu[ki]) it->second->gpu[ki]->publish(picture.tex, picture.w, picture.h);
}

static int standardRate(double fps)
{
    static const int rates[] = {24, 25, 30, 48, 50, 60, 72, 75, 90, 100, 120};
    if (fps < 1.0) return 60;
    int best = 60;
    double bestD = 1e9;
    for (int r : rates) {
        const double d = std::fabs(fps - r);
        if (d < bestD) {
            bestD = d;
            best = r;
        }
    }
    return best;
}

void Publishing::readback(quint64 viewport, const RenderTarget &out, double fps, quint64 frame,
                          const std::optional<Timecode> &tc)
{
    const auto found = m_pubs.find(viewport);
    if (found == m_pubs.end()) return;
    Publication &pub = *found->second;
    // Receiver count, about twice per second
    if ((frame % 30) == 0) {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        for (int ki = 0; ki < kPublishKindCount; ++ki) {
            if (pub.cpu[ki] && pub.cpu[ki]->liveState(&pub.states[ki])) continue; // it tells how it goes
            if (pub.states[ki].level != PublishState::Ok) continue;
            if (pub.cpu[ki]) pub.states[ki].receivers = pub.cpu[ki]->receivers();
            else if (pub.gpu[ki]) pub.states[ki].receivers = pub.gpu[ki]->receivers();
        }
    }

    if (!pub.sender || !pub.sender->hasPublishers() || !out.tex) {
        pub.pboPending = false;
        return;
    }
    auto f = gl();
    const int w = out.w, h = out.h;
    const GLsizeiptr bytes = GLsizeiptr(w) * h * 4;
    if (!pub.pbo[0] || w != pub.pboW || h != pub.pboH) {
        if (!pub.pbo[0]) f->glGenBuffers(2, pub.pbo);
        for (GLuint b : pub.pbo) {
            f->glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
            f->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        for (GLsync &s : pub.fence)
            if (s) {
                f->glDeleteSync(s);
                s = nullptr;
            }
        pub.pboW = w;
        pub.pboH = h;
        pub.pboPending = false;
    }
    pub.readback.ensure(w, h);

    // 1) Current frame flipped (top-to-bottom rows), then read back asynchronously into a PBO
    pub.readback.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(m_flipProgram);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, out.tex);
    f->glUniform1i(m_flipTexLoc, 0);
    m_drawQuad();
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, pub.pbo[pub.pboIndex]);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    if (pub.fence[pub.pboIndex]) f->glDeleteSync(pub.fence[pub.pboIndex]);
    pub.fence[pub.pboIndex] = f->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // 2) Previous frame, normally available by now: copied to the send thread. Its read back not done yet (the GPU
    // late): waited for 2 ms at most, then that frame is not sent (counted) rather than the render thread held up
    // by the mapping
    bool ready = pub.pboPending;
    if (ready && pub.fence[1 - pub.pboIndex]) {
        const GLenum r = f->glClientWaitSync(pub.fence[1 - pub.pboIndex], GL_SYNC_FLUSH_COMMANDS_BIT, 2000000);
        ready = r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
        if (!ready) ++m_readbacksSkipped;
    }
    if (ready) {
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, pub.pbo[1 - pub.pboIndex]);
        const void *ptr = f->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (ptr) {
            if (CpuFrame *cf = pub.sender->acquire()) {
                cf->bgra.resize(size_t(bytes));
                std::memcpy(cf->bgra.data(), ptr, size_t(bytes));
                cf->width = w;
                cf->height = h;
                cf->stride = w * 4;
                cf->timestamp100ns = qint64(pub.pboTime);
                cf->timecode = pub.pboTc;
                // Announced frame rate: only changes if the new value holds for 2 s
                const int rate = standardRate(fps);
                const double nowS = m_clock.nsecsElapsed() / 1e9;
                if (pub.announcedRate == 0) pub.announcedRate = rate;
                if (rate == pub.announcedRate) pub.rateSince = nowS;
                else if (nowS - pub.rateSince > 2.0) pub.announcedRate = rate;
                cf->fpsN = pub.announcedRate;
                cf->fpsD = 1;
                pub.sender->submit(cf);
            }
            f->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    pub.pboTime = double(m_clock.nsecsElapsed() / 100);
    pub.pboTc = tc;
    pub.pboPending = true;
    pub.pboIndex = 1 - pub.pboIndex;
}
