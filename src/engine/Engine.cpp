// Engine: life of the engine itself — OpenGL context and render thread, the runGl() task protocol,
// the output window, the composition size, and the composition opacity / blackout fades.
// The rest of its methods live in Layers.cpp, Render.cpp, Project.cpp, Media.cpp and Snapshots.cpp.
#include "EngineInternal.h"
#include "ProcessMemory.h"
#include "Midi.h"
#include "VideoDecoder.h"
#include "VideoTexture.h"
#include "WorkerPool.h"

#include <QDebug>
#include <QJsonArray>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QThread>
#include <QWindow>
#include <cmath>

#ifdef Q_OS_MACOS
#include <OpenGL/OpenGL.h> // GL_SILENCE_DEPRECATION: set by CMake
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Render thread, OpenGL context, start and stop
// ---------------------------------------------------------------------------
std::atomic<bool> Engine::s_failRenderStart{false};

class RenderThread : public QThread
{
public:
    explicit RenderThread(Engine *e) : m_engine(e) { setObjectName("Pinol-render"); }

protected:
    void run() override { m_engine->renderLoop(); }

private:
    Engine *m_engine;
};

// Makes the engine context current for the duration of a scope (manual mode only).
struct ScopedCurrent {
    Engine *e;
    explicit ScopedCurrent(Engine *engine) : e(engine) { e->makeCurrent(); }
    ~ScopedCurrent() { e->doneCurrent(); }
};

Engine::Engine(QObject *parent)
    : QObject(parent), m_audio(std::make_unique<AudioOutput>()), m_sequencer(new Sequencer(*this, m_mutex)),
      m_tcRouter(std::make_unique<TimecodeRouter>())
{
    connect(this, &Engine::timecodeRoutingChanged, this, &Engine::updateTimecodeRouting);
    connect(this, &Engine::layersChanged, this, &Engine::updateTimecodeRouting); // a layer gone, or back (undo)
}

bool Engine::startAudio(const QString &device, QString *err, bool nullDevice)
{
    return m_audio->start(device, err, nullDevice);
}

void Engine::stopAudio() { m_audio->stop(); }

Engine::~Engine()
{
    m_sequencer->shutdownClock(); // the cue clock recalls snapshots on its own thread: stopped before anything goes
    m_midiIns.clear(); // their callbacks feed the interactions: stopped first
    shutdown();
}

void Engine::makeCurrent()
{
    if (m_context) m_context->makeCurrent(m_surface);
    m_currentSurface = m_surface;
}

void Engine::doneCurrent()
{
    if (m_context) m_context->doneCurrent();
    m_currentSurface = nullptr;
}

