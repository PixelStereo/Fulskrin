// macOS output windows through Metal (PresentMetal.h)
#include "PresentMetal.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QThread>
#include <QWindow>
#include <atomic>

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>

#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

namespace {
id<MTLDevice> sharedDevice()
{
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device;
}

// A fullscreen triangle sampling the picture; its alpha is dropped (a window shows no transparency)
const char *kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) {
    float2 p = float2((id << 1) & 2, id & 2);
    V o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
)";
} // namespace

// One surface shared by OpenGL (written) and Metal (read); three in turn, so that OpenGL never writes the one Metal
// is still reading
struct Slot {
    IOSurfaceRef surface = nullptr;
    GLuint tex = 0, fbo = 0;
    id<MTLTexture> mtl = nil;
    int w = 0, h = 0;
    std::atomic<bool> busy{false};
};

struct MetalOutput::D {
    CAMetalLayer *layer = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSamplerState> sampler = nil;
    Slot ring[3];
    int next = 0;
    QSize drawable;
    bool warned = false;
    void warnOnce(const char *what)
    {
        if (!warned) qWarning("Metal output: %s", what);
        warned = true;
    }

    void waitIdle(Slot &s)
    {
        for (int k = 0; s.busy.load() && k < 2000; ++k) QThread::usleep(50); // 100 ms at most
    }
    void releaseSlot(Slot &s, QOpenGLExtraFunctions *f)
    {
        waitIdle(s);
        if (f) {
            if (s.fbo) f->glDeleteFramebuffers(1, &s.fbo);
            if (s.tex) f->glDeleteTextures(1, &s.tex);
        }
        s.fbo = s.tex = 0;
        s.mtl = nil;
        if (s.surface) CFRelease(s.surface);
        s.surface = nullptr;
        s.w = s.h = 0;
    }
    bool ensure(Slot &s, int w, int h, QOpenGLExtraFunctions *f)
    {
        if (s.surface && s.w == w && s.h == h) return true;
        releaseSlot(s, f);
        NSDictionary *props = @{
            (id)kIOSurfaceWidth: @(w),
            (id)kIOSurfaceHeight: @(h),
            (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
        };
        s.surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        if (!s.surface) {
            warnOnce("IOSurfaceCreate failed");
            return false;
        }
        f->glGenTextures(1, &s.tex);
        f->glBindTexture(GL_TEXTURE_RECTANGLE, s.tex);
        if (CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA, GLsizei(w), GLsizei(h), GL_BGRA,
                                   GL_UNSIGNED_INT_8_8_8_8_REV, s.surface, 0) != kCGLNoError) {
            warnOnce("CGLTexImageIOSurface2D failed");
            f->glBindTexture(GL_TEXTURE_RECTANGLE, 0);
            releaseSlot(s, f);
            return false;
        }
        f->glBindTexture(GL_TEXTURE_RECTANGLE, 0);
        f->glGenFramebuffers(1, &s.fbo);
        f->glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
        f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, s.tex, 0);
        const bool complete = f->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:NSUInteger(w)
                                                                                     height:NSUInteger(h)
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s.mtl = [sharedDevice() newTextureWithDescriptor:td iosurface:s.surface plane:0];
        if (!complete || !s.mtl) {
            warnOnce(!complete ? "the shared surface's framebuffer is incomplete" : "no Metal texture on the shared surface");
            releaseSlot(s, f);
            return false;
        }
        s.w = w;
        s.h = h;
        return true;
    }
};

bool MetalOutput::available() { return sharedDevice() != nil; }

MetalOutput::MetalOutput() : d(std::make_unique<D>()) {}

MetalOutput::~MetalOutput()
{
    for (Slot &s : d->ring) d->releaseSlot(s, nullptr); // the OpenGL side went with releaseGl
}

std::shared_ptr<MetalOutput> MetalOutput::forWindow(QWindow *w)
{
    id<MTLDevice> device = sharedDevice();
    if (!w || !device || w->surfaceType() != QSurface::MetalSurface) return nullptr;
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(w->winId());
    if (!view || ![view.layer isKindOfClass:[CAMetalLayer class]]) {
        qWarning("Metal output: the window has no Metal layer (%s)", view ? view.layer.className.UTF8String : "no view");
        return nullptr;
    }
    std::shared_ptr<MetalOutput> out(new MetalOutput);
    D &d = *out->d;
    d.layer = (CAMetalLayer *)view.layer;
    d.layer.device = device;
    d.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.layer.framebufferOnly = YES;
    d.queue = [device newCommandQueue];
    NSError *error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(kShader) options:nil error:&error];
    if (!lib) {
        qWarning("Metal output: %s", error.localizedDescription.UTF8String);
        return nullptr;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!d.pipeline) {
        qWarning("Metal output: %s", error.localizedDescription.UTF8String);
        return nullptr;
    }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    d.sampler = [device newSamplerStateWithDescriptor:sd];
    return out;
}

bool MetalOutput::present(unsigned framebuffer, int w, int h, QSize pixels, bool waitForScreen)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx || !framebuffer || w <= 0 || h <= 0 || pixels.isEmpty()) return false;
    auto *f = ctx->extraFunctions();
    @autoreleasepool {
        Slot &s = d->ring[d->next];
        d->waitIdle(s);
        if (!d->ensure(s, w, h, f)) return false;
        d->next = (d->next + 1) % 3;
        // OpenGL: the picture into the shared surface, turned upside down (OpenGL's rows go up, the surface's down)
        f->glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
        f->glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        f->glFlush(); // written before Metal reads it
        // Metal: the surface stretched over the window
        if (d->drawable != pixels) {
            d->layer.drawableSize = CGSizeMake(pixels.width(), pixels.height());
            d->drawable = pixels;
        }
        d->layer.displaySyncEnabled = waitForScreen ? YES : NO;
        id<CAMetalDrawable> drawable = [d->layer nextDrawable];
        if (!drawable) {
            d->warnOnce("no drawable from the window's layer");
            return false;
        }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cmd = [d->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:d->pipeline];
        [enc setFragmentTexture:s.mtl atIndex:0];
        [enc setFragmentSamplerState:d->sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        s.busy = true;
        Slot *slot = &s;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) { slot->busy = false; }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
    return true;
}

void MetalOutput::releaseGl()
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    for (Slot &s : d->ring) d->releaseSlot(s, ctx ? ctx->extraFunctions() : nullptr);
}
