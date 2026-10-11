#include "VideoSurface.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif

namespace videosurface {

bool available() { return CGLGetCurrentContext() != nullptr; }

bool bindPlane(const AVFrame *hw, int plane, GLuint tex, GLenum internal, GLenum format, GLenum type, int w, int h)
{
    // VideoToolbox's frames: a CVPixelBuffer in data[3], backed by an IOSurface
    auto pixels = reinterpret_cast<CVPixelBufferRef>(hw->data[3]);
    if (!pixels) return false;
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixels);
    CGLContextObj ctx = CGLGetCurrentContext();
    if (!surface || !ctx) return false;
    // The surface's own plane size (rounded up chroma planes): the layout's when they agree
    if (int(IOSurfaceGetPlaneCount(surface)) > 0) {
        if (size_t(plane) >= IOSurfaceGetPlaneCount(surface)) return false;
        if (int(IOSurfaceGetWidthOfPlane(surface, size_t(plane))) != w || int(IOSurfaceGetHeightOfPlane(surface, size_t(plane))) != h)
            return false;
    } else if (plane != 0) {
        return false;
    }
    auto f = gl();
    f->glBindTexture(GL_TEXTURE_RECTANGLE, tex);
    const CGLError e = CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE, internal, w, h, format, type, surface, GLuint(plane));
    f->glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    return e == kCGLNoError;
}

} // namespace videosurface