bool Engine::initialize(QString *err)
{
    m_ownerThread = QThread::currentThread();
    // No parent: the context is moved to the render thread.
    m_context = new QOpenGLContext;
    m_context->setShareContext(QOpenGLContext::globalShareContext());
    m_context->setFormat(QSurfaceFormat::defaultFormat());
    if (!m_context->create()) {
        if (err) *err = QStringLiteral("Unable to create an OpenGL 3.3 context.");
        return false;
    }
    m_surface = new QOffscreenSurface(nullptr, this);
    m_surface->setFormat(m_context->format());
    m_surface->create();
    if (!m_context->makeCurrent(m_surface)) {
        if (err) *err = QStringLiteral("Unable to make the OpenGL context current.");
        return false;
    }
    const QSurfaceFormat fmt = m_context->format();
    if (fmt.majorVersion() * 10 + fmt.minorVersion() < 33) {
        if (err)
            *err = QStringLiteral("OpenGL 3.3 required (got %1.%2).").arg(fmt.majorVersion()).arg(fmt.minorVersion());
        return false;
    }
    auto f = gl();
    m_glRenderer = QString::fromLatin1(reinterpret_cast<const char *>(f->glGetString(GL_RENDERER)));
    m_glVersion = QString::fromLatin1(reinterpret_cast<const char *>(f->glGetString(GL_VERSION)));
    m_glVendor = QString::fromLatin1(reinterpret_cast<const char *>(f->glGetString(GL_VENDOR)));
    qInfo().noquote() << "OpenGL" << reinterpret_cast<const char *>(f->glGetString(GL_VERSION)) << "-"
                      << reinterpret_cast<const char *>(f->glGetString(GL_RENDERER));

    // Fullscreen quad
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    f->glGenVertexArrays(1, &m_quadVao);
    f->glBindVertexArray(m_quadVao);
    f->glGenBuffers(1, &m_quadVbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Mapping mesh: dynamic vertices, fixed indices
    f->glGenVertexArrays(1, &m_meshVao);
    f->glBindVertexArray(m_meshVao);
    f->glGenBuffers(1, &m_meshVbo);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_meshVbo);
    { // the whole frame (viewport outputs); each layer has a buffer of its own
        std::vector<float> full;
        Mapping().buildVertices(kMeshSubdiv, full);
        f->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(full.size() * sizeof(float)), full.data(), GL_STATIC_DRAW);
    }
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
    std::vector<GLuint> idx;
    const int n = kMeshSubdiv;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            GLuint a = GLuint(y * (n + 1) + x), b = a + 1, c = a + GLuint(n + 1), d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    f->glGenBuffers(1, &m_meshIbo);
    f->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_meshIbo);
    f->glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(GLuint)), idx.data(), GL_STATIC_DRAW);
    m_meshIndexCount = GLsizei(idx.size());
    f->glBindVertexArray(0);

    QString log;
    const char *quadVs = "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                         "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n";
    m_blitProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ o = texture(u_tex, v_uv); }\n",
                                   &log);
    m_presentProgram = compileProgram(quadVs,
                                      "#version 330 core\nuniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
                                      "void main(){ o = vec4(texture(u_tex, v_uv).rgb, 1.0); }\n",
                                      &log);
    m_compProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; out vec2 v_uv;\n"
        "uniform vec4 u_view;\n" // scale, offset: the part of the composition the target shows
        "uniform vec4 u_rot; uniform float u_asp;\n" // that part turned: cos, sin, center (clip space); composition width / height
        "out vec2 v_comp;\n" // where in the composition (0..1, origin bottom left): its masks there
        "void main(){ v_uv = a_uv; v_comp = a_pos*0.5+0.5; vec2 q = a_pos - u_rot.zw; q.x *= u_asp;\n"
        "  q = vec2(u_rot.x*q.x - u_rot.y*q.y, u_rot.y*q.x + u_rot.x*q.y); q.x /= u_asp;\n"
        "  gl_Position = vec4((q+u_rot.zw)*u_view.xy+u_view.zw,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "uniform sampler2D u_mask; uniform int u_maskOn; in vec2 v_comp;\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity;\n"
        "  if (u_maskOn != 0) a *= texture(u_mask, vec2(v_comp.x, 1.0 - v_comp.y)).r;\n"
        "  o = vec4(c.rgb*a, a); }\n",
        &log);
    // Difference needs what is already drawn, which blending cannot give: the layer reads a copy of it
    m_diffProgram = compileProgram(
        "#version 330 core\nlayout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; out vec2 v_uv;\n"
        "uniform vec4 u_view;\n"
        "uniform vec4 u_rot; uniform float u_asp;\n"
        "out vec2 v_comp;\n"
        "void main(){ v_uv = a_uv; v_comp = a_pos*0.5+0.5; vec2 q = a_pos - u_rot.zw; q.x *= u_asp;\n"
        "  q = vec2(u_rot.x*q.x - u_rot.y*q.y, u_rot.y*q.x + u_rot.x*q.y); q.x /= u_asp;\n"
        "  gl_Position = vec4((q+u_rot.zw)*u_view.xy+u_view.zw,0.0,1.0); }\n",
        "#version 330 core\nuniform sampler2D u_tex; uniform sampler2D u_dst; uniform float u_opacity; in vec2 v_uv; out vec4 o;\n"
        "uniform sampler2D u_mask; uniform int u_maskOn; in vec2 v_comp;\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); float a = c.a*u_opacity;\n"
        "  if (u_maskOn != 0) a *= texture(u_mask, vec2(v_comp.x, 1.0 - v_comp.y)).r;\n"
        "  vec4 d = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0);\n"
        "  o = vec4(abs(d.rgb - c.rgb*a), d.a + a*(1.0-d.a)); }\n",
        &log);
    if (!m_blitProgram || !m_compProgram || !m_presentProgram || !m_diffProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_blitTexLoc = f->glGetUniformLocation(m_blitProgram, "u_tex");
    m_presentTexLoc = f->glGetUniformLocation(m_presentProgram, "u_tex");
    m_publishing.initGl(&log);
    // Layer preparation: roi (part of the source used), color (added / removed), unpremultiplied alpha (groups)
    m_prepProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_tex; uniform vec4 u_roi; uniform vec3 u_add;\n"
                                   "uniform vec3 u_remove; uniform vec3 u_balance; uniform int u_unpremul;\n"
                                   "uniform sampler2D u_mask; uniform int u_maskMode; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 c = texture(u_tex, mix(u_roi.xy, u_roi.zw, v_uv));\n"
                                   "  if (u_unpremul != 0 && c.a > 0.0) c.rgb /= c.a;\n"
                                   "  vec3 col = clamp(c.rgb * u_balance * (1.0 - u_remove) + u_add, 0.0, 1.0);\n"
                                   // Color through a mask: 1 as it is, 2 inverted (luminance times alpha)
                                   "  if (u_maskMode != 0) { vec4 m = texture(u_mask, v_uv);\n"
                                   "    float k = clamp(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * m.a, 0.0, 1.0);\n"
                                   "    if (u_maskMode == 2) k = 1.0 - k;\n"
                                   "    col = mix(clamp(c.rgb, 0.0, 1.0), col, k); }\n"
                                   "  o = vec4(col, c.a); }\n",
                                   &log);
    if (!m_prepProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    // Mask of an effect: input and result mixed by the mask's luminance (times its alpha), the mask stretched
    m_maskProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_in; uniform sampler2D u_fx; uniform sampler2D u_mask;\n"
                                   "uniform int u_invert; in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 m = texture(u_mask, v_uv);\n"
                                   "  float k = clamp(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * m.a, 0.0, 1.0);\n"
                                   "  if (u_invert != 0) k = 1.0 - k;\n"
                                   "  o = mix(texture(u_in, v_uv), texture(u_fx, v_uv), k); }\n",
                                   &log);
    if (!m_maskProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    // A picture through its masks' alpha (Masks.h): the alpha only, or the color too (a viewport: shown opaque)
    m_shapeMaskProgram = compileProgram(quadVs,
                                        "#version 330 core\nuniform sampler2D u_tex; uniform sampler2D u_mask; uniform int u_rgb;\n"
                                        "in vec2 v_uv; out vec4 o;\n"
                                        "void main(){ vec4 c = texture(u_tex, v_uv);\n"
                                        "  float m = texture(u_mask, vec2(v_uv.x, 1.0 - v_uv.y)).r;\n" // stored top row first
                                        "  o = u_rgb != 0 ? c * m : vec4(c.rgb, c.a * m); }\n",
                                        &log);
    if (!m_shapeMaskProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_shapeMaskTexLoc = f->glGetUniformLocation(m_shapeMaskProgram, "u_tex");
    m_shapeMaskMaskLoc = f->glGetUniformLocation(m_shapeMaskProgram, "u_mask");
    m_shapeMaskRgbLoc = f->glGetUniformLocation(m_shapeMaskProgram, "u_rgb");
    // A crossfade between two pictures (a missing one: transparent), mixed in the shader rather than by blending
    m_fadeProgram = compileProgram(quadVs,
                                   "#version 330 core\nuniform sampler2D u_a; uniform sampler2D u_b; uniform vec3 u_k;\n"
                                   "in vec2 v_uv; out vec4 o;\n"
                                   "void main(){ vec4 a = u_k.y > 0.5 ? texture(u_a, v_uv) : vec4(0.0);\n"
                                   "  vec4 b = u_k.z > 0.5 ? texture(u_b, v_uv) : vec4(0.0);\n"
                                   "  o = mix(a, b, u_k.x); }\n",
                                   &log);
    if (!m_fadeProgram) {
        if (err) *err = QStringLiteral("Internal shaders: ") + log;
        return false;
    }
    m_fadeALoc = f->glGetUniformLocation(m_fadeProgram, "u_a");
    m_fadeBLoc = f->glGetUniformLocation(m_fadeProgram, "u_b");
    m_fadeKLoc = f->glGetUniformLocation(m_fadeProgram, "u_k");
    m_maskInLoc = f->glGetUniformLocation(m_maskProgram, "u_in");
    m_maskFxLoc = f->glGetUniformLocation(m_maskProgram, "u_fx");
    m_maskMaskLoc = f->glGetUniformLocation(m_maskProgram, "u_mask");
    m_maskInvertLoc = f->glGetUniformLocation(m_maskProgram, "u_invert");
    m_prepTexLoc = f->glGetUniformLocation(m_prepProgram, "u_tex");
    m_prepRoiLoc = f->glGetUniformLocation(m_prepProgram, "u_roi");
    m_prepAddLoc = f->glGetUniformLocation(m_prepProgram, "u_add");
    m_prepRemoveLoc = f->glGetUniformLocation(m_prepProgram, "u_remove");
    m_prepUnpremulLoc = f->glGetUniformLocation(m_prepProgram, "u_unpremul");
    m_prepBalanceLoc = f->glGetUniformLocation(m_prepProgram, "u_balance");
    m_prepMaskLoc = f->glGetUniformLocation(m_prepProgram, "u_mask");
    m_prepMaskModeLoc = f->glGetUniformLocation(m_prepProgram, "u_maskMode");
    m_compTexLoc = f->glGetUniformLocation(m_compProgram, "u_tex");
    m_compOpacityLoc = f->glGetUniformLocation(m_compProgram, "u_opacity");
    m_compMaskLoc = f->glGetUniformLocation(m_compProgram, "u_mask");
    m_compMaskOnLoc = f->glGetUniformLocation(m_compProgram, "u_maskOn");
    m_diffMaskLoc = f->glGetUniformLocation(m_diffProgram, "u_mask");
    m_diffMaskOnLoc = f->glGetUniformLocation(m_diffProgram, "u_maskOn");
    m_diffTexLoc = f->glGetUniformLocation(m_diffProgram, "u_tex");
    m_diffDstLoc = f->glGetUniformLocation(m_diffProgram, "u_dst");
    m_diffOpacityLoc = f->glGetUniformLocation(m_diffProgram, "u_opacity");
    m_diffViewLoc = f->glGetUniformLocation(m_diffProgram, "u_view");
    m_compViewLoc = f->glGetUniformLocation(m_compProgram, "u_view");
    m_compRotLoc = f->glGetUniformLocation(m_compProgram, "u_rot");
    m_compAspLoc = f->glGetUniformLocation(m_compProgram, "u_asp");
    m_diffRotLoc = f->glGetUniformLocation(m_diffProgram, "u_rot");
    m_diffAspLoc = f->glGetUniformLocation(m_diffProgram, "u_asp");

    if (!m_videoConv.init(m_quadVao, &log)) {
        if (err) *err = log;
        return false;
    }
    // HAP: what the decoders leave compressed for the GPU, and what they decode themselves
    VideoDecoder::setGpuFormats(m_videoConv.s3tc(), m_videoConv.bptc());
    qInfo().noquote() << "Compressed textures: DXT" << (m_videoConv.s3tc() ? "yes" : "no") << "- BPTC"
                      << (m_videoConv.bptc() ? "yes" : "no (Hap R and Hap HDR decoded on the CPU)");
    // Hardware-decoded frames sampled where they were decoded (macOS): no copy, no upload
    VideoDecoder::setZeroCopy(m_videoConv.surfaces());
    if (m_videoConv.surfaces()) qInfo("Hardware video frames: zero copy");

    const unsigned char black[4] = {0, 0, 0, 0};
    f->glGenTextures(1, &m_blackTex);
    f->glBindTexture(GL_TEXTURE_2D, m_blackTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (RenderTarget &o : m_output) o.ensure(m_compSize.width(), m_compSize.height());
    doneCurrent();

    m_library.scan();
    m_clock.start();
    ensureViewport(); // the composition is always shown through at least one viewport
    m_lastNs = m_clock.nsecsElapsed();
    m_initialized = true;
    return true;
}

bool Engine::start()
{
    if (!m_initialized || m_threaded) return m_threaded;
    // macOS never offers OpenGL in a window from another thread: the engine still renders on its own thread (into its
    // framebuffers), the windows being shown through Metal
    if (!QOpenGLContext::supportsThreadedOpenGL() && !metalOutput()) {
        qWarning("Platform does not support OpenGL rendering on a separate thread: rendering on the UI thread.");
        return false;
    }
    m_quit = false;
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        m_renderFailed = false;
    }
    // The context must not be current on any thread before being handed to the render thread.
    if (QOpenGLContext::currentContext() == m_context) doneCurrent();
    m_thread = new RenderThread(this);
    m_context->moveToThread(m_thread);
    m_threaded = true;
    m_thread->start(QThread::HighPriority);
    return true;
}

void Engine::stop()
{
    if (!m_threaded) return;
    m_quit = true;
    m_taskCv.notify_all();
    m_thread->wait();
    tsanAcquire(&m_thread);
    delete m_thread;
    m_thread = nullptr;
    m_threaded = false;
}

void Engine::shutdown()
{
    m_audio->stop();
    if (!m_initialized) return;
    stop();
    if (m_quadVao) { // not yet released by the render thread
        ScopedCurrent sc(this);
        releaseAll();
    }
    delete m_context;
    m_context = nullptr;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// OpenGL tasks (runGl)
// ---------------------------------------------------------------------------
void Engine::runGl(std::function<void()> fn, bool wait)
{
    if (!m_threaded) {
        if (QOpenGLContext::currentContext() == m_context) {
            fn();
        } else {
            ScopedCurrent sc(this);
            fn();
        }
        return;
    }
    if (QThread::currentThread() == m_thread) {
        fn();
        return;
    }
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    std::unique_lock<std::mutex> lk(m_taskMutex);
    if (m_renderFailed) return; // the render thread could not start: nothing runs there
    m_tasks.push_back(task);
    if (!wait) return;
    m_taskCv.wait(lk, [&] { return task->done || !m_threaded; });
}

void Engine::runPendingTasks()
{
    std::deque<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        tasks.swap(m_tasks);
    }
    if (tasks.empty()) return;
    for (auto &t : tasks) {
        t->fn();
        t->fn = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        for (auto &t : tasks) t->done = true;
    }
    m_taskCv.notify_all();
}

void Engine::renderLoop()
{
    if (s_failRenderStart.exchange(false) || !m_context->makeCurrent(m_surface)) {
        qCritical("Render thread: unable to make the OpenGL context current.");
        // Nothing will be rendered: the tasks waiting, and those asked from now on, return without running
        {
            std::lock_guard<std::mutex> lk(m_taskMutex);
            m_renderFailed = true;
            for (auto &t : m_tasks) t->done = true;
            m_tasks.clear();
        }
        m_taskCv.notify_all();
        m_context->moveToThread(m_ownerThread);
        tsanRelease(&m_thread);
        return;
    }
    m_currentSurface = m_surface;
    QElapsedTimer pace;
    while (!m_quit) {
        pace.start();
        runPendingTasks();

        // Pace: the frame rate chosen; else the screen of the output window shown (the last one, which waits for its
        // vertical sync); else the main screen's refresh rate. A frame its output does not show (no surface from
        // macOS this time) is not followed by one at another screen's pace.
        const double rate = effectiveRender().frameRate;
        const double hz = rate > 0 ? rate : m_presentHz > 1 ? m_presentHz : screenRefreshRate();
        const qint64 periodNs = qint64(1e9 / std::clamp(hz, 1.0, 1000.0));

        const qint64 t0 = FrameTiming::nowNs();
        frame(nextDt());
        const qint64 t1 = FrameTiming::nowNs();
        const bool presented = presentViewports();
        const qint64 t2 = FrameTiming::nowNs();
        m_frameTiming.noteFrame(t0, t1, t2, periodNs, presented);
        sampleResources();
        if (!m_framePending.exchange(true)) emit frameRendered();

        // The vertical sync held this frame (it waited for its screen): it is the pace, nothing to add. Otherwise the
        // next frame is due one period after this one was (absolute deadlines: a late wake-up is caught up on the next
        // frame instead of adding up; more than a period late, the count starts again from now)
        const bool heldBySync = rate <= 0 && presented && t2 - t1 >= 4000000;
        const qint64 sleepNs = heldBySync ? FramePacer::resync(m_paceNext, t2, periodNs)
                                          : FramePacer::next(m_paceNext, t0, FrameTiming::nowNs(), periodNs);
        if (sleepNs > 0) QThread::usleep(static_cast<unsigned long>(sleepNs / 1000));
        (void)pace;
    }
    runPendingTasks();
    releaseAll();
    {
        // Release any callers still waiting
        std::lock_guard<std::mutex> lk(m_taskMutex);
        for (auto &t : m_tasks) t->done = true;
        m_tasks.clear();
    }
    m_taskCv.notify_all();
    m_context->doneCurrent();
    m_currentSurface = nullptr;
    m_context->moveToThread(m_ownerThread);
    tsanRelease(&m_thread); // what the render thread did is done before stop() goes on
}

void Engine::releaseAll()
{
    m_dstCopy.destroy();
    for (auto &[size, ms] : m_msaa) ms.destroy();
    m_msaa.clear();
    // Output windows: their contexts (the windows themselves belong to the interface)
    for (auto &[id, o] : m_outWindows) releaseOutputSurface(o);
    // Source transitions in progress (render thread: released here at once)
    for (auto &[id, t] : m_transitions) retireTransition(std::move(t));
    m_transitions.clear();
    // Publishers: GPU (current context required), then the send threads
    m_publishing.release();
    {
        Lock lk(&m_mutex);
        for (auto &l : m_layers) releaseLayer(*l);
        m_layers.clear();
    }
    auto f = gl();
    for (RenderTarget &o : m_output) o.destroy();
    m_previewTarget.destroy();
    if (m_previewPbo) f->glDeleteBuffers(1, &m_previewPbo);
    m_previewPbo = 0;
    m_previewPboW = m_previewPboH = 0;
    m_previewPboId = 0;
    {
        std::lock_guard<std::mutex> lk(m_fenceMutex);
        if (m_frameFence) f->glDeleteSync(m_frameFence);
        m_frameFence = nullptr;
    }
    m_videoConv.release();
    m_testCard.release();
    for (GLuint p : {m_blitProgram, m_compProgram, m_diffProgram, m_presentProgram, m_prepProgram, m_maskProgram,
                     m_fadeProgram, m_shapeMaskProgram})
        if (p) f->glDeleteProgram(p);
    GLuint bufs[] = {m_quadVbo, m_meshVbo, m_meshIbo};
    f->glDeleteBuffers(3, bufs);
    GLuint vaos[] = {m_quadVao, m_meshVao};
    f->glDeleteVertexArrays(2, vaos);
    f->glDeleteTextures(1, &m_blackTex);
    m_quadVao = m_meshVao = 0;
    m_blitProgram = m_compProgram = m_diffProgram = m_presentProgram = m_prepProgram = m_maskProgram = 0;
    m_fadeProgram = m_shapeMaskProgram = 0;
}

// ---------------------------------------------------------------------------
// Output window
// ---------------------------------------------------------------------------
void Engine::setViewportWindow(quint64 viewport, QWindow *w)
{
    // Its Metal layer is taken here, on the main thread (AppKit)
    std::shared_ptr<MetalOutput> metal = MetalOutput::forWindow(w);
    runGl([this, viewport, w, metal] {
        auto it = m_outWindows.find(viewport);
        if (it != m_outWindows.end() && it->second.window != w) {
            releaseOutputSurface(it->second); // before the window goes
            m_outWindows.erase(it);
        }
        if (w && !m_outWindows.count(viewport)) {
            OutputSurface o;
            o.window = w;
            o.metal = metal;
            m_outWindows[viewport] = std::move(o);
        }
    });
}

void Engine::setViewportExposed(quint64 viewport, bool exposed, QSize pixelSize, double refreshHz)
{
    runGl([this, viewport, exposed, pixelSize, refreshHz] {
        auto it = m_outWindows.find(viewport);
        if (it == m_outWindows.end()) return;
        it->second.exposed = exposed;
        it->second.pixels = pixelSize;
        if (refreshHz > 1) it->second.hz = refreshHz;
    });
}

// The window's context goes; its vertex array goes with it
void Engine::releaseOutputSurface(OutputSurface &o)
{
    if (o.metal) { // its shared surfaces, with the engine's context current
        m_context->makeCurrent(m_surface);
        m_currentSurface = m_surface;
        o.metal->releaseGl();
        o.metal.reset();
    }
    if (o.context && o.vao && o.window && o.context->makeCurrent(o.window)) {
        o.context->extraFunctions()->glDeleteVertexArrays(1, &o.vao);
        o.context->doneCurrent();
    }
    delete o.context;
    o.context = nullptr;
    o.vao = 0;
    m_context->makeCurrent(m_surface);
    m_currentSurface = m_surface;
}

void Engine::waitForFrame(QOpenGLExtraFunctions *f) const
{
    std::lock_guard<std::mutex> lk(m_fenceMutex);
    if (m_frameFence) f->glWaitSync(m_frameFence, 0, GL_TIMEOUT_IGNORED);
}

// Draws a viewport's last finished picture into its window, with the window's context current
void Engine::present(const Layer &viewport, OutputSurface &out)
{
    auto f = out.context->extraFunctions();
    if (!out.vao) { // the shared quad, seen through this context
        f->glGenVertexArrays(1, &out.vao);
        f->glBindVertexArray(out.vao);
        f->glBindBuffer(GL_ARRAY_BUFFER, m_quadVbo);
        f->glEnableVertexAttribArray(0);
        f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, out.context->defaultFramebufferObject());
    f->glViewport(0, 0, out.pixels.width(), out.pixels.height());
    f->glDisable(GL_BLEND);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    f->glUseProgram(m_presentProgram);
    f->glActiveTexture(GL_TEXTURE0);
    waitForFrame(f);
    f->glBindTexture(GL_TEXTURE_2D, viewport.vpOut[viewport.vpPublished.load()].tex);
    f->glUniform1i(m_presentTexLoc, 0);
    f->glBindVertexArray(out.vao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    f->glBindVertexArray(0);
}

// Vertical sync of the swap to come. With several windows, only the last swap of the frame waits for the
// screen: each one waiting in turn would divide the frame rate by the number of windows.
// macOS: a setting of the context; Windows and Linux: of the window that is current.
static void setSwapInterval(QOpenGLContext *ctx, int n)
{
#ifdef Q_OS_MACOS
    Q_UNUSED(ctx);
    const GLint v = n;
    if (CGLContextObj c = CGLGetCurrentContext()) CGLSetParameter(c, kCGLCPSwapInterval, &v);
#elif defined(Q_OS_WIN)
    using Fn = BOOL(WINAPI *)(int);
    static Fn fn = reinterpret_cast<Fn>(ctx->getProcAddress("wglSwapIntervalEXT"));
    if (fn) fn(n);
#else
    using Fn = int (*)(unsigned);
    static Fn fn = reinterpret_cast<Fn>(ctx->getProcAddress("glXSwapIntervalMESA"));
    if (fn) fn(unsigned(n));
#endif
}

// Each window on screen is drawn with its own context and swapped; the engine's context then becomes current
// again. Each window's context waits on the GPU for the frame's fence (waitForFrame) before drawing it.
bool Engine::presentViewports()
{
    std::vector<std::pair<const Layer *, OutputSurface *>> shown;
    {
        Lock lk(&m_mutex);
        for (const auto &l : m_layers) {
            if (!l->isViewport) continue;
            auto it = m_outWindows.find(l->id);
            if (it != m_outWindows.end() && it->second.window && it->second.exposed) shown.push_back({l.get(), &it->second});
        }
    }
    bool any = false, glUsed = false;
    // The pace of the window that waits for its screen (the last one): the render follows it while it is shown
    m_presentHz = shown.empty() ? 0 : shown.back().second->hz;
    for (size_t k = 0; k < shown.size(); ++k) {
        const Layer *vp = shown[k].first;
        OutputSurface &out = *shown[k].second;
        const bool last = k + 1 == shown.size();
        if (out.metal) { // drawn by Metal from here, with the engine's context current
            const RenderTarget &pic = vp->vpOut[vp->vpPublished.load()];
            switch (out.metal->present(pic.fbo, pic.w, pic.h, out.pixels, last)) {
            case MetalOutput::Result::Shown:
                any = true;
                ++out.presented;
                break;
            case MetalOutput::Result::NoDrawable: ++out.noDrawable; break;
            case MetalOutput::Result::SurfacesBusy: ++out.surfacesBusy; break;
            case MetalOutput::Result::NoPicture: ++out.noPicture; break;
            case MetalOutput::Result::Failed: ++out.failed; break;
            }
            const quint64 dropped = out.metal->droppedFrames();
            if (dropped > out.dropped) m_outputsDropped += dropped - out.dropped;
            out.dropped = dropped;
            continue;
        }
#ifdef Q_OS_MACOS
        if (m_threaded) { // OpenGL into a window: the main thread only (no Metal layer for this window)
            ++out.notRenderThread;
            continue;
        }
#endif
        glUsed = true;
        if (!out.context) {
            out.context = new QOpenGLContext;
            out.context->setShareContext(m_context);
            out.context->setFormat(m_context->format());
            if (!out.context->create()) {
                qWarning("Output window: unable to create its OpenGL context.");
                delete out.context;
                out.context = nullptr;
                ++out.failed;
                continue;
            }
        }
        if (!out.context->makeCurrent(out.window)) {
            ++out.failed;
            continue;
        }
        present(*vp, out);
        setSwapInterval(out.context, last ? 1 : 0); // only the last one waits for the screen
        out.context->swapBuffers(out.window);
        any = true;
        ++out.presented;
    }
    if (glUsed) {
        m_context->makeCurrent(m_surface);
        m_currentSurface = m_surface;
    }
    return any;
}

quint64 Engine::windowFrames(quint64 viewport)
{
    quint64 n = 0;
    runGl([this, viewport, &n] {
        const auto it = m_outWindows.find(viewport);
        if (it != m_outWindows.end()) n = it->second.presented;
    });
    return n;
}

GLuint Engine::outputTexture() const { return m_output[m_published.load()].tex; }

// ---------------------------------------------------------------------------
// Composition size, opacity and blackout, source preview
// ---------------------------------------------------------------------------
QSize Engine::compositionSize() const
{
    Lock lk(&m_mutex);
    return m_compSize;
}

void Engine::setCompositionSize(QSize s)
{
    s = s.expandedTo(QSize(16, 16)).boundedTo(QSize(16384, 16384));
    {
        Lock lk(&m_mutex);
        if (s == m_compSize) return;
        m_compSize = s;
        for (auto &l : m_layers) l->mapping.aspect = double(s.width()) / s.height(); // their rotations are in its pixels
    }
    emit compositionSizeChanged(s);
}

void Engine::updateTimeScale() { timeScale() = m_paused.load() ? 0.0 : m_compositionSpeed.load(); }

void Engine::setCompositionSpeed(double speed)
{
    m_compositionSpeed = std::clamp(speed, 0.0, 10.0);
    updateTimeScale();
}

void Engine::setPaused(bool on)
{
    m_paused = on;
    updateTimeScale();
}

double Engine::compositionOpacityTarget() const
{
    Lock lk(&m_mutex);
    return m_compositionOpacityTarget;
}

void Engine::setAudioVolume(float v)
{
    {
        Lock lk(&m_mutex);
        m_compFade.volume = false;
    }
    m_audio->setVolume(v);
}

void Engine::fadeCompositionOpacity(double target, double seconds)
{
    Lock lk(&m_mutex);
    m_compFade.opacity = false; // the fader takes over from a snapshot's fade
    m_compositionOpacityTarget = std::clamp(target, 0.0, 1.0);
    m_compositionOpacitySpeed = seconds > 0.0 ? 1.0 / seconds : 0.0;
}

void Engine::setBlackout(bool on, double seconds)
{
    {
        Lock lk(&m_mutex);
        m_blackTarget = on ? 0.0 : 1.0;
        m_blackSpeed = seconds > 0.0 ? 1.0 / seconds : 0.0;
        if (m_blackSpeed <= 0.0) m_blackLevel = m_blackTarget;
    }
    m_audio->fadeTo(on ? 0.0f : 1.0f, seconds);
}

bool Engine::blackout() const
{
    Lock lk(&m_mutex);
    return m_blackTarget < 0.5;
}

void Engine::setBlackoutFade(double seconds)
{
    Lock lk(&m_mutex);
    m_blackFade = std::clamp(seconds, 0.0, 60.0);
}

double Engine::blackoutFade() const
{
    Lock lk(&m_mutex);
    return m_blackFade;
}

void Engine::requestSourcePreview(quint64 layerId, int maxSide)
{
    Lock lk(&m_mutex);
    m_previewId = layerId;
    m_previewSide = std::clamp(maxSide, 16, 2048);
}

QImage Engine::sourcePreview(quint64 *layerId) const
{
    std::lock_guard<std::mutex> lk(m_previewMutex);
    if (layerId) *layerId = m_previewImageId;
    return m_previewImage;
}

// ---------------------------------------------------------------------------
// Rendering settings
// ---------------------------------------------------------------------------
void Engine::setRenderSettings(const RenderSettings &r)
{
    Lock lk(&m_mutex);
    m_render = r;
}

Engine::RenderSettings Engine::renderSettings() const
{
    Lock lk(&m_mutex);
    return m_render;
}

void Engine::setRenderDefaults(const RenderSettings &r)
{
    Lock lk(&m_mutex);
    m_renderDefaults = r;
}

Engine::RenderSettings Engine::renderDefaults() const
{
    Lock lk(&m_mutex);
    return m_renderDefaults;
}

Engine::RenderSettings Engine::effectiveRender() const
{
    Lock lk(&m_mutex);
    RenderSettings e;
    e.frameRate = m_render.frameRate >= 0 ? m_render.frameRate : std::max(0.0, m_renderDefaults.frameRate);
    e.samples = m_render.samples >= 0 ? m_render.samples : std::max(0, m_renderDefaults.samples);
    e.mipmaps = m_render.mipmaps >= 0 ? m_render.mipmaps : std::max(0, m_renderDefaults.mipmaps);
    // RGBA16 UNORM ("10") unless 8 bits are chosen: smooth generated gradients by default, half the GPU memory and
    // bandwidth on request (heavy 4K / 8K shows)
    e.depth = m_render.depth > 0 ? m_render.depth : (m_renderDefaults.depth > 0 ? m_renderDefaults.depth : 10);
    return e;
}

void Engine::setScreenRefreshRate(double hz)
{
    Lock lk(&m_mutex);
    m_screenHz = hz > 1 ? hz : 60.0;
}

double Engine::screenRefreshRate() const
{
    Lock lk(&m_mutex);
    return m_screenHz;
}

// ---------------------------------------------------------------------------
// Resources against their budgets (Engine::resourcesJson)
// ---------------------------------------------------------------------------
#ifndef GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif

void Engine::sampleResources()
{
    const qint64 now = FrameTiming::nowNs();
    if (m_resourcesSampledNs && now - m_resourcesSampledNs < 1000000000) return;
    m_resourcesSampledNs = now;
    { // The output windows, for frameTimingJson (render thread: m_outWindows is its own)
        std::vector<OutputStats> outs;
        Lock lk(&m_mutex);
        for (const auto &[id, o] : m_outWindows) {
            OutputStats st;
            st.viewport = id;
            const int i = indexOfId(id);
            if (i >= 0) st.name = m_layers[size_t(i)]->name;
            st.metal = bool(o.metal);
            st.exposed = o.exposed;
            st.hz = o.hz;
            st.presented = o.presented;
            st.noDrawable = o.noDrawable;
            st.surfacesBusy = o.surfacesBusy;
            st.failed = o.failed;
            st.noPicture = o.noPicture;
            st.notRenderThread = o.notRenderThread;
            outs.push_back(st);
        }
        std::lock_guard<std::mutex> g(m_outputStatsMutex);
        m_outputStats = std::move(outs);
    }
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (ctx) {
        if (m_vramQuery < 0)
            m_vramQuery = ctx->hasExtension("GL_NVX_gpu_memory_info") ? 1 : ctx->hasExtension("GL_ATI_meminfo") ? 2 : 0;
        auto f = ctx->extraFunctions();
        if (m_vramQuery == 1) {
            GLint total = 0, free = 0;
            f->glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
            f->glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &free);
            m_vramTotalKb = total;
            m_vramFreeKb = free;
        } else if (m_vramQuery == 2) {
            GLint info[4] = {0, 0, 0, 0}; // free, largest block, auxiliary free, largest auxiliary block (kB)
            f->glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, info);
            m_vramFreeKb = info[0];
        }
    }
    quint32 over = 0;
    if (VideoDecoder::decodedBytesInUse() > VideoDecoder::decodedBytesLimit()) over |= 1;
    if (VideoTexture::pboBytesInUse() > VideoTexture::pboBudgetBytes()) over |= 2;
    if (VideoDecoder::softwareThreadsInUse() > VideoDecoder::softwareThreadBudgetNow()) over |= 4;
    const qint64 freeKb = m_vramFreeKb.load(), totalKb = m_vramTotalKb.load();
    if (freeKb >= 0 && ((totalKb > 0 && freeKb * 10 < totalKb) || (totalKb <= 0 && freeKb < 256 * 1024))) over |= 8;
    const quint32 before = m_overBudget.exchange(over);
    static const char *const what[] = {"decoded frames over their memory budget (each stream keeps 2 ahead)",
                                       "upload buffers over their budget (shared among the layers)",
                                       "software decoders' threads over the budget (new ones share it)",
                                       "graphics memory nearly full"};
    for (int k = 0; k < 4; ++k)
        if ((over & ~before) >> k & 1u) qWarning("Resources: %s", what[k]);
}

