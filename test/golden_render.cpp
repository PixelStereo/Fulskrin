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
    int failures = 0;
    if (image.size() != QSize(kWidth, kHeight) || checks.isEmpty()) {
        std::printf("FAIL golden %s: viewport is %dx%d (expected 128x64), or reference is absent\n",
                    qPrintable(name), image.width(), image.height());
        return 1;
    }
    for (const QJsonValue &v : checks) {
        const QJsonObject o = v.toObject();
        const int x = o.value("x").toInt(-1), y = o.value("y").toInt(-1);
        const QJsonArray rgb = o.value("rgb").toArray();
        const int tol = o.value("tolerance").toInt(3);
        if (x < 0 || x >= image.width() || y < 0 || y >= image.height() || rgb.size() != 3 || tol < 0) {
            std::printf("FAIL golden %s: invalid reference sample\n", qPrintable(name));
            ++failures;
            continue;
        }
        const QColor c = image.pixelColor(x, y);
        const int got[3] = {c.red(), c.green(), c.blue()};
        bool matches = true;
        for (int k = 0; k < 3; ++k)
            if (std::abs(got[k] - rgb[k].toInt(-1000)) > tol) matches = false;
        if (!matches) {
            std::printf("FAIL golden %s at (%d,%d): RGB actual %d,%d,%d; expected %d,%d,%d (+/-%d)\n",
                        qPrintable(name), x, y, got[0], got[1], got[2],
                        rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt(), tol);
            ++failures;
        }
    }
    if (failures) {
        const QString path = tmp + "/golden-" + name + "-actual.png";
        if (image.save(path)) std::printf("Golden render diagnostic: %s\n", qPrintable(path));
    } else std::printf("ok golden %s (%d samples)\n", qPrintable(name), checks.size());
    return failures;
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
        engine.setViewportSize(engine.mainViewportId(), QSize(kWidth, kHeight));
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
        // The first layer in the list is drawn on top of later layers.
        const int topLayer = engine.addLayer(QStringLiteral("Top"));
        const int bottomLayer = engine.addLayer(QStringLiteral("Bottom"));
        if (need(engine.setLayerImage(topLayer, topPath, &err), "top source") &&
            need(engine.setLayerImage(bottomLayer, bottomPath, &err), "bottom source"))
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
