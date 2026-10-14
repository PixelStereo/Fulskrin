// Golden-render tests compare finished viewport pixels with independently
// specified expected colors. They must never learn a new reference from PINOL.
#include "Engine.h"
#include "Isf.h"
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QColor>
#include <cmath>
#include <cstdio>

namespace {
constexpr int kWidth = 128;
constexpr int kHeight = 64;

int compareImage(const QString &name, const QImage &image, const QJsonObject &refs, const QString &tmp)
{
    const QJsonArray checks = refs.value(name).toArray();
    int failedRegions = 0;
    int checkedPixels = 0;
    if (image.size() != QSize(kWidth, kHeight) || checks.isEmpty()) {
        std::printf("FAIL golden %s: viewport %dx%d instead of 128x64, or no references\n",
                    qPrintable(name), image.width(), image.height());
        return 1;
    }
    for (const QJsonValue &v : checks) {
        const QJsonObject o = v.toObject();
        const QJsonArray rect = o.value("rect").toArray();
        const QJsonArray rgb = o.value("rgb").toArray();
        const int tol = o.value("tolerance").toInt(3);
        if (rect.size() != 4 || rgb.size() != 3 || tol < 0) {
            std::printf("FAIL golden %s: invalid region reference\n", qPrintable(name));
            ++failedRegions;
            continue;
        }
        const int x0 = rect[0].toInt(-1), y0 = rect[1].toInt(-1);
        const int x1 = rect[2].toInt(-1), y1 = rect[3].toInt(-1);
        if (x0 < 0 || y0 < 0 || x1 > image.width() || y1 > image.height() || x1 <= x0 || y1 <= y0) {
            std::printf("FAIL golden %s: region outside image\n", qPrintable(name));
            ++failedRegions;
            continue;
        }
        int badPixels = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const QColor c = image.pixelColor(x, y);
                const int got[3] = {c.red(), c.green(), c.blue()};
                ++checkedPixels;
                if (std::abs(got[0] - rgb[0].toInt(-1000)) <= tol &&
                    std::abs(got[1] - rgb[1].toInt(-1000)) <= tol &&
                    std::abs(got[2] - rgb[2].toInt(-1000)) <= tol) continue;
                if (++badPixels <= 5)
                    std::printf("FAIL golden %s (%d,%d): RGB %d,%d,%d expected %d,%d,%d (+/-%d)\n",
                                qPrintable(name), x, y, got[0], got[1], got[2],
                                rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt(), tol);
            }
        }
        if (badPixels) {
            std::printf("FAIL golden %s: %d/%d pixels in region [%d,%d,%d,%d]\n",
                        qPrintable(name), badPixels, (x1-x0)*(y1-y0), x0, y0, x1, y1);
            ++failedRegions;
        }
    }
    if (failedRegions) {
        const QString path = tmp + "/golden-" + name + "-actual.png";
        if (image.save(path)) std::printf("Golden render diagnostic: %s\n", qPrintable(path));
    } else std::printf("ok golden %s (%d validated pixels)\n", qPrintable(name), checkedPixels);
    return failedRegions;
}
}

int runGoldenRenderTests(Engine &engine, const QString &root, const QString &tmp)
{
    QFile file(root + "/golden/references.json");
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("FAIL golden: cannot read fixture %s\n", qPrintable(file.fileName()));
        return 1;
    }
    QJsonParseError pe;
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &pe);
    if (!parsed.isObject()) {
        std::printf("FAIL golden: invalid fixture: %s\n", qPrintable(pe.errorString()));
        return 1;
    }
    const QJsonObject refs = parsed.object();
    int failures = 0;
    QString err;

    auto reset = [&] {
        engine.newProject();
        engine.setCompositionSize(QSize(kWidth, kHeight));
        engine.setViewportSize(engine.indexOfId(engine.mainViewportId()), QSize(kWidth, kHeight));
        engine.fadeCompositionOpacity(1.0, 0.0);
    };
    auto capture = [&] {
        for (int i = 0; i < 3; ++i) engine.renderFrame();
        return engine.grabViewport(engine.mainViewportId());
    };
    auto need = [&](bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL golden setup: %s (%s)\n", what, qPrintable(err));
            ++failures;
        }
        return ok;
    };

    // Source image: opaque red on the left, blue on the right; 2:1 aspect.
    QImage halves(64, 32, QImage::Format_RGBA8888);
    for (int y = 0; y < halves.height(); ++y)
        for (int x = 0; x < halves.width(); ++x)
            halves.setPixelColor(x, y, x < 32 ? QColor(255, 0, 0) : QColor(0, 0, 255));
    const QString halvesPath = tmp + "/golden-halves.png";
    if (!need(halves.save(halvesPath), "write input PNG")) return failures;
    reset();
    int layer = engine.addLayer(QStringLiteral("Split"));
    if (need(engine.setLayerImage(layer, halvesPath, &err), "image source"))
        failures += compareImage(QStringLiteral("split"), capture(), refs, tmp);

    // The stock FlipCrop ISF filter should exchange the two image halves.
    reset();
    layer = engine.addLayer(QStringLiteral("Flip"));
    const QString flipFx = root + "/../isf/effects/FlipCrop.fs";
    if (need(engine.setLayerImage(layer, halvesPath, &err), "image before ISF")) {
        const int fx = engine.addEffect(layer, flipFx, &err);
        if (need(fx >= 0, "ISF filter")) {
            IsfInput *in = engine.layer(layer)->effects[size_t(fx)]->input(QStringLiteral("flipH"));
            if (need(in != nullptr, "flipH ISF parameter")) {
                in->bValue = true;
                failures += compareImage(QStringLiteral("flip"), capture(), refs, tmp);
            }
        }
    }

    // Correct source-over alpha without sampling boundaries or edges.
    // 50% red over opaque blue = (128, 0, 127) in the current SDR pipeline.
    QImage top(64, 32, QImage::Format_RGBA8888);
    top.fill(QColor(255, 0, 0, 128));
    QImage bottom(64, 32, QImage::Format_RGBA8888);
    bottom.fill(QColor(0, 0, 255, 255));
    const QString topPath = tmp + "/golden-half-red.png";
    const QString bottomPath = tmp + "/golden-blue.png";
    if (need(top.save(topPath) && bottom.save(bottomPath), "alpha input PNGs")) {
        reset();
        // addLayer() inserts just below the viewport, so add the bottom
        // first; the next inserted layer becomes the visible top layer.
        const int bottomLayer = engine.addLayer(QStringLiteral("Bottom"));
        const int topLayer = engine.addLayer(QStringLiteral("Top"));
        if (need(engine.setLayerImage(topLayer, topPath, &err), "top source") &&
            need(engine.setLayerImage(bottomLayer + 1, bottomPath, &err), "bottom source"))
            failures += compareImage(QStringLiteral("alpha"), capture(), refs, tmp);
    }

    // Generator output is checked independently of the texture upload path.
    reset();
    const int gen = engine.addLayer(QStringLiteral("Golden solid"));
    if (need(engine.setLayerIsf(gen, root + "/../isf/generators/SolidColor.fs", &err), "SolidColor generator")) {
        auto *input = engine.layer(gen)->generator->input(QStringLiteral("color"));
        if (need(input != nullptr, "SolidColor.color")) {
            input->cValue[0] = 0.2f;
            input->cValue[1] = 0.6f;
            input->cValue[2] = 0.8f;
            input->cValue[3] = 1.0f;
            failures += compareImage(QStringLiteral("solid_isf"), capture(), refs, tmp);
        }
    }
    engine.newProject();
    return failures;
}