QJsonObject Engine::resourcesJson() const
{
    auto mb = [](size_t bytes) { return std::round(double(bytes) / (1024.0 * 1024.0) * 10.0) / 10.0; };
    QJsonObject o;
    o["decoded_frames_mb"] = mb(VideoDecoder::decodedBytesInUse());
    o["decoded_frames_budget_mb"] = mb(VideoDecoder::decodedBytesLimit());
    o["upload_buffers_mb"] = mb(VideoTexture::pboBytesInUse());
    o["upload_buffers_budget_mb"] = mb(VideoTexture::pboBudgetBytes());
    o["upload_buffer_layers"] = VideoTexture::pboStreams();
    o["software_decoders"] = VideoDecoder::softwareDecodersOpen();
    o["software_decode_threads"] = VideoDecoder::softwareThreadsInUse();
    o["software_decode_thread_budget"] = VideoDecoder::softwareThreadBudgetNow();
    o["hap_workers"] = workers::threadCount();
    if (m_vramFreeKb.load() >= 0) o["graphics_memory_free_mb"] = double(m_vramFreeKb.load()) / 1024.0;
    if (m_vramTotalKb.load() > 0) o["graphics_memory_total_mb"] = double(m_vramTotalKb.load()) / 1024.0;
    if (const uint64_t rss = processMemory::residentBytes()) o["process_resident_mb"] = mb(size_t(rss)); // all of it
    QJsonArray warnings;
    static const char *const keys[] = {"decoded_frames", "upload_buffers", "decode_threads", "graphics_memory"};
    const quint32 over = m_overBudget.load();
    for (int k = 0; k < 4; ++k)
        if (over >> k & 1u) warnings.append(QString::fromLatin1(keys[k]));
    o["warnings"] = warnings;
    return o;
}
