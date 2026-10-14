// Test that failed framebuffer allocations never retire a working GPU image.
// This exercises the real GL allocation path and simulates an incomplete FBO
// after the replacement objects are created.
#include "Engine.h"
#include "Gl.h"
#include <QImage>
#include <cstdio>

int runGpuTargetTests(Engine &e)
{
    int failures = 0;
    auto check = [&](bool condition, const char *message) {
        if (condition) std::printf("ok GPU target: %s\n", message);
        else {
            std::printf("FAIL GPU target: %s\n", message);
            ++failures;
        }
    };

    e.runGl([&] {
        RenderTarget t;
        check(t.ensure(32, 32) && t.fbo && t.tex && t.w == 32, "initial framebuffer complete");
        const GLuint previousFbo = t.fbo, previousTex = t.tex;
        failNextRenderTargetAllocationForTests();
        check(!t.ensure(64, 64) && t.fbo == previousFbo && t.tex == previousTex && t.w == 32 && t.h == 32,
              "failed replacement preserves the existing image and dimensions");
        t.bind();
        check(gl()->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "retained framebuffer remains complete");
        check(t.ensure(64, 64) && t.fbo && t.tex && t.w == 64 && t.h == 64,
              "retry allocates a new complete framebuffer");
        t.destroy();

        GLint maximum = 0;
        gl()->glGetIntegerv(GL_MAX_SAMPLES, &maximum);
        if (maximum >= 2) {
            MsaaBuffer ms;
            ms.ensure(32, 32, 2);
            check(ms.fbo && ms.rbo, "initial multisample target complete");
            const GLuint beforeFbo = ms.fbo, beforeRbo = ms.rbo;
            failNextMsaaAllocationForTests();
            ms.ensure(64, 64, 2);
            check(ms.fbo == beforeFbo && ms.rbo == beforeRbo && ms.w == 32 && ms.h == 32,
                  "failed multisample replacement preserves the previous buffer");
            ms.ensure(64, 64, 2);
            check(ms.fbo && ms.rbo && ms.w == 64 && ms.h == 64,
                  "multisample allocation recovers on retry");
            ms.destroy();
        } else std::printf("skip GPU target: driver has no multisample support\n");
        gl()->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    });

    // Source-grab is a one-off GPU readback. The target is allocated inside
    // grabLayerSource(), so injecting its failure is deterministic (unlike
    // failing a target somewhere in the middle of the whole scene render).
    e.newProject();
    e.setCompositionSize(QSize(64, 36));
    const int layer = e.addLayer(QStringLiteral("Green test input"));
    QString err;
    if (e.setLayerInput(layer, QStringLiteral("input:test:0,255,0"), &err)) {
        for (int k = 0; k < 3; ++k) e.renderFrame();
        const QImage before = e.grabLayerSource(layer);
        check(!before.isNull() && before.pixelColor(before.width() / 2, before.height() / 2).green() > 240,
              "source grab succeeds with a valid intermediate target");

        failNextRenderTargetAllocationForTests();
        const QImage unavailable = e.grabLayerSource(layer);
        check(unavailable.isNull(), "source grab fails safely (no default framebuffer readback)");

        const QImage recovered = e.grabLayerSource(layer);
        check(!recovered.isNull() && recovered.pixelColor(recovered.width() / 2, recovered.height() / 2).green() > 240,
              "source grab recovers after one failed target allocation");
    } else {
        std::printf("FAIL GPU target: could not open synthetic test input: %s\n", qPrintable(err));
        ++failures;
    }
    e.newProject();
    return failures;
}
