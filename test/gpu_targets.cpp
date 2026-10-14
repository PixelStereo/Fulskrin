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
        check(t.ready(32, 32) && !t.ready(64, 64),
              "the previous framebuffer remains valid only for its original dimensions");
        t.bind();
        check(gl()->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "retained framebuffer remains complete");
        check(t.ensure(64, 64) && t.fbo && t.tex && t.w == 64 && t.h == 64,
              "retry allocates a new complete framebuffer");
        check(t.ready(64, 64), "new framebuffer has the requested dimensions and color depth");
        setRenderBits(16);
        check(!t.ready(64, 64), "an 8-bit FBO is not a valid 16-bit replacement");
        check(t.ensure(64, 64) && t.ready(64, 64), "framebuffer upgrades precision on next successful allocation");
        setRenderBits(8);
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
    // A preprocessing target is required for ROI/color transformations.
    // If allocating it fails, showing the unprocessed source would bypass
    // show-critical crop/color settings. This scene must fail closed.
    e.newProject();
    e.setCompositionSize(QSize(64, 36));
    e.fadeCompositionOpacity(1.0, 0.0);
    e.setBlackout(false, 0.0);
    const int adjusted = e.addLayer(QStringLiteral("ROI allocation fault"));
    if (e.setLayerInput(adjusted, QStringLiteral("input:test:0,255,0"), &err)) {
        for (int k = 0; k < 3; ++k) e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            e.layer(adjusted)->roi = QRectF(0, 0, 0.5, 1);
        }
        failNextRenderTargetAllocationForTests();
        e.renderFrame();
        const QImage rejected = e.grabOutput();
        check(!rejected.isNull() && rejected.pixelColor(rejected.width() / 2, rejected.height() / 2).green() < 5,
              "failed ROI/prep allocation never displays the unprocessed media");
        {
            Engine::Lock lk(&e.mutex());
            check(e.layer(adjusted)->preFxTex == 0,
                  "failed ROI/prep allocation leaves no stale pre-effects picture for layers that read it");
        }
        e.renderFrame();
        const QImage restored = e.grabOutput();
        check(!restored.isNull() && restored.pixelColor(restored.width() / 2, restored.height() / 2).green() > 240,
              "ROI/prep allocation recovers on the next frame");
    } else {
        std::printf("FAIL GPU target: could not set up ROI synthetic input: %s\n", qPrintable(err));
        ++failures;
    }
    e.newProject();
    return failures;
}
