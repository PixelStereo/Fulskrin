// GPU preview regression: independent layer/viewport sources, bounded copies, deletion and source switching.
#include "Engine.h"
#include "Gl.h"
#include <QImage>
#include <cstdio>
int runPreviewSourceTests(Engine &e, const QString &tmp)
{
    int failures = 0;
    auto check = [&](bool ok, const char *message) { if (!ok) { ++failures; std::printf("FAIL preview_sources: %s\n", message); } };
    e.newProject();
    e.setCompositionSize(QSize(64, 32));
    const quint64 viewport = e.mainViewportId();
    e.setViewportSize(e.indexOfId(viewport), QSize(64, 32));
    QImage image(2048, 1024, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    const QString path = tmp + "/preview-red.png";
    check(image.save(path), "save image");
    const int index = e.addLayer("Red");
    QString error;
    check(e.setLayerImage(index, path, &error), "load image");
    const quint64 id = e.layerId(index);
    e.setPreviewSource(0, id);
    e.setPreviewSource(1, viewport);
    e.setPreviewSource(2, 0);
    e.setPreviewSource(3, id);
    e.renderFrame();
    e.runGl([&] {
        Engine::Lock lock(&e.mutex());
        auto *f = gl();
        for (int slot = 0; slot < 4; ++slot) {
            QSize size;
            const GLuint texture = e.previewTexture(slot, &size);
            check(texture && !size.isEmpty(), "all four sources published");
            check(size.width() <= 1024 && size.height() <= 1024, "bounded preview dimensions");
            GLuint framebuffer = 0;
            f->glGenFramebuffers(1, &framebuffer);
            f->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            unsigned char pixel[4] = {};
            f->glReadPixels(size.width()/2, size.height()/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            check(pixel[0] > 240 && pixel[1] < 10 && pixel[2] < 10, "selected source has red pixels");
            f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
            f->glDeleteFramebuffers(1, &framebuffer);
        }
    });
    e.setPreviewSource(0, 999999);
    e.runGl([&] { Engine::Lock lock(&e.mutex()); check(!e.previewTexture(0, nullptr), "source switch never shows previous source"); });
    e.renderFrame();
    e.runGl([&] { Engine::Lock lock(&e.mutex()); check(!e.previewTexture(0, nullptr), "missing source stays empty"); });
    e.removeLayer(e.indexOfId(id));
    e.renderFrame();
    e.runGl([&] { Engine::Lock lock(&e.mutex()); check(!e.previewTexture(3, nullptr), "deleted layer never leaves a stale image"); });
    for (int slot = 0; slot < 4; ++slot) e.setPreviewSource(slot, 0);
    e.newProject();
    return failures;
}
