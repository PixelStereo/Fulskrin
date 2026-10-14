#pragma once
// Small OpenGL building blocks shared by the whole engine.
// The engine targets OpenGL 3.3 core (macOS provides 4.1 core, Windows/Linux 3.3+).

#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QString>

#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

inline QOpenGLExtraFunctions *gl() { return QOpenGLContext::currentContext()->extraFunctions(); }

// Upside down: images (top row first) to and from textures (bottom row first). QImage::mirrored is deprecated
// since Qt 6.13, flipped exists since Qt 6.9.
inline QImage flippedVertically(const QImage &img)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    return img.flipped(Qt::Vertical);
#else
    return img.mirrored(false, true);
#endif
}

// Compiles and links a program. Returns 0 on failure (log filled in).
GLuint compileProgram(const QString &vs, const QString &fs, QString *log);

// Texture + render framebuffer.
// Precision of the render targets (the layers, groups, viewports and the composition): 8 bits per channel, or 10
// (kept as 16-bit normalized, so the alpha keeps its full range). A change takes effect as targets are next sized.
void setRenderBits(int bits);
int renderBits();

// Regression-test fault injection. The next allocation fails *after* the new
// GL objects exist, validating that the previous target is never destroyed.
void failNextRenderTargetAllocationForTests();
void failNextMsaaAllocationForTests();

struct RenderTarget {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
    bool isFloat = false;
    int bits = 8; // 8 or 16 (the 10-bit choice); float targets are 32
    // (Re)creates if the size or format changes. Returns true if recreated (contents cleared).
    bool ensure(int w, int h, bool isFloat = false);
    void bind() const;
    void clear(float r = 0, float g = 0, float b = 0, float a = 0) const;
    void destroy();
};

// Multisampled framebuffer (antialiasing): drawn into, then resolved into a RenderTarget of the same size.
struct MsaaBuffer {
    GLuint fbo = 0, rbo = 0;
    int w = 0, h = 0, samples = 0, bits = 8;
    bool used = false; // this frame (unused ones are released)
    void ensure(int w, int h, int samples);
    void resolveInto(const RenderTarget &target) const;
    void destroy();
};

// Simple texture fed from the CPU (video, images).
struct Texture2D {
    GLuint tex = 0;
    int w = 0, h = 0;
    void upload(const void *rgba, int w, int h);
    void destroy();
};
