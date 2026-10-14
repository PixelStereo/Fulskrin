#include <atomic>
#include "Gl.h"
#include <QByteArray>
#include <QDebug>

static GLuint compileStage(GLenum type, const QString &src, QString *log)
{
    auto f = gl();
    GLuint s = f->glCreateShader(type);
    QByteArray utf8 = src.toUtf8();
    const char *p = utf8.constData();
    f->glShaderSource(s, 1, &p, nullptr);
    f->glCompileShader(s);
    GLint ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        f->glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        QByteArray b(qMax(len, 1) + 1, 0);
        f->glGetShaderInfoLog(s, len, nullptr, b.data());
        if (log)
            *log += (type == GL_VERTEX_SHADER ? QStringLiteral("[vertex] ") : QStringLiteral("[fragment] "))
                    + QString::fromUtf8(b).trimmed() + '\n';
        f->glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint compileProgram(const QString &vs, const QString &fs, QString *log)
{
    auto f = gl();
    GLuint v = compileStage(GL_VERTEX_SHADER, vs, log);
    GLuint fr = compileStage(GL_FRAGMENT_SHADER, fs, log);
    if (!v || !fr) {
        if (v) f->glDeleteShader(v);
        if (fr) f->glDeleteShader(fr);
        return 0;
    }
    GLuint p = f->glCreateProgram();
    f->glAttachShader(p, v);
    f->glAttachShader(p, fr);
    f->glLinkProgram(p);
    f->glDeleteShader(v);
    f->glDeleteShader(fr);
    GLint ok = 0;
    f->glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        f->glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        QByteArray b(qMax(len, 1) + 1, 0);
        f->glGetProgramInfoLog(p, len, nullptr, b.data());
        if (log) *log += QStringLiteral("[link] ") + QString::fromUtf8(b).trimmed() + '\n';
        f->glDeleteProgram(p);
        return 0;
    }
    return p;
}

static std::atomic<int> g_renderBits{8};
static std::atomic<bool> g_failTarget{false}, g_failMsaa{false};
void failNextRenderTargetAllocationForTests() { g_failTarget = true; }
void failNextMsaaAllocationForTests() { g_failMsaa = true; }

void setRenderBits(int bits) { g_renderBits = bits > 8 ? 16 : 8; }
int renderBits() { return g_renderBits; }

bool RenderTarget::ensure(int nw, int nh, bool flt)
{
    const int wantBits = flt ? 32 : g_renderBits.load();
    nw = qMax(1, nw);
    nh = qMax(1, nh);
    if (fbo && nw == w && nh == h && flt == isFloat && wantBits == bits)
        return false;
    auto f = gl();
    GLuint nextTex = 0, nextFbo = 0;
    f->glGenTextures(1, &nextTex);
    f->glBindTexture(GL_TEXTURE_2D, nextTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, flt ? GL_RGBA32F : wantBits > 8 ? GL_RGBA16 : GL_RGBA8, nw, nh, 0, GL_RGBA,
                    flt ? GL_FLOAT : wantBits > 8 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glGenFramebuffers(1, &nextFbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, nextFbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, nextTex, 0);
    const GLenum st = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const bool forced = g_failTarget.exchange(false);
    if (st != GL_FRAMEBUFFER_COMPLETE || forced) {
        qWarning() << "RenderTarget allocation rejected; keeping previous picture:" << Qt::hex << st << nw << nh << flt;
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (nextFbo) f->glDeleteFramebuffers(1, &nextFbo);
        if (nextTex) f->glDeleteTextures(1, &nextTex);
        return false; // the previous framebuffer, texture, size and precision are intact
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    destroy(); // only after the replacement has been validated
    fbo = nextFbo;
    tex = nextTex;
    w = nw;
    h = nh;
    isFloat = flt;
    bits = wantBits;
    clear();
    return true; // a new, complete target
}

void RenderTarget::bind() const
{
    auto f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f->glViewport(0, 0, w, h);
}

void RenderTarget::clear(float r, float g, float b, float a) const
{
    auto f = gl();
    bind();
    f->glClearColor(r, g, b, a);
    f->glClear(GL_COLOR_BUFFER_BIT);
}

void RenderTarget::destroy()
{
    auto f = gl();
    if (fbo) f->glDeleteFramebuffers(1, &fbo);
    if (tex) f->glDeleteTextures(1, &tex);
    fbo = tex = 0;
    w = h = 0;
}

void MsaaBuffer::ensure(int nw, int nh, int s)
{
    const int wantBits = g_renderBits.load();
    if (fbo && nw == w && nh == h && s == samples && wantBits == bits) return;
    auto f = gl();
    GLuint nextRbo = 0, nextFbo = 0;
    f->glGenRenderbuffers(1, &nextRbo);
    f->glBindRenderbuffer(GL_RENDERBUFFER, nextRbo);
    f->glRenderbufferStorageMultisample(GL_RENDERBUFFER, s, wantBits > 8 ? GL_RGBA16 : GL_RGBA8, nw, nh);
    f->glGenFramebuffers(1, &nextFbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, nextFbo);
    f->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, nextRbo);
    const GLenum st = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const bool forced = g_failMsaa.exchange(false);
    if (st != GL_FRAMEBUFFER_COMPLETE || forced) {
        qWarning() << "MSAA allocation rejected; keeping previous buffer:" << Qt::hex << st << nw << nh << s;
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (nextFbo) f->glDeleteFramebuffers(1, &nextFbo);
        if (nextRbo) f->glDeleteRenderbuffers(1, &nextRbo);
        return;
    }
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    destroy();
    fbo = nextFbo;
    rbo = nextRbo;
    w = nw;
    h = nh;
    samples = s;
    bits = wantBits;
}

void MsaaBuffer::resolveInto(const RenderTarget &target) const
{
    auto f = gl();
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fbo);
    f->glBlitFramebuffer(0, 0, w, h, 0, 0, target.w, target.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    f->glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
}

void MsaaBuffer::destroy()
{
    auto f = gl();
    if (fbo) f->glDeleteFramebuffers(1, &fbo);
    if (rbo) f->glDeleteRenderbuffers(1, &rbo);
    fbo = rbo = 0;
    w = h = samples = 0;
}

void Texture2D::upload(const void *rgba, int nw, int nh)
{
    auto f = gl();
    if (!tex) {
        f->glGenTextures(1, &tex);
        f->glBindTexture(GL_TEXTURE_2D, tex);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (nw != w || nh != h) {
        w = nw; h = nh;
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

void Texture2D::destroy()
{
    if (tex) gl()->glDeleteTextures(1, &tex);
    tex = 0;
    w = h = 0;
}
