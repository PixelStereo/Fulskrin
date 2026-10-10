#pragma once
// macOS: the output windows shown through Metal. OpenGL may only draw into a window on the main thread there (AppKit),
// which would tie the frames to the interface; the engine renders on its own thread into its framebuffers instead,
// and each window's picture goes to it through a shared surface (IOSurface, no copy): OpenGL writes it, Metal draws it
// into the window's CAMetalLayer, from the render thread.
//
// Elsewhere (and on a Mac without Metal) there is none of this: the windows are drawn with OpenGL.

#include <QSize>
#include <QtGlobal>
#include <memory>

class QWindow;

class MetalOutput
{
public:
#ifdef Q_OS_MACOS
    static bool available(); // a Metal device
    // The window's CAMetalLayer (a window whose surface type is Metal), main thread; null: it has none
    static std::shared_ptr<MetalOutput> forWindow(QWindow *w);
    ~MetalOutput();
    // Render thread, the engine's OpenGL context current: the framebuffer's picture (w × h) into the window, stretched
    // over its pixels. waitForScreen: this one is synchronised with the display (the last window of the frame).
    bool present(unsigned framebuffer, int w, int h, QSize pixels, bool waitForScreen);
    void releaseGl(); // its surfaces' OpenGL side (render thread, context current), before it goes

private:
    MetalOutput();
    struct D;
    std::unique_ptr<D> d;
#else
    static bool available() { return false; }
    static std::shared_ptr<MetalOutput> forWindow(QWindow *) { return nullptr; }
    bool present(unsigned, int, int, QSize, bool) { return false; }
    void releaseGl() {}
#endif
};
