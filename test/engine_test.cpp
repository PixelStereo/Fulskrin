// Headless engine tests: load/save, video, ISF (multi-pass, .vs, errors).
#include "Commands.h"
#include "EventLog.h"
#include "FrameTiming.h"
#include "LogModel.h"
#include "DeckLink.h"
#include "Engine.h"
#include "AudioOutput.h"
#include "AudioStream.h"
#include "LayerTree.h"
#include "Osc.h"
#include "Parameter.h"
#include "Zeroconf.h"
#include <QJsonArray>
#include <QSet>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>
#include <QColor>
#include <QElapsedTimer>
#include <QLineF>
#include <map>
#include <mutex>
#include <QRegularExpression>
#include <QUndoStack>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QScreen>
#include <QWindow>
#include <QJsonDocument>
#include <QLineF>
#include <QSurfaceFormat>
#include <QThread>
#include <QTime>
#include <cmath>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdio>
#include <cstring>

static int failures = 0;
int runVideoTests(Engine &e, const QString &root, const QString &tmp); // video_test.cpp
int runCueCaptureTests(Engine &e); // cue_capture.cpp
int runGpuTargetTests(Engine &e); // gpu_targets.cpp
int runGoldenRenderTests(Engine &e, const QString &root, const QString &tmp); // golden_render.cpp
int runEmergencyStopTests(Engine &e); // emergency_stop.cpp
int runTimelineRepeatTests(Engine &e, const QString &tmp); // timeline_repeat.cpp
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } \
        else std::printf("ok    %s\n", #cond);                                      \
    } while (0)

// A parameter's value in a layer's state (its "params", by address)
static void setParam(QJsonObject &layer, const QString &path, const QJsonValue &v)
{
    QJsonObject p = layer.value("params").toObject();
    p[path] = v;
    layer["params"] = p;
}
static QJsonValue paramOf(const QJsonObject &layer, const QString &path) { return layer.value("params").toObject().value(path); }

static QJsonObject readJson(const QString &p)
{
    QFile f(p);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(f.readAll()).object();
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // every line out before a crash (CI)
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    const QString root = QString(TEST_DIR);
    const QString isf = root + "/../isf";


    Engine e;
    QString err;
    if (!e.initialize(&err)) { // nothing else can run
        std::printf("FAIL OpenGL: %s\n", qPrintable(err));
        return 1;
    }
    // Areas: each one starts from a new project and can run alone (pinol_tests --area <name>: CTest runs them so,
    // one process each); without --area, all of them in turn
    QStringList only;
    for (int k = 1; k + 1 < argc; ++k)
        if (QString(argv[k]) == "--area") only << QString(argv[k + 1]);
    // Its own folder: areas run side by side (ctest -j)
    const QString tmp = QDir::tempPath() + "/pinol_test" + (only.isEmpty() ? QString() : "_" + only.join('_'));
    QDir().mkpath(tmp);
    { // Pictures several areas use: left half red, right half blue; top half red, bottom half blue
        QImage rb(64, 32, QImage::Format_RGB32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) rb.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        rb.save(tmp + "/redblue.png");
        QImage tr(64, 32, QImage::Format_RGBA8888);
        tr.fill(QColor(255, 0, 0));
        for (int y = 16; y < 32; ++y)
            for (int x = 0; x < 64; ++x) tr.setPixelColor(x, y, QColor(0, 0, 255));
        tr.save(tmp + "/topred.png");
    }
    auto area = [&](const char *name) {
        if (!only.isEmpty() && !only.contains(QLatin1String(name))) return false;
        std::printf("\n== %s\n", name);
        e.newProject();
        return true;
    };

    // A composition is always shown through a viewport, and viewports come first in the list:
    // the layers of these tests start at row V.
    const int V = 1;

    // Compatibility mode: pinol_tests --check-isf <folder>  -> compiles and renders every shader in the folder
    if (argc >= 3 && QString(argv[1]) == "--check-isf") {
        QDirIterator it(argv[2], {"*.fs"}, QDir::Files, QDirIterator::Subdirectories);
        int total = 0, ok = 0, skipped = 0;
        int v = e.addLayer("src");
        e.setLayerVideo(v, root + "/media/h264.mp4", &err);
        int g = e.addLayer("gen");
        while (it.hasNext()) {
            const QString path = it.next();
            IsfInstance::Header h = IsfInstance::readHeader(path);
            if (!h.ok) { std::printf("HEADER   %s\n", qPrintable(path)); ++total; continue; }
            if (h.isTransition) { ++skipped; continue; }
            ++total;
            QString msg;
            bool valid;
            if (h.isFilter) {
                int fx = e.addEffect(v, path, &msg);
                valid = fx >= 0 && e.layer(v)->effects[size_t(fx)]->isValid();
                if (valid) {
                    for (int i = 0; i < 2; ++i) e.renderFrame();
                    e.removeEffect(v, fx);
                }
            } else {
                valid = e.setLayerIsf(g, path, &msg);
                for (int i = 0; i < 2; ++i) e.renderFrame();
            }
            if (valid) ++ok;
            else std::printf("FAIL     %s\n%s\n", qPrintable(QFileInfo(path).fileName()), qPrintable(msg.left(600)));
        }
        std::printf("\nISF: %d/%d compiled (%d transitions skipped)\n", ok, total, skipped);
        e.shutdown();
        return ok == total ? 0 : 1;
    }

    if (area("emergency_stop")) {
        failures += runEmergencyStopTests(e);
    } // emergency_stop
    if (area("cue_capture")) {
        failures += runCueCaptureTests(e);
    } // cue_capture
    if (area("gpu_targets")) {
        failures += runGpuTargetTests(e);
    } // gpu_targets
    if (area("golden")) {
        failures += runGoldenRenderTests(e, root, tmp);
    } // golden
    if (area("timeline_repeat")) {
        failures += runTimelineRepeatTests(e, tmp);
    } // timeline_repeat
    if (area("isf_safe")) {
        // Live reloading or inserting an invalid shader must never take the
        // existing source, its program, its parameters or its image away.
        e.setCompositionSize(QSize(128, 64));
        e.setViewportSize(e.indexOfId(e.mainViewportId()), QSize(128, 64));
        e.fadeCompositionOpacity(1.0, 0.0);
        const QString live = tmp + "/HotReload.fs";
        QFile original(isf + "/generators/SolidColor.fs");
        CHECK(original.open(QIODevice::ReadOnly));
        const QByteArray valid = original.readAll();
        const QByteArray broken = R"(/*{"INPUTS":[{"NAME":"color","TYPE":"color","DEFAULT":[1,1,1,1]}]}*/
void main(){ gl_FragColor = definitely_not_defined; }
)";
        auto writeShader = [&](const QByteArray &body) {
            QFile f(live);
            return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(body) == body.size();
        };
        CHECK(writeShader(valid));
        int g = e.addLayer(QStringLiteral("Live shader"));
        CHECK(e.setLayerIsf(g, live, &err));
        IsfInstance *inst = e.layer(g)->generator.get();
        CHECK(inst && inst->isValid());
        IsfInput *color = inst->input(QStringLiteral("color"));
        CHECK(color != nullptr);
        if (color) {
            color->cValue[0] = 0.0f;
            color->cValue[1] = 1.0f;
            color->cValue[2] = 0.0f;
            color->cValue[3] = 1.0f;
        }
        e.renderFrame();
        const QImage before = e.grabViewport(e.mainViewportId());
        CHECK(!before.isNull() && before.pixelColor(64, 32).green() > 240);
        CHECK(writeShader(broken));
        CHECK(!e.reloadIsf(inst));
        CHECK(inst->isValid() && e.layer(g)->generator.get() == inst);
        CHECK(inst->error().contains("definitely_not_defined"));
        e.renderFrame();
        const QImage afterBadReload = e.grabViewport(e.mainViewportId());
        CHECK(!afterBadReload.isNull() && afterBadReload.pixelColor(64, 32).green() > 240);
        CHECK(!e.setLayerIsf(g, live, &err) && err.contains("definitely_not_defined"));
        CHECK(e.layer(g)->generator.get() == inst && inst->isValid());
        const int nFx = int(e.layer(g)->effects.size());
        CHECK(e.addEffect(g, live, &err) == -1);
        CHECK(int(e.layer(g)->effects.size()) == nFx);
        CHECK(writeShader(valid));
        CHECK(e.reloadIsf(inst) && inst->isValid());
        CHECK(e.layer(g)->generator.get() == inst);
        color = inst->input(QStringLiteral("color"));
        CHECK(color && color->cValue[0] == 0.0f && color->cValue[1] == 1.0f);
        e.renderFrame();
        const QImage afterRepair = e.grabViewport(e.mainViewportId());
        CHECK(!afterRepair.isNull() && afterRepair.pixelColor(64, 32).green() > 240);
        // A project whose shaders are broken or missing when it is opened (another machine, a shader being
        // edited): the generator and the effect stay, with their paths, values and errors. Saved again, nothing of
        // them is lost; once the shaders are back, they work again with their values.
        {
            e.newProject();
            const QString gen = tmp + "/KeepGen.fs", fx = tmp + "/KeepFx.fs";
            QFile::remove(gen);
            QFile::remove(fx);
            CHECK(QFile::copy(isf + "/generators/SolidColor.fs", gen) && QFile::copy(isf + "/effects/FlipCrop.fs", fx));
            QFile::setPermissions(gen, QFile::ReadOwner | QFile::WriteOwner);
            QFile::setPermissions(fx, QFile::ReadOwner | QFile::WriteOwner);
            const int gl = e.addLayer(QStringLiteral("Kept generator"));
            CHECK(e.setLayerIsf(gl, gen, &err));
            if (IsfInput *c = e.layer(gl)->generator->input(QStringLiteral("color"))) {
                c->cValue[0] = 0.25f;
                c->cValue[1] = 0.5f;
                c->cValue[2] = 0.75f;
            }
            CHECK(e.addEffect(gl, fx, &err) == 0);
            const QString first = tmp + "/keep_first.pinol", second = tmp + "/keep_second.pinol";
            CHECK(e.saveProject(first, {}, &err));
            // The generator does not compile any more; the effect's file is gone
            {
                QFile f(gen);
                CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(broken) == broken.size());
            }
            CHECK(QFile::remove(fx));
            err.clear();
            CHECK(e.loadProject(first, nullptr, &err));
            std::printf("       opened with broken shaders: %s\n", qPrintable(err.section('\n', 0, 1)));
            int kept = -1;
            for (int k = 0; k < e.layerCount(); ++k)
                if (e.layer(k)->name == QStringLiteral("Kept generator")) kept = k;
            CHECK(kept >= 0);
            if (kept >= 0) {
                const Layer *l = e.layer(kept);
                CHECK(l->type == SourceType::Isf && l->generator && !l->generator->isValid() && !l->error.isEmpty());
                CHECK(QFileInfo(l->sourcePath).fileName() == QStringLiteral("KeepGen.fs"));
                CHECK(l->effects.size() == 1 && !l->effects[0]->isValid() &&
                      QFileInfo(l->effects[0]->path()).fileName() == QStringLiteral("KeepFx.fs"));
                e.renderFrame(); // a broken generator and a missing effect render (black / passed through), no crash
                // An undo of the chain keeps the broken effect too
                const QJsonArray chain = e.effectsJson(kept);
                e.setEffectsJson(kept, chain);
                CHECK(e.layer(kept)->effects.size() == 1);
            }
            CHECK(e.saveProject(second, {}, &err));
            {
                QFile f(second);
                CHECK(f.open(QIODevice::ReadOnly));
                const QByteArray saved = f.readAll();
                CHECK(saved.contains("KeepGen.fs") && saved.contains("KeepFx.fs")); // nothing lost on save
            }
            // The shaders back: the second save opens as the first one was, values included
            QFile::remove(gen);
            CHECK(QFile::copy(isf + "/generators/SolidColor.fs", gen) && QFile::copy(isf + "/effects/FlipCrop.fs", fx));
            CHECK(e.loadProject(second, nullptr, &err));
            kept = -1;
            for (int k = 0; k < e.layerCount(); ++k)
                if (e.layer(k)->name == QStringLiteral("Kept generator")) kept = k;
            CHECK(kept >= 0);
            if (kept >= 0) {
                const Layer *l = e.layer(kept);
                CHECK(l->generator && l->generator->isValid() && l->effects.size() == 1 && l->effects[0]->isValid());
                const IsfInput *c = l->generator->input(QStringLiteral("color"));
                CHECK(c && std::abs(c->cValue[0] - 0.25f) < 1e-6 && std::abs(c->cValue[2] - 0.75f) < 1e-6);
            }
            e.newProject();
        }
    } // isf_safe

    if (area("video")) {
    // 0. Video pipeline: pixel formats, HAP, upload buffers (video_test.cpp). Leaves a new project.
    failures += runVideoTests(e, root, tmp);
    } // video

    if (area("project")) {
    // 1. JSON round trip (project built here: generator + warped mesh, video + effects)
    int gen = e.addLayer("Test Pattern");
    CHECK(e.setLayerIsf(gen, isf + "/generators/TestPattern.fs", &err));
    e.layer(gen)->mapping.meshMode = true;
    e.layer(gen)->mapping.setControlPoint(1, 1, QPointF(0.4, 0.3));
    e.layer(gen)->mapping.setCorner(1, QPointF(0.9, 0.1));
    int vid = e.addLayer("Video", V + 1);
    CHECK(e.setLayerVideo(vid, root + "/media/h264.mp4", &err));
    e.addEffect(vid, isf + "/effects/ColorCorrection.fs", &err);
    e.addEffect(vid, isf + "/effects/Trails.fs", &err);
    e.layer(vid)->effects[0]->inputs()[1].fValue = 0.5;
    e.layer(vid)->blend = BlendMode::Screen;
    CHECK(e.layerCount() == V + 2);
    for (int i = 0; i < 10; ++i) e.renderFrame();
    const QPointF cp = e.layer(gen)->mapping.controlPoint(1, 1);
    CHECK(e.saveProject(tmp + "/a.pinol", {}, &err));
    CHECK(e.loadProject(tmp + "/a.pinol", nullptr, &err));
    CHECK(err.isEmpty());
    CHECK(e.saveProject(tmp + "/b.pinol", {}, &err));
    QJsonObject a = readJson(tmp + "/a.pinol"), b = readJson(tmp + "/b.pinol");
    CHECK(a.value("layers") == b.value("layers"));
    CHECK(e.layer(V + 0)->mapping.meshMode);
    CHECK(QLineF(e.layer(V + 0)->mapping.controlPoint(1, 1), cp).length() < 1e-9);
    CHECK(e.layer(V + 1)->blend == BlendMode::Screen);
    CHECK(e.layer(V + 1)->effects.size() == 2 && std::abs(e.layer(V + 1)->effects[0]->inputs()[1].fValue - 0.5) < 1e-9);

    { // Position / scale of the mapped shape: an exact affine transform of every point (corners and mesh warp)
        Mapping m;
        m.setCorner(0, QPointF(0.1, 0.2));
        m.setCorner(1, QPointF(0.7, 0.15));
        m.setCorner(2, QPointF(0.8, 0.9));
        m.setCorner(3, QPointF(0.05, 0.8));
        m.setControlPoint(1, 1, m.controlPoint(1, 1) + QPointF(0.04, -0.03));
        const QRectF from = m.bounds();
        const QPointF p = m.map(0.37, 0.61);
        const QRectF to(0.3, 0.25, from.width() * 0.5, from.height() * 1.5);
        m.setBounds(to);
        const QPointF expected(to.left() + (p.x() - from.left()) * 0.5, to.top() + (p.y() - from.top()) * 1.5);
        CHECK(QLineF(m.map(0.37, 0.61), expected).length() < 1e-9);
        CHECK(std::abs(m.bounds().width() - to.width()) < 1e-9 && std::abs(m.bounds().center().x() - to.center().x()) < 1e-9);
    }

    { // Only a Pinol project is opened, and the one open stays when another file is refused
        const int before = e.layerCount();
        auto write = [&](const QString &name, const QByteArray &json) {
            QFile f(tmp + "/" + name);
            f.open(QIODevice::WriteOnly);
            f.write(json);
            return tmp + "/" + name;
        };
        QString why;
        CHECK(!e.loadProject(write("empty.pinol", "{}"), nullptr, &why) && why.startsWith("Not a Pinol project"));
        CHECK(!e.loadProject(write("other.json", R"({"app":"Other","layers":[]})"), nullptr, &why));
        CHECK(!e.loadProject(write("future.pinol", R"({"app":"Pinol","format_version":99,"layers":[]})"), nullptr, &why) &&
              why.contains("format 99"));
        CHECK(e.layerCount() == before);

        // Invalid *structure* must be refused before clearProject(): the live
        // scene, viewport and current project path all remain untouched.
        const QJsonObject working = e.layerJson(V);
        const quint64 workingId = e.layerId(V);
        const QString workingPath = e.projectPath();
        const auto malformed = [&](const QString &name, const QByteArray &body) {
            QString reason;
            const bool loaded = e.loadProject(write(name, body), nullptr, &reason);
            CHECK(!loaded && reason.startsWith("Invalid Pinol project at "));
            CHECK(e.layerCount() == before && e.layerId(V) == workingId && e.layerJson(V) == working);
            CHECK(e.projectPath() == workingPath);
        };
        malformed("bad-layer.pinol", R"({"app":"Pinol","format_version":1,"layers":[null]})");
        malformed("bad-fx.pinol", R"({"app":"Pinol","format_version":1,"layers":[{"source":{"type":"none"},"fx":[null]}]})");
        malformed("bad-source.pinol", R"({"app":"Pinol","format_version":1,"layers":[{"source":{"type":"unrecognized"}}]})");
        malformed("bad-size.pinol", R"({"app":"Pinol","format_version":1,"composition":{"width":-5},"layers":[]})");
        malformed("bad-sequence.pinol", R"({"app":"Pinol","format_version":1,"layers":[],"sequences":[false]})");
        malformed("bad-params.pinol", R"({"app":"Pinol","format_version":1,"layers":[{"params":[1,2]}]})");
        malformed("bad-anims.pinol", R"({"app":"Pinol","format_version":1,"layers":[{"anims":[null]}]})");
        malformed("bad-snapshot-tree.pinol", R"({"app":"Pinol","format_version":1,"layers":[],"snapshot_tree":[false]})");
        malformed("bad-snapshot-id.pinol", R"({"app":"Pinol","format_version":1,"layers":[],"snapshot_tree":["oops"]})");
        malformed("bad-snapshot-children.pinol", R"({"app":"Pinol","format_version":1,"layers":[],"snapshot_tree":[{"group":"Act","id":"3","children":[null]}]})");

        // saveProject() serializes snapshot leaves as strings, while groups
        // are objects; both (also when nested) must load successfully.
        const QString treeProject = write("mixed-snapshot-tree.pinol",
            R"({"app":"Pinol","format_version":1,"layers":[{"name":"Live","source":{"type":"none"}}],"snapshots":[{"id":"12","name":"Look 1","fade":0,"layers":[]},{"id":"13","name":"Look 2","fade":0,"layers":[]}],"snapshot_tree":["12",{"group":"Act","id":"44","open":true,"children":["13"]}]})");
        QString treeLoadError;
        CHECK(e.loadProject(treeProject, nullptr, &treeLoadError));
        CHECK(e.snapshotCount() == 2 && e.snapshotTree().size() == 2);
        CHECK(e.snapshotTree().size() == 2 && e.snapshotTree()[1].group == 44 &&
              e.snapshotTree()[1].children.size() == 1 && e.snapshotTree()[1].children[0].snapshot == 13);
    }
    // Collect Files: the project and every file it uses in one folder, copied, then moved
    {
        e.newProject();
        const QString src = tmp + "/collect_src", other = tmp + "/collect_other";
        QDir(src).removeRecursively();
        QDir(other).removeRecursively();
        QDir(tmp + "/collect_out").removeRecursively();
        QDir().mkpath(src + "/shaders");
        QDir().mkpath(other);
        auto put = [](const QString &from, const QString &to) {
            QFile::remove(to);
            const bool ok = QFile::copy(from, to);
            QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
            return ok;
        };
        CHECK(put(root + "/media/bars.png", src + "/bars.png") && put(root + "/media/h264.mp4", src + "/clip.mp4") &&
              put(root + "/media/tone.wav", src + "/tone.wav") && put(root + "/media/av.mp4", src + "/later.mp4") &&
              put(isf + "/effects/FlipCrop.fs", src + "/Flip.fs") &&
              put(isf + "/transitions/Dissolve.fs", src + "/Dissolve.fs") &&
              put(root + "/media/bars.png", src + "/shaders/tex.png") &&
              put(root + "/media/bars.png", other + "/bars.png")); // another file of the same name
        {
            QFile f(src + "/shaders/Pic.fs");
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("/*{\"ISFVSN\":\"2\",\"INPUTS\":[],\"IMPORTED\":{\"tex\":{\"PATH\":\"tex.png\"}}}*/\n"
                    "void main(){ gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n");
        }
        const int img = e.addLayer(QStringLiteral("Image"));
        CHECK(e.setLayerImage(img, src + "/bars.png", &err));
        CHECK(e.addEffect(img, src + "/Flip.fs", &err) >= 0);
        const int img2 = e.addLayer(QStringLiteral("Image 2"));
        CHECK(e.setLayerImage(img2, other + "/bars.png", &err));
        const int vid = e.addLayer(QStringLiteral("Clip"));
        CHECK(e.setLayerVideo(vid, src + "/clip.mp4", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(vid)->transition = src + "/Dissolve.fs";
        }
        const quint64 vidId = e.layerId(vid);
        const int snd = e.addLayer(QStringLiteral("Sound"));
        CHECK(e.setLayerAudio(snd, src + "/tone.wav", &err));
        const int gen = e.addLayer(QStringLiteral("Generator"));
        CHECK(e.setLayerIsf(gen, src + "/shaders/Pic.fs", &err));
        e.addBinItems({src + "/missing.mov"});
        // A snapshot that gives the clip another file: that file is collected too
        Engine::Snapshot later;
        later.name = QStringLiteral("Later");
        for (const QJsonValue &v : e.captureLayers()) {
            QJsonObject o = v.toObject();
            if (o.value("id").toString().toULongLong() == vidId) {
                QJsonObject so = o.value("source").toObject();
                so["path"] = src + "/later.mp4";
                o["source"] = so;
            }
            later.layers.append(o);
        }
        e.addSnapshot(later);

        const Engine::CollectPlan plan = e.planCollect(tmp + "/collect_out", QStringLiteral("Show"), {});
        QStringList rel;
        for (const auto &f : plan.files) rel << QDir(plan.folder).relativeFilePath(f.to);
        rel.sort();
        std::printf("       collect: %s; missing: %s\n", qPrintable(rel.join(", ")), qPrintable(plan.missing.join(", ")));
        for (const char *want : {"MEDIA/PICTS/bars.png", "MEDIA/PICTS/bars 2.png", "MEDIA/MOVIES/clip.mp4",
                                 "MEDIA/MOVIES/later.mp4", "MEDIA/AUDIO/tone.wav", "MEDIA/ISF/Flip.fs",
                                 "MEDIA/ISF/Dissolve.fs", "MEDIA/ISF/Pic/Pic.fs", "MEDIA/ISF/Pic/tex.png"})
            CHECK(rel.contains(QString::fromLatin1(want)));
        CHECK(plan.files.size() == 9 && plan.missing.size() == 1 && plan.missing.front().endsWith("missing.mov"));
        CHECK(plan.projectFile == tmp + "/collect_out/Show/Show.pinol" && !QFileInfo::exists(plan.folder)); // nothing yet
        CHECK(e.collect(plan, &err));
        CHECK(!e.collect(plan, &err) && !err.isEmpty()); // the folder exists now: refused
        {
            QFile f(plan.projectFile);
            CHECK(f.open(QIODevice::ReadOnly));
            const QByteArray json = f.readAll();
            CHECK(!json.contains(QFile::encodeName(src + "/bars.png")) && !json.contains(QFile::encodeName(src + "/later.mp4")) &&
                  json.contains("missing.mov")); // every collected path relocated; a missing one kept
        }
        // Opened from there: every source, effect, transition and imported image found inside the folder
        auto inside = [&](const QString &p) { return QDir::cleanPath(p).startsWith(plan.folder + "/MEDIA/"); };
        auto checkOpened = [&] {
            CHECK(e.loadProject(plan.projectFile, nullptr, &err));
            for (int k = 0; k < e.layerCount(); ++k) {
                Engine::Lock lk(&e.mutex());
                const Layer *l = e.layer(k);
                if (l->isViewport) continue;
                CHECK(inside(l->sourcePath) && l->error.isEmpty());
                for (const auto &fx : l->effects) CHECK(inside(fx->path()) && fx->isValid());
                if (l->name == QStringLiteral("Clip")) CHECK(inside(l->transition));
                if (l->generator) CHECK(l->generator->isValid());
            }
            bool snapshotInside = false;
            for (const QJsonValue &v : e.snapshot(0).layers)
                if (v.toObject().value("source").toObject().value("path").toString().endsWith("later.mp4"))
                    snapshotInside = inside(v.toObject().value("source").toObject().value("path").toString());
            CHECK(snapshotInside);
        };
        checkOpened();
        CHECK(QFileInfo::exists(src + "/clip.mp4")); // Copy: the originals stay

        // Move: the originals go once the collected project is the one open
        e.loadProject(plan.projectFile, nullptr, &err);
        const Engine::CollectPlan moved = e.planCollect(tmp + "/collect_out", QStringLiteral("Moved"), {});
        CHECK(e.collect(moved, &err) && e.loadProject(moved.projectFile, nullptr, &err));
        QStringList failed;
        CHECK(Engine::removeCollectedOriginals(moved, &failed) == int(moved.files.size()) && failed.isEmpty());
        CHECK(!QFileInfo::exists(plan.folder + "/MEDIA/MOVIES/clip.mp4") && QFileInfo::exists(moved.folder + "/MEDIA/MOVIES/clip.mp4"));
        for (int k = 0; k < e.layerCount(); ++k) {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(k);
            if (!l->isViewport) CHECK(QDir::cleanPath(l->sourcePath).startsWith(moved.folder + "/MEDIA/"));
        }
        e.newProject();
    }
    { // Startup project: loaded as a template — same content, untitled, its file never written over
        const int gen2 = e.addLayer("Startup layer");
        CHECK(e.setLayerIsf(gen2, isf + "/generators/TestPattern.fs", &err));
        const int count = e.layerCount();
        const QString startup = tmp + "/startup.pinol";
        CHECK(e.saveProject(startup, {}, &err));
        QFile sf(startup);
        CHECK(sf.open(QIODevice::ReadOnly));
        const QByteArray before = sf.readAll();
        sf.close();
        e.newProject();
        CHECK(e.layerCount() != count);
        CHECK(e.loadProjectAsTemplate(startup, nullptr, &err));
        CHECK(e.layerCount() == count);
        CHECK(e.projectPath().isEmpty());
        CHECK(e.saveProject(tmp + "/from_startup.pinol", {}, &err));
        CHECK(sf.open(QIODevice::ReadOnly) && sf.readAll() == before);
        sf.close();
        QString missingErr;
        CHECK(!e.loadProjectAsTemplate(tmp + "/no_such_startup.pinol", nullptr, &missingErr) && !missingErr.isEmpty());
        e.newProject();
    }
    } // project

    if (area("playback")) {
    // 2. Video: seeking and end of playback without loop
    e.newProject();
    int v = e.addLayer("v");
    CHECK(e.setLayerVideo(v, root + "/media/h264.mp4", &err));
    CHECK(std::abs(e.layer(v)->duration() - 4.0) < 0.1);
    CHECK(e.layer(v)->video->width() == 1280);
    e.setLayerPlayMode(v, PlayMode::OneShot);
    e.seekLayer(v, 3.9);
    { // until the end of the media is reached and a frame uploaded (slow machines: up to 5 s)
        QElapsedTimer t;
        t.start();
        do {
            e.renderFrame();
            QThread::msleep(10);
        } while ((e.layer(v)->playing || !e.layer(v)->videoTex || e.layer(v)->videoTex->width() != 1280) && t.elapsed() < 5000);
    }
    CHECK(!e.layer(v)->playing);           // stops at end of media
    CHECK(e.layer(v)->videoTex && e.layer(v)->videoTex->width() == 1280); // a frame was indeed uploaded to the GPU
    e.setLayerPlaying(v, true);             // restarts from the beginning
    CHECK(e.layer(v)->position() < 0.01);

    // 3. ISF: custom vertex shader, broken shader, passes with computed size
    int fx = e.addEffect(v, root + "/isf/Offset.fs", &err);
    CHECK(e.layer(v)->effects[size_t(fx)]->isValid());
    QFile bad(tmp + "/Broken.fs");
    bad.open(QIODevice::WriteOnly);
    bad.write("/*{ \"INPUTS\": [ {\"NAME\":\"inputImage\",\"TYPE\":\"image\"}, ] }*/\nvoid main(){ gl_FragColor = undefinedThing; }\n");
    bad.close();
    const int beforeFx = int(e.layer(v)->effects.size());
    int fx2 = e.addEffect(v, tmp + "/Broken.fs", &err);
    CHECK(fx2 == -1 && int(e.layer(v)->effects.size()) == beforeFx);
    CHECK(err.contains("undefinedThing"));
    int fx3 = e.addEffect(v, root + "/../isf/effects/Blur.fs", &err);
    CHECK(fx3 >= 0 && e.layer(v)->effects[size_t(fx3)]->isValid());
    for (int i = 0; i < 5; ++i) e.renderFrame(); // still shows the working video and effects
    QImage out = e.grabOutput();
    CHECK(!out.isNull());
    } // playback

    if (area("undo")) {
    // 4. Undo / redo (manual mode)
    {
        QUndoStack undo;
        e.newProject();
        int g = e.addLayer("Test Pattern");
        e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
        undo.push(new cmd::AddLayer(&e, g, "add"));
        // Parameter, by its address: two close moves merge into a single step
        IsfValue v0 = cmd::resolveIsf(&e, g, -1)->inputs()[0].value();
        auto divisions = [&](double f) {
            IsfValue x = v0;
            x.f = f;
            return cmd::isfValueJson(QString(), cmd::resolveIsf(&e, g, -1)->inputs()[0], x);
        };
        CHECK(divisions(1).keys() == QStringList{"param/divisions"});
        CHECK(cmd::setValues(&undo, &e, e.layerId(g), divisions(20), "divisions"));
        CHECK(cmd::setValues(&undo, &e, e.layerId(g), divisions(30), "divisions"));
        CHECK(!cmd::setValues(&undo, &e, e.layerId(g), divisions(30), "divisions")); // unchanged: no step
        CHECK(undo.count() == 2);
        CHECK(cmd::resolveIsf(&e, g, -1)->inputs()[0].fValue == 30);
        undo.undo();
        CHECK(cmd::resolveIsf(&e, g, -1)->inputs()[0].fValue == v0.f);
        undo.redo();
        // ISF speed: undoable, merged, saved and restored, exposed to timelines
        {
            CHECK(cmd::resolveIsf(&e, g, -1)->speed == 1.0);
            const int n0 = undo.count();
            cmd::setValue(&undo, &e, e.layerId(g), "speed", 2.0, "speed");
            cmd::setValue(&undo, &e, e.layerId(g), "speed", 3.0, "speed");
            CHECK(undo.count() == n0 + 1 && cmd::resolveIsf(&e, g, -1)->speed == 3.0);
            undo.undo();
            CHECK(cmd::resolveIsf(&e, g, -1)->speed == 1.0);
            undo.redo();
            bool found = false;
            for (const auto &p : e.animatableParams(e.layerId(g))) found |= (p.path == "speed");
            CHECK(found);
            // The timeline menu and the snapshot timing offer the speed of an effect too
            const int fxi = e.addEffect(g, QStringLiteral(TEST_DIR) + "/isf/Offset.fs", &err);
            CHECK(fxi >= 0);
            bool fxFound = false;
            QString fxPath;
            for (const auto &p : e.animatableParams(e.layerId(g)))
                if (p.path.startsWith("fx/") && p.path.endsWith("/speed")) fxFound = true, fxPath = p.path;
            CHECK(fxFound);
            // Their values in the layer's state, at their addresses (the keys of their times in a snapshot)
            const QJsonObject saved = e.layerJson(g).value("params").toObject();
            CHECK(saved.contains(fxPath) && saved.contains("speed"));
            CHECK(Engine::timingKeyOf(QJsonObject{{"fx", 2.0}}, fxPath) == "fx" &&
                  Engine::timingKeyOf(QJsonObject{{fxPath, 1.0}, {"fx", 2.0}}, fxPath) == fxPath &&
                  Engine::timingKeyOf(QJsonObject{{"speed", 1.0}}, fxPath).isEmpty());
            e.removeEffect(g, fxi);
            CHECK(e.saveProject(tmp + "/speed.pinol", {}, &err));
            e.newProject();
            CHECK(e.loadProject(tmp + "/speed.pinol", nullptr, &err));
            CHECK(e.layerCount() == V + 1 && cmd::resolveIsf(&e, V + 0, -1)->speed == 3.0);
            e.newProject();
            g = e.addLayer("Test Pattern");
            e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
            undo.clear();
            undo.push(new cmd::AddLayer(&e, g, "add"));
            v0 = cmd::resolveIsf(&e, g, -1)->inputs()[0].value();
            CHECK(cmd::setValues(&undo, &e, e.layerId(g), divisions(30), "divisions"));
        }
        // Mapping
        Mapping before = cmd::SetMapping::read(&e, g), after = before;
        after.setCorner(0, QPointF(0.2, 0.2));
        undo.push(new cmd::SetMapping(&e, g, before, after, "corner"));
        CHECK(QLineF(e.layer(g)->mapping.corner(0), QPointF(0.2, 0.2)).length() < 1e-9);
        undo.undo();
        CHECK(QLineF(e.layer(g)->mapping.corner(0), before.corner(0)).length() < 1e-9);
        undo.redo();
        // Delete, then restore identically
        const QJsonObject snap = e.layerJson(g);
        undo.push(new cmd::RemoveLayer(&e, g));
        CHECK(e.layerCount() == V + 0);
        undo.undo();
        CHECK(e.layerCount() == V + 1);
        CHECK(e.layerJson(V + 0) == snap);
        // Effects
        const QJsonArray fxBefore = e.effectsJson(V + 0);
        e.addEffect(V + 0, root + "/../isf/effects/Hue.fs", &err);
        undo.push(new cmd::SetEffects(&e, V + 0, fxBefore, "fx"));
        CHECK(e.layer(V + 0)->effects.size() == 1);
        undo.undo();
        CHECK(e.layer(V + 0)->effects.empty());
        undo.redo();
        CHECK(e.layer(V + 0)->effects.size() == 1);
        // Back to the very start of the stack
        while (undo.canUndo()) undo.undo();
        CHECK(e.layerCount() == V + 0);
    }
    } // undo

    if (area("structure")) {
    // 4b. Structure helpers (viewports, groups inside groups)
    {
        auto item = [](quint64 id, quint64 parent) { return TreeNode{id, parent, TreeNode::Item}; };
        auto group = [](quint64 id, quint64 parent) { return TreeNode{id, parent, TreeNode::Group}; };
        auto viewport = [](quint64 id) { return TreeNode{id, 0, TreeNode::Viewport}; };
        LayerTree t = {item(1, 0), group(2, 0), item(3, 2), viewport(9), item(4, 0), item(5, 2)};
        LayerTree n = tree::normalized(t);
        // Viewports first, then the contents of a group right after it
        CHECK(n.size() == 6 && n[0].id == 9 && n[1].id == 1 && n[2].id == 2 && n[3].id == 3 && n[4].id == 5 &&
              n[5].id == 4);
        LayerTree in = tree::intoGroup(n, {1, 4}, 2);
        CHECK(in[1].id == 2 && in.back().id == 4 && in.back().parent == 2 && in[4].id == 1 && in[4].parent == 2);
        LayerTree out = tree::moved(in, {3}, 0, 0); // to the end, top level
        CHECK(out.back().id == 3 && out.back().parent == 0);
        LayerTree un = tree::ungrouped(in, 2);
        CHECK(un.back().id == 2 && un[1].id == 3 && un[1].parent == 0);
        LayerTree g = tree::moved(n, {2}, 1, 0); // a group moves with its contents
        CHECK(g[1].id == 2 && g[2].id == 3 && g[3].id == 5 && g[4].id == 1);
        // A group inside a group, and its contents follow it at every depth
        LayerTree nest = tree::intoGroup(n, {2}, 0); // 0 is not a group: nothing moves
        CHECK(nest == n);
        LayerTree two = tree::normalized({viewport(9), group(10, 0), item(11, 10), group(2, 10), item(3, 2), item(4, 0)});
        CHECK(two[1].id == 10 && two[2].id == 11 && two[3].id == 2 && two[4].id == 3 && two[5].id == 4);
        CHECK(tree::depthOf(two, 3) == 2 && tree::depthOf(two, 2) == 1 && tree::depthOf(two, 4) == 0);
        CHECK(tree::descendants(two, 10) == std::vector<quint64>({11, 2, 3}));
        // Never inside itself (directly or through a group it holds), never inside a viewport
        CHECK(tree::intoGroup(two, {10}, 2) == two);
        LayerTree loop = {viewport(9), group(10, 2), group(2, 10)};
        const LayerTree cut = tree::normalized(loop);
        CHECK(cut[size_t(tree::indexOf(cut, 10))].parent == 0 || cut[size_t(tree::indexOf(cut, 2))].parent == 0);
        LayerTree inVp = tree::normalized({viewport(9), item(1, 9)});
        CHECK(inVp[1].parent == 0);
        // Ungrouping a group inside a group: its contents go up one level only
        LayerTree up = tree::ungrouped(two, 2);
        CHECK(up[size_t(tree::indexOf(up, 3))].parent == 10);
    }
    } // structure

    if (area("compositing")) {
    // 4c. Groups, roi, color, effects switch, lock, blackout (manual rendering)
    {
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        QImage rb(64, 32, QImage::Format_RGB32); // left half red, right half blue
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) rb.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        rb.save(tmp + "/redblue.png");
        auto render = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            return e.grabOutput();
        };
        auto px = [](const QImage &img, int x, int y) { return QColor(img.pixel(x, y)); };
        int l = e.addLayer("img");
        CHECK(e.setLayerImage(l, tmp + "/redblue.png", &err));
        QImage img = render();
        CHECK(px(img, 8, 16).red() > 250 && px(img, 56, 16).blue() > 250);
        // Roi: the right half only fills the layer
        e.layer(l)->roi = QRectF(0.5, 0, 0.5, 1);
        img = render();
        CHECK(px(img, 8, 16).blue() > 250 && px(img, 8, 16).red() < 5);
        e.layer(l)->roi = Layer::fullRoi();
        // Color: remove red, add green
        e.layer(l)->color.remove[0] = 1.0f;
        e.layer(l)->color.add[1] = 0.5f;
        img = render();
        CHECK(px(img, 8, 16).red() < 5 && std::abs(px(img, 8, 16).green() - 128) < 4);
        e.layer(l)->color = ColorAdjust();
        // Balance: warmer = more red, less blue; magenta tint = less green; luminance kept
        {
            QImage gray(64, 32, QImage::Format_RGB32);
            gray.fill(qRgb(128, 128, 128));
            gray.save(tmp + "/gray.png");
            const int gl = e.addLayer("gray", V + 0);
            e.setLayerImage(gl, tmp + "/gray.png", &err);
            e.layer(gl)->color.temp = 4000;
            QColor c = px(render(), 32, 16);
            CHECK(c.red() > 140 && c.blue() < 110 && std::abs(qGray(c.rgb()) - 128) < 12);
            e.layer(gl)->color.temp = 0;
            e.layer(gl)->color.tint = 100;
            c = px(render(), 32, 16);
            CHECK(c.green() < 120 && c.red() > 135 && c.blue() > 135);
            e.removeLayer(gl);
        }
        // Effects switch: FlipCrop flips the picture, the general switch bypasses the chain
        const int fx = e.addEffect(l, isf + "/effects/FlipCrop.fs", &err);
        CHECK(fx == 0);
        for (IsfInput &in : e.layer(l)->effects[0]->inputs())
            if (in.name == "flipH") in.bValue = true;
        img = render();
        CHECK(px(img, 8, 16).blue() > 250);
        e.layer(l)->effectsEnabled = false;
        img = render();
        CHECK(px(img, 8, 16).red() > 250);
        e.layer(l)->effectsEnabled = true;
        e.removeEffect(l, 0);

        // Group: the layer moves in, the group's opacity, visibility, roi and color apply to it
        int g = e.addGroup("G", V + 0);
        CHECK(e.layer(g)->isGroup && e.layerCount() == V + 2);
        const quint64 gid = e.layerId(g), lid = e.layerId(V + 1);
        e.setStructure(tree::intoGroup(e.structure(), {lid}, gid));
        CHECK(e.groupIndexOf(V + 1) == V + 0 && e.groupMembers(V + 0) == QList<int>{V + 1});
        img = render();
        CHECK(px(img, 8, 16).red() > 250 && px(img, 56, 16).blue() > 250); // a group with defaults changes nothing
        e.layer(V + 0)->opacity = 0.5f;
        img = render();
        CHECK(std::abs(px(img, 8, 16).red() - 128) < 6);
        e.layer(V + 0)->opacity = 1.0f;
        e.layer(V + 0)->roi = QRectF(0, 0, 0.5, 1); // left half of the composition: red everywhere
        img = render();
        CHECK(px(img, 56, 16).red() > 250 && px(img, 56, 16).blue() < 5);
        e.layer(V + 0)->roi = Layer::fullRoi();
        e.layer(V + 0)->color.remove[2] = 1.0f;
        img = render();
        CHECK(px(img, 56, 16).blue() < 5 && px(img, 8, 16).red() > 250);
        e.layer(V + 0)->color = ColorAdjust();
        e.layer(V + 0)->enabled = false;
        img = render();
        CHECK(qGray(img.pixel(8, 16)) < 3 && !e.layer(V + 1)->parentEnabled);
        e.layer(V + 0)->enabled = true;
        // Solo: a soloed member keeps its group; a layer soloed elsewhere leaves the group out, and it is not drawn
        {
            const int o = e.addLayer("Other", e.layerCount());
            e.layer(V + 1)->solo = true;
            std::vector<bool> out = e.soloLeftOut();
            CHECK(!out[size_t(V + 0)] && !out[size_t(V + 1)] && out[size_t(o)]);
            img = render();
            CHECK(px(img, 8, 16).red() > 250);
            e.layer(V + 1)->solo = false;
            e.layer(o)->solo = true;
            out = e.soloLeftOut();
            CHECK(out[size_t(V + 0)] && out[size_t(V + 1)] && !out[size_t(o)]);
            img = render();
            CHECK(qGray(img.pixel(8, 16)) < 3);
            e.layer(o)->solo = false;
            out = e.soloLeftOut();
            CHECK(std::none_of(out.begin(), out.end(), [](bool b) { return b; }));
            img = render();
            CHECK(px(img, 8, 16).red() > 250);
            e.removeLayer(o);
        }
        // Source preview: the group's composite, at the requested size
        e.requestSourcePreview(gid, 32);
        for (int k = 0; k < 8; ++k) e.renderFrame();
        quint64 pid = 0;
        const QImage prev = e.sourcePreview(&pid);
        CHECK(pid == gid && prev.width() == 32 && prev.height() == 16 && QColor(prev.pixel(4, 8)).red() > 250);
        e.requestSourcePreview(0);
        // Lock: of the layer itself or of its group
        CHECK(!e.isLocked(V + 1));
        e.layer(V + 0)->locked = true;
        CHECK(e.isLocked(V + 1) && e.isLocked(V + 0));
        e.layer(V + 0)->locked = false;

        // Save / load keeps the structure and the new properties
        e.layer(V + 1)->roi = QRectF(0.1, 0.2, 0.5, 0.6);
        e.layer(V + 1)->color.add[2] = 0.25f;
        e.layer(V + 1)->effectsEnabled = false;
        e.layer(V + 0)->locked = true;
        const LayerTree before = e.structure();
        CHECK(e.saveProject(tmp + "/groups.pinol", {}, &err));
        CHECK(e.loadProject(tmp + "/groups.pinol", nullptr, &err));
        CHECK(e.structure() == before);
        CHECK(e.layer(V + 0)->locked && e.layer(V + 0)->isGroup);
        CHECK(QLineF(e.layer(V + 1)->roi.topLeft(), QPointF(0.1, 0.2)).length() < 1e-9 && std::abs(e.layer(V + 1)->roi.width() - 0.5) < 1e-9);
        CHECK(std::abs(e.layer(V + 1)->color.add[2] - 0.25f) < 1e-6 && !e.layer(V + 1)->effectsEnabled);
        e.layer(V + 0)->locked = false;
        // Duplicate a group: its members are copied into the copy
        const int dup = e.duplicateLayer(V + 0);
        CHECK(dup == V + 0 && e.layerCount() == V + 4 && e.layer(V + 0)->isGroup && e.layer(V + 1)->parent == e.layerId(V + 0));
        CHECK(e.layer(V + 2)->isGroup && e.layer(V + 3)->parent == e.layerId(V + 2) && e.layerId(V + 0) != e.layerId(V + 2));
        // Undo / redo of a structure change, and of a removed member
        {
            QUndoStack undo;
            const LayerTree t0 = e.structure();
            undo.push(new cmd::SetStructure(&e, t0, tree::ungrouped(t0, e.layerId(V + 0)), QStringLiteral("Ungroup")));
            CHECK(e.layer(V + 0)->parent == 0 && e.layer(V + 1)->isGroup);
            undo.undo();
            CHECK(e.structure() == t0);
            undo.push(new cmd::RemoveLayer(&e, V + 3));
            CHECK(e.layerCount() == V + 3 && e.groupMembers(V + 2).isEmpty());
            undo.undo();
            CHECK(e.structure() == t0);
            // A ROI moved past its old sides: one step, every side reached (each holds the others in)
            e.layer(V + 1)->roi = QRectF(QPointF(0, 0), QPointF(0.4, 0.4));
            CHECK(cmd::setValues(&undo, &e, e.layerId(V + 1),
                                 {{"roi/left", 0.6}, {"roi/top", 0.6}, {"roi/right", 0.9}, {"roi/bottom", 0.9}}, "roi"));
            CHECK(QLineF(e.layer(V + 1)->roi.topLeft(), QPointF(0.6, 0.6)).length() < 1e-9 &&
                  QLineF(e.layer(V + 1)->roi.bottomRight(), QPointF(0.9, 0.9)).length() < 1e-9);
            undo.undo();
            CHECK(QLineF(e.layer(V + 1)->roi.bottomRight(), QPointF(0.4, 0.4)).length() < 1e-9);
            e.layer(V + 1)->roi = Layer::fullRoi();
            CHECK(cmd::setValues(&undo, &e, e.layerId(V + 1), {{"remove/r", 1.0}, {"remove/g", 0.0}, {"remove/b", 0.0}}, "c"));
            CHECK(e.layer(V + 1)->color.remove[0] == 1.0f && e.layer(V + 1)->color.remove[1] == 0.0f);
            // By id: the layer moved in the list, the step still finds it
            const quint64 cid = e.layerId(V + 1);
            const int extra = e.addLayer("Test Pattern");
            e.moveLayer(extra, V + 0);
            CHECK(e.indexOfId(cid) == V + 2);
            undo.undo();
            CHECK(e.layer(e.indexOfId(cid))->color.remove[0] == 0.0f);
            e.removeLayer(V + 0);
        }
        // Removing a group keeps its members, at the top level
        e.removeLayer(V + 0);
        CHECK(e.layerCount() == V + 3 && e.layer(V + 0)->parent == 0 && !e.layer(V + 0)->isGroup);

        // Blackout: picture and sound
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        l = e.addLayer("img");
        e.setLayerImage(l, tmp + "/redblue.png", &err);
        e.setBlackout(true, 0);
        CHECK(e.blackout() && qGray(render().pixel(8, 16)) < 2 && e.audioOutput().fadeLevel() == 0.0f);
        e.fadeCompositionOpacity(1.0, 0); // the master fader does not end the blackout
        CHECK(qGray(render().pixel(8, 16)) < 2);
        e.setBlackout(false, 0);
        CHECK(!e.blackout() && render().pixel(8, 16) != qRgb(0, 0, 0) && e.audioOutput().fadeLevel() == 1.0f);
        // Composition speed: a coefficient on all time, clamped to 0..10; pause holds it at 0 and gives it back
        e.setCompositionSpeed(25);
        CHECK(e.compositionSpeed() == 10.0 && timeScale() == 10.0);
        e.setCompositionSpeed(0.5);
        e.setPaused(true);
        CHECK(timeScale() == 0.0 && e.compositionSpeed() == 0.5);
        e.setPaused(false);
        CHECK(timeScale() == 0.5);
        e.saveProject(tmp + "/cspeed.pinol", {}, &err);
        e.newProject();
        CHECK(e.compositionSpeed() == 1.0 && timeScale() == 1.0);
        CHECK(e.loadProject(tmp + "/cspeed.pinol", nullptr, &err) && e.compositionSpeed() == 0.5);
        e.newProject();
    }

    // Media Bin: an ISF generator can be imported (kept in the project, not listed as a media)
    {
        e.newProject();
        const QString gen = root + "/../isf/generators/TestPattern.fs";
        e.addBinItems({gen});
        bool inMedia = false;
        for (const auto &r : e.mediaUsage()) inMedia |= r.path.endsWith("TestPattern.fs");
        CHECK(Engine::isIsfFile(gen) && !Engine::isIsfFile("a.mp4") && !inMedia);
        CHECK(e.saveProject(tmp + "/binisf.pinol", {}, &err));
        e.newProject();
        CHECK(e.binItems().isEmpty());
        CHECK(e.loadProject(tmp + "/binisf.pinol", nullptr, &err) && e.binItems().size() == 1);
        e.newProject();
    }

    // Pivot: the center of the rotation, fixed to the picture
    {
        Mapping m;
        const QSize comp(100, 50);
        m.aspect = 2;
        m.position = {0.4, 0.4};
        m.size = {0.4, 0.4};
        CHECK((m.corner(0) - QPointF(0.2, 0.2)).manhattanLength() < 1e-9);
        CHECK((m.pivotPoint() - QPointF(0.4, 0.4)).manhattanLength() < 1e-9); // the middle by default
        m.setPivotPoint(QPointF(0.2, 0.2)); // the top left corner
        CHECK((m.pivot - QPointF(0, 0)).manhattanLength() < 1e-9);
        m.rotate(90, comp);
        CHECK((m.pivotPoint() - QPointF(0.2, 0.2)).manhattanLength() < 1e-9 && std::abs(m.angle(comp) - 90) < 1e-6);
        CHECK((m.corner(0) - QPointF(0.2, 0.2)).manhattanLength() < 1e-9); // it did not move: it turned about that corner
        m.resetCorners();
        CHECK((m.pivot - QPointF(0.5, 0.5)).manhattanLength() < 1e-9);
    }

    // A viewport can be turned: its region is a rectangle of the composition rotated about its center
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 32));
        QImage rb(64, 32, QImage::Format_RGB32); // left half red, right half blue
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) rb.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        rb.save(tmp + "/redblue_vp.png");
        const int v1 = e.viewports().first();
        e.setViewportSize(v1, QSize(32, 32));
        const int li = e.addLayer("img");
        CHECK(e.setLayerImage(li, tmp + "/redblue_vp.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 vid = e.layerId(v1);
        auto shot = [&](double angle) {
            {
                Engine::Lock lk(&e.mutex());
                Mapping &m = e.layer(e.indexOfId(vid))->mapping;
                m.setRect({QPointF(32, 16), 32, 32, angle}, QSize(64, 32));
            }
            for (int k = 0; k < 3; ++k) e.renderFrame();
            return e.grabViewport(vid);
        };
        QImage g = shot(0); // the middle square: red on its left, blue on its right
        CHECK(g.size() == QSize(32, 32) && g.pixelColor(2, 2).red() > 250 && g.pixelColor(29, 2).blue() > 250);
        g = shot(90); // turned a quarter: the halves are now top and bottom, the blue one on top
        CHECK(g.pixelColor(2, 2).blue() > 250 && g.pixelColor(2, 29).red() > 250 && g.pixelColor(29, 2).blue() > 250);
        {
            Engine::Lock lk(&e.mutex());
            const Mapping::Rect r = e.layer(e.indexOfId(vid))->mapping.rect(QSize(64, 32));
            CHECK(std::abs(r.angle - 90) < 1e-6 && std::abs(r.w - 32) < 1e-6 && std::abs(r.center.x() - 32) < 1e-6);
        }
        e.newProject();
    }
    } // compositing

    if (area("osc")) {
    // 4c. The Logs panel's events (audit P1): bounded in lines and in cost, whatever the traffic
    {
        EventLog log(8, 5); // 8 lines, 5 new lines a second
        const qint64 t0 = 1000000;
        // A fader sending its value: one line, its repeats counted, the last value shown
        for (int k = 0; k < 100; ++k) log.append(EventLog::OscIn, QStringLiteral("/fader/1  %1").arg(k), t0 + k);
        std::vector<EventLog::Entry> l = log.entries();
        CHECK(l.size() == 1 && l[0].repeats == 100 && l[0].message == "/fader/1  99" && l[0].time == t0 + 99);
        CHECK(log.counters().received == 100 && log.counters().merged == 99);
        // Another address, another channel: new lines; the same address a second later: a new line too
        log.append(EventLog::OscIn, "/fader/2  1", t0 + 200);
        log.append(EventLog::MidiIn, "/fader/2  1", t0 + 201);
        log.append(EventLog::OscIn, "/fader/1  5", t0 + 1300);
        CHECK(log.entries().size() == 4);
        // The rate: beyond 5 new lines in a second, counted, not kept
        for (int k = 0; k < 20; ++k) log.append(EventLog::OscOut, QStringLiteral("/out/%1").arg(k), t0 + 1400);
        CHECK(log.counters().discarded == 16 && log.entries().size() == 8); // 4 + the 4 left of this second's 5
        // The ring: the oldest lines leave it, in order
        for (int k = 0; k < 5; ++k) log.append(EventLog::OscOut, QStringLiteral("/later/%1").arg(k), t0 + 2500);
        l = log.entries();
        CHECK(l.size() == 8 && log.counters().overwritten == 5 && l.back().message == "/later/4");
        for (size_t k = 1; k < l.size(); ++k) CHECK(l[k].seq == l[k - 1].seq + 1);
        CHECK(EventLog::keyOf("/a/b  1, 2") == "/a/b" && EventLog::keyOf("MIDI · ch 1 · CC 7 = 0.5") == "MIDI · ch 1 · CC 7");
        // A hidden channel: not recorded
        log.setWanted(EventLog::Keyboard, false);
        const quint64 g = log.generation();
        log.append(EventLog::Keyboard, "Key Space = 1", t0 + 3000);
        CHECK(!log.wants(EventLog::Keyboard) && log.generation() == g);
        // The panel's rows follow the ring by sequence number: added, merged, removed, cleared
        LogModel model;
        CHECK(model.update(log.entries()) && model.rowCount() == 8);
        int inserted = 0, removed = 0, changed = 0, resets = 0;
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex &, int a, int b) { inserted += b - a + 1; });
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&](const QModelIndex &, int a, int b) { removed += b - a + 1; });
        QObject::connect(&model, &QAbstractItemModel::dataChanged, [&] { ++changed; });
        QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
        log.append(EventLog::OscOut, "/later/4  again", t0 + 2600); // merged into the last line
        CHECK(!model.update(log.entries()) && changed == 1 && inserted == 0 && removed == 0);
        CHECK(model.data(model.index(7, 2), Qt::DisplayRole).toString().contains(QStringLiteral("×2")));
        log.append(EventLog::OscIn, "/new/1", t0 + 4000);
        log.append(EventLog::OscIn, "/new/2", t0 + 4000);
        CHECK(model.update(log.entries()) && inserted == 2 && removed == 2 && resets == 0 && model.rowCount() == 8);
        CHECK(model.data(model.index(7, 2), Qt::DisplayRole).toString() == "/new/2");
        log.clear();
        model.update(log.entries());
        CHECK(model.rowCount() == 0 && resets == 0); // all removed from the start
        // Any thread at once: nothing lost from the counts, the ring never beyond its size
        EventLog shared(64, 1000000);
        std::vector<std::thread> writers;
        for (int w = 0; w < 4; ++w)
            writers.emplace_back([&shared, w] {
                for (int k = 0; k < 5000; ++k) shared.append(EventLog::OscIn, QStringLiteral("/w%1/%2").arg(w).arg(k % 7));
            });
        for (std::thread &t : writers) t.join();
        const EventLog::Counters c = shared.counters();
        CHECK(c.received == 20000 && shared.entries().size() <= 64 && c.received == c.merged + c.discarded + c.overwritten + shared.entries().size());
        // OSC in: recorded where it is received, and not at all when its channel is hidden
        EventLog &app = EventLog::instance();
        app.clear();
        OscServer srv(&e);
        srv.handleMessage({"/nowhere", "f", {0.5}});
        CHECK(app.entries().size() == 1 && app.entries()[0].message == "/nowhere  0.5" &&
              app.entries()[0].channel == EventLog::OscIn);
        app.setWanted(EventLog::OscIn, false);
        srv.handleMessage({"/nowhere/else", "f", {0.5}});
        CHECK(app.entries().size() == 1);
        app.setWanted(EventLog::OscIn, true);
        app.clear();
    }
    // 4c'. Frame timing (audit P2): frame intervals, missed deadlines, input to frame — from fixed times, then live
    {
        FrameTiming t;
        const qint64 ms = 1000000, period = 16 * ms;
        t.noteInput(FrameTiming::Osc, 5 * ms);   // before the first frame: shown by it
        t.noteInput(FrameTiming::Osc, 7 * ms);   // a later one in the same frame: the earliest is counted
        t.noteFrame(10 * ms, 14 * ms, 15 * ms, period, true);
        t.noteInput(FrameTiming::Cue, 27 * ms);  // during the next frame (started at 26): the one after shows it
        t.noteFrame(26 * ms, 29 * ms, 30 * ms, period, true);
        t.noteFrame(80 * ms, 82 * ms, 90 * ms, period, false); // 54 ms after the one before: deadlines missed
        FrameTiming::Stats st = t.stats();
        // Interval telemetry must have a *window-local* maximum: an early spike
        // must not mask later seconds. Late events are bounded, timestamped data.
        const FrameTiming::Window firstWindow = t.takeWindow();
        CHECK(firstWindow.stats.frames == 3 && firstWindow.stats.interval.count == 2);
        CHECK(std::abs(firstWindow.stats.interval.max - 54.0) < 1e-9);
        CHECK(firstWindow.eventCount == 1 && firstWindow.events[0].atNs == 90 * ms);
        CHECK(std::abs(firstWindow.events[0].intervalMs - 54.0) < 1e-9);
        CHECK(t.takeWindow().stats.frames == 0);
        t.noteFrame(100 * ms, 102 * ms, 104 * ms, period, true);
        const FrameTiming::Window secondWindow = t.takeWindow();
        CHECK(secondWindow.stats.frames == 1 && secondWindow.stats.interval.count == 1);
        CHECK(std::abs(secondWindow.stats.interval.max - 20.0) < 1e-9 && secondWindow.eventCount == 0);
        // The journal (what the show did) and the inputs are per window too, bounded, and handed over once
        t.noteInput(FrameTiming::Osc);
        t.noteInput(FrameTiming::Osc);
        t.noteInput(FrameTiming::Cue);
        t.noteEvent(QStringLiteral("snapshot_recall"), QStringLiteral("Scene 2"));
        for (size_t k = 0; k < FrameTiming::kJournalPerWindow + 3; ++k) t.noteEvent(QStringLiteral("seek"), QString());
        const FrameTiming::Window journalWindow = t.takeWindow();
        CHECK(journalWindow.inputs[FrameTiming::Osc] == 2 && journalWindow.inputs[FrameTiming::Cue] == 1);
        CHECK(journalWindow.journal.size() == FrameTiming::kJournalPerWindow && journalWindow.journalLost == 4);
        CHECK(journalWindow.journal.front().kind == "snapshot_recall" && journalWindow.journal.front().detail == "Scene 2");
        const FrameTiming::Window emptyWindow = t.takeWindow();
        CHECK(emptyWindow.journal.empty() && emptyWindow.inputs[FrameTiming::Osc] == 0);
        CHECK(st.frames == 3 && st.presented == 2 && st.missed == 1);
        CHECK(st.interval.count == 2 && std::abs(st.interval.max - 54.0) < 1e-9);
        CHECK(st.inputToFrame[FrameTiming::Osc].count == 1 && std::abs(st.inputToFrame[FrameTiming::Osc].max - 10.0) < 1e-9);
        CHECK(st.inputToFrame[FrameTiming::Cue].count == 1 && std::abs(st.inputToFrame[FrameTiming::Cue].max - 63.0) < 1e-9);
        CHECK(std::abs(st.present.max - 8.0) < 1e-9 && std::abs(st.compute.max - 4.0) < 1e-9);
        // Percentiles: the upper edge of the bucket, never beyond the largest value
        FrameTiming::Histogram h;
        for (int k = 0; k < 99; ++k) h.add(3.0);
        h.add(40.0);
        CHECK(h.percentile(0.5) == 4.0 && h.percentile(0.99) == 4.0 && h.percentile(1.0) == 40.0 && h.max == 40.0);
        const QJsonObject j = st.json();
        CHECK(j.value("missed_deadlines").toDouble() == 1 && j.value("osc_to_frame").toObject().value("max_ms").toDouble() == 10.0);
        t.reset();
        CHECK(t.stats().frames == 0);
        // The pace without a vertical sync: absolute deadlines, a late frame caught up, far behind: from now
        {
            qint64 next = -1;
            const qint64 P = 20 * ms;
            CHECK(FramePacer::next(next, 0, 2 * ms, P) == 18 * ms); // the first: one period after its start
            CHECK(FramePacer::next(next, 20 * ms, 23 * ms, P) == 17 * ms); // on time
            CHECK(FramePacer::next(next, 45 * ms, 47 * ms, P) == 13 * ms); // woke up 5 ms late: the next one is not
            CHECK(FramePacer::next(next, 60 * ms, 85 * ms, P) == 0 && next == 80 * ms); // late: no sleep, schedule kept
            CHECK(FramePacer::next(next, 85 * ms, 140 * ms, P) == 0 && next == 140 * ms); // far behind: from now on
            CHECK(FramePacer::next(next, 140 * ms, 141 * ms, P) == 19 * ms);
            CHECK(FramePacer::resync(next, 300 * ms, P) == 0 && next == 300 * ms); // a vertical sync paced it
            CHECK(FramePacer::next(next, 301 * ms, 302 * ms, P) == 18 * ms);
            CHECK(FramePacer::next(next, 0, 0, 0) == 0); // no period: no wait
        }
        // The histogram reads where frames land: 50 Hz frames are not "25 ms"
        {
            FrameTiming::Histogram f;
            for (int k = 0; k < 100; ++k) f.add(20.0 + (k % 3) * 0.1);
            CHECK(f.percentile(0.5) <= 20.5 && f.percentile(0.99) <= 20.5);
        }
        // The engine: each frame counted; an OSC message that changed a value, until the frame that has it
        e.resetFrameTiming();
        OscServer srv(&e);
        const quint64 lid = e.layerId(e.addLayer("Timed"));
        (void)lid;
        e.renderFrame();
        CHECK(srv.handleMessage({"/layer/Timed/opacity", "f", {0.5}}));
        e.renderFrame();
        e.renderFrame();
        const FrameTiming::Stats live = e.frameTiming();
        CHECK(live.frames == 3 && live.inputToFrame[FrameTiming::Osc].count == 1 && live.interval.count == 2);
        // Read over OSCQuery as one JSON string, reset over OSC
        int status = 0;
        const QJsonObject v = QJsonDocument::fromJson(srv.httpGet("/composition/timing?VALUE", &status)).object();
        const QJsonObject timing = QJsonDocument::fromJson(v.value("VALUE").toArray().at(0).toString().toUtf8()).object();
        CHECK(status == 200 && timing.value("frames").toDouble() == 3 && timing.contains("osc_to_frame") &&
              timing.contains("cue_to_frame") && timing.contains("outputs_dropped"));
        CHECK(timing.value("outputs").isArray() && timing.value("pace_hz").toDouble() > 0 &&
              timing.value("cue_late").toObject().contains("p99_ms"));
        CHECK(timing.value("cue_capture").toObject().contains("p99_ms") &&
              timing.value("cue_recall").toObject().contains("p95_ms") &&
              timing.value("cue_execution").toObject().contains("max_ms"));
        CHECK(srv.handleMessage({"/composition/timing/reset", "N", {QVariant()}}) && e.frameTiming().frames == 0);
        CHECK(e.sequencer().cueTiming().fired == 0 && e.sequencer().cueWorkTiming().total.count == 0);
    }
    // 4d. OSC and OSCQuery
    {
        osc::Message m{"/a/b", "ifsTFNd", {7, 0.5, QStringLiteral("hey"), true, false, QVariant(), 2.25}};
        std::vector<osc::Message> back;
        CHECK(osc::decode(osc::encode(m), back) && back.size() == 1);
        CHECK(back[0].address == "/a/b" && back[0].types == "ifsTFNd" && back[0].args[0].toInt() == 7 &&
              back[0].args[1].toDouble() == 0.5 && back[0].args[2].toString() == "hey" && back[0].args[3].toBool() &&
              !back[0].args[4].toBool() && back[0].args[6].toDouble() == 2.25);
        QByteArray bundle("#bundle\0\0\0\0\0\0\0\0\1", 16);
        for (const QByteArray &part : {osc::encode({"/x", "i", {1}}), osc::encode({"/y", "s", {QStringLiteral("z")}})}) {
            char n[4];
            qToBigEndian(qint32(part.size()), n);
            bundle += QByteArray(n, 4) + part;
        }
        back.clear();
        CHECK(osc::decode(bundle, back) && back.size() == 2 && back[1].address == "/y");
        CHECK(osc::match("/layer/*/opacity", "/layer/A_b/opacity") && !osc::match("/layer/*/opacity", "/layer/a/b/opacity"));
        CHECK(osc::match("/l/{foo,bar}/[a-c]?", "/l/bar/bx") && !osc::match("/l/{foo,bar}/[!a-c]", "/l/foo/a"));
        CHECK(osc::match("/l/*o*o*", "/l/foo") && osc::match("/l/f**", "/l/f") && !osc::match("/l/*x*", "/l/foo"));
        CHECK(osc::match("/l/{a,ab}c", "/l/abc") && osc::match("/l/x{}y", "/l/xy") && !osc::match("/l/[ab", "/l/a"));
        { // Runs of '*' (found by the fuzzer): exponential for a backtracking matcher, linear here
            QElapsedTimer t;
            t.start();
            const QString stars = QString(40, '*');
            CHECK(!osc::match("/****/" + stars + "/" + stars + "777777777", "/layer/a/opacity"));
            CHECK(!osc::match("/l/" + stars + "a" + stars + "a" + stars + "b", "/l/" + QString(200, 'a')));
            CHECK(osc::match("/l/" + stars + "a" + stars + "a" + stars + "b", "/l/" + QString(200, 'a') + "b"));
            CHECK(t.elapsed() < 500);
        }
        // Bounds apply to attacker-controlled wildcard input, including
        // literal paths (which otherwise bypass the tokenized matching path).
        CHECK(!osc::match(QStringLiteral("/") + QString(8192, 'a'), "/a"));
        CHECK(!osc::match("/a", QStringLiteral("/") + QString(8192, 'a')));
        CHECK(!osc::match(QStringLiteral("/") + QString(4097, '*'), "/a"));
        CHECK(osc::match("/l/*/opacity", "/l/Video/opacity"));
        CHECK(osc::safeName(" My layer/1 ") == "My_layer_1");
        // Hostile packets (lengths read from the network): refused, never read out of bounds
        {
            auto be32 = [](qint32 v) {
                char n[4];
                qToBigEndian(v, n);
                return QByteArray(n, 4);
            };
            std::vector<osc::Message> none;
            // A blob announcing nearly 2 GiB, then an int: the length must not wrap the read position
            CHECK(!osc::decode(QByteArray("/a\0\0,bi\0", 8) + be32(0x7ffffffc) + be32(1), none));
            CHECK(!osc::decode(QByteArray("/a\0\0,b\0\0", 8) + be32(-4), none)); // negative
            CHECK(!osc::decode(QByteArray("/a\0\0,b\0\0", 8) + be32(64) + QByteArray(8, 'x'), none)); // truncated
            // A bundle element as large as possible, then another one
            CHECK(!osc::decode(QByteArray("#bundle\0", 8) + QByteArray(8, '\0') + be32(0x7ffffff0) + be32(8), none));
            CHECK(!osc::decode(QByteArray("#bundle\0", 8) + QByteArray(8, '\0') + be32(0), none)); // empty element
            // Nested deeper than 8 bundles
            QByteArray deep = osc::encode({"/x", "i", {1}});
            for (int k = 0; k < 10; ++k) deep = QByteArray("#bundle\0", 8) + QByteArray(8, '\0') + be32(qint32(deep.size())) + deep;
            CHECK(!osc::decode(deep, none));
            // A small, structurally valid bundle must not schedule thousands
            // of OSC messages (including when messages are nested).
            QByteArray flood("#bundle\0", 8);
            flood += QByteArray(8, '\0');
            const QByteArray item = osc::encode({"/x", "", {}});
            for (int k = 0; k < 1100; ++k) flood += be32(qint32(item.size())) + item;
            std::vector<osc::Message> flooded;
            CHECK(!osc::decode(flood, flooded) && flooded.size() <= 1024);
            // Strings without their end, an address alone, a packet too large
            CHECK(!osc::decode(QByteArray("/abc"), none));
            CHECK(!osc::decode(QByteArray("/a\0\0,s\0\0abc", 11), none));
            CHECK(osc::decode(QByteArray("/a\0\0", 4), none) && none.back().args.isEmpty());
            CHECK(!osc::decode(QByteArray("/a\0\0", 4) + QByteArray(2 << 20, '\0'), none));
            // The other argument types travel both ways (character, color, MIDI, time tag)
            osc::Message more{"/m", "crmt", {QStringLiteral("Z"), 0x11223344LL, 0x00904064LL, 42LL}};
            std::vector<osc::Message> back2;
            CHECK(osc::decode(osc::encode(more), back2) && back2.size() == 1 && back2[0].args[0].toString() == "Z" &&
                  back2[0].args[1].toLongLong() == 0x11223344LL && back2[0].args[3].toLongLong() == 42);
        }

        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        const int li = e.addLayer("My layer");
        e.setLayerImage(li, tmp + "/redblue.png", &err);
        e.addEffect(li, isf + "/effects/FlipCrop.fs", &err);
        const int gi = e.addGroup("G", V + 0);
        e.setStructure(tree::intoGroup(e.structure(), {e.layerId(V + 1)}, e.layerId(gi)));
        {
            // Listening on this computer only: reached on 127.0.0.1; an address that is not one is refused
            OscServer local(&e);
            CHECK(local.start(0, 0, "test", false, "127.0.0.1") && local.oscPort() > 0);
            QUdpSocket sender;
            const float before = e.layer(V + 1)->opacity;
            sender.writeDatagram(osc::encode({"/layer/G/layer/My_layer/opacity", "f", {0.125}}), QHostAddress::LocalHost,
                                 local.oscPort());
            QElapsedTimer t;
            t.start();
            while (std::abs(e.layer(V + 1)->opacity - 0.125f) > 1e-6 && t.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            CHECK(std::abs(e.layer(V + 1)->opacity - 0.125f) < 1e-6);
            e.layer(V + 1)->opacity = before;
            OscServer wrong(&e);
            CHECK(!wrong.start(0, 0, "test", false, "not-an-address") && wrong.status().contains("not an address"));
        }
        OscServer server(&e);
        CHECK(server.start(0, 0, "test", false));
        CHECK(server.oscPort() > 0 && server.queryPort() > 0);
        const QString L = "/layer/G/layer/My_layer";
        CHECK(server.handleMessage({L + "/opacity", "f", {0.25}}) && std::abs(e.layer(V + 1)->opacity - 0.25f) < 1e-6);
        CHECK(server.handleMessage({"/layer/G/layer/*/opacity", "f", {0.5}}) && std::abs(e.layer(V + 1)->opacity - 0.5f) < 1e-6);
        CHECK(server.handleMessage({L + "/add", "fff", {0.1, 0.2, 0.3}}) && std::abs(e.layer(V + 1)->color.add[2] - 0.3f) < 1e-6);
        CHECK(server.handleMessage({L + "/temp", "f", {-2000.0}}) && e.layer(V + 1)->color.temp == -2000.0f);
        CHECK(server.handleMessage({L + "/tint", "f", {500.0}}) && e.layer(V + 1)->color.tint == 100.0f); // clipped
        CHECK(server.handleMessage({L + "/roi/left", "f", {0.25}}) && std::abs(e.layer(V + 1)->roi.left() - 0.25) < 1e-9);
        CHECK(server.handleMessage({L + "/fx/FlipCrop/param/flipH", "T", {true}}));
        CHECK(e.layer(V + 1)->effects[0]->inputs()[1].bValue || e.layer(V + 1)->effects[0]->inputs()[2].bValue);
        CHECK(server.handleMessage({L + "/fx/enable", "F", {false}}) && !e.layer(V + 1)->effectsEnabled);
        CHECK(server.handleMessage({"/layer/G/opacity", "i", {0}}) && e.layer(V + 0)->opacity == 0.0f);
        CHECK(server.handleMessage({"/composition/blackout", "T", {true}}) && e.blackout());
        server.handleMessage({"/composition/blackout", "i", {0}});
        CHECK(!e.blackout());
        // Locked group: its member refuses edits, visibility still works
        CHECK(server.handleMessage({"/layer/G/locked", "T", {true}}) && e.isLocked(V + 1));
        CHECK(!server.handleMessage({L + "/opacity", "f", {0.9}}) && std::abs(e.layer(V + 1)->opacity - 0.5f) < 1e-6);
        CHECK(server.handleMessage({L + "/enable", "F", {false}}) && !e.layer(V + 1)->enabled);
        server.handleMessage({"/layer/G/locked", "F", {false}});
        // A viewport: position and size in composition pixels (no scale)
        {
            const int vi = e.indexOfId(e.mainViewportId());
            const QString VP = "/viewport/" + osc::safeName(e.layer(vi)->name);
            CHECK(server.handleMessage({VP + "/width", "f", {32}}) && server.handleMessage({VP + "/height", "f", {16}}));
            CHECK(server.handleMessage({VP + "/position/x", "f", {10}}) && server.handleMessage({VP + "/position/y", "f", {8}}));
            const QRectF bb = e.layer(vi)->mapping.bounds();
            CHECK(std::abs(bb.width() * 64 - 32) < 1e-6 && std::abs(bb.height() * 32 - 16) < 1e-6);
            CHECK(std::abs(bb.center().x() * 64 - 10) < 1e-6 && std::abs(bb.center().y() * 32 - 8) < 1e-6);
            CHECK(!server.handleMessage({VP + "/scale", "ff", {50, 50}}));
            // The pivot in % of the layer: its top left corner, (-6, 0) px; turning about it leaves it there
            CHECK(server.handleMessage({VP + "/pivot", "ff", {0, 0}}));
            CHECK(server.handleMessage({VP + "/rotation", "f", {90}}));
            {
                const Mapping &pm = e.layer(vi)->mapping;
                CHECK(pm.pivot.isNull() && (pm.pivotPoint() - QPointF(-6.0 / 64, 0)).manhattanLength() < 1e-6 &&
                      std::abs(pm.angle(e.compositionSize()) - 90) < 1e-6);
            }
            QStringList paths;
            for (const auto &p : e.animatableParams(e.layerId(vi))) paths << p.path;
            CHECK(paths.contains("width") && paths.contains("height") && paths.contains("position/x") &&
                  !paths.contains("scale/x") && paths.contains("rotation") &&
                  paths.contains("pivot/x") && paths.contains("pivot/y"));
            e.layer(vi)->mapping.resetCorners();
        }
        // Renaming changes the address
        CHECK(server.handleMessage({L + "/name", "s", {QStringLiteral("Front wall")}}) && e.layer(V + 1)->name == "Front wall");
        int status = 0;
        QJsonObject v = QJsonDocument::fromJson(server.httpGet("/layer/G/layer/Front_wall/opacity?VALUE", &status)).object();
        CHECK(status == 200 && v.value("VALUE").toArray().at(0).toDouble() == 0.5);
        server.httpGet(L + "/opacity", &status);
        CHECK(status == 404);
        // One vocabulary: every value the layer's state keeps is at that same address in OSC
        {
            const QJsonObject params = e.layerJson(V + 1).value("params").toObject();
            int missing = 0;
            for (auto it = params.begin(); it != params.end(); ++it) {
                server.httpGet("/layer/G/layer/Front_wall/" + it.key(), &status);
                if (status != 200) ++missing, qWarning("no OSC node for %s", qPrintable(it.key()));
            }
            CHECK(params.size() > 20 && params.contains("position/x") && params.contains("corner/top_left/x") && missing == 0);
        }
        QJsonObject root = QJsonDocument::fromJson(server.httpGet("/", &status)).object();
        const QJsonObject op = root["CONTENTS"].toObject()["layer"].toObject()["CONTENTS"].toObject()["G"].toObject()["CONTENTS"]
                                   .toObject()["layer"].toObject()["CONTENTS"].toObject()["Front_wall"].toObject()["CONTENTS"]
                                   .toObject()["opacity"].toObject();
        CHECK(op.value("TYPE").toString() == "f" && op.value("ACCESS").toInt() == 3 &&
              op.value("RANGE").toArray().at(0).toObject().value("MAX").toDouble() == 1.0);
        const QJsonObject compo = root["CONTENTS"].toObject()["composition"].toObject()["CONTENTS"].toObject();
        CHECK(!root["CONTENTS"].toObject().contains("master"));
        for (const char *k : {"opacity", "volume", "blackout", "width", "height", "fps"}) CHECK(compo.contains(k));
        CHECK(!compo.contains("mute") && compo["blackout"].toObject()["CONTENTS"].toObject().contains("fade"));
        CHECK(server.handleMessage({"/composition/opacity", "f", {0.4}}) && std::abs(e.compositionOpacityTarget() - 0.4) < 1e-3);
        e.fadeCompositionOpacity(1.0, 0);
        CHECK(server.handleMessage({"/composition/fps", "f", {30.0}}) && e.effectiveRender().frameRate == 30.0);
        e.setRenderSettings(Engine::RenderSettings());
        // Hierarchy, lowercase, and an "enable" child for every switch, over the whole tree
        {
            int bad = 0, nodes = 0;
            std::function<void(const QJsonObject &, const QString &)> walk = [&](const QJsonObject &n, const QString &parent) {
                const QJsonObject c = n.value("CONTENTS").toObject();
                for (auto it = c.begin(); it != c.end(); ++it) {
                    ++nodes;
                    const QString seg = it.key();
                    const bool userName = parent == "layer" || parent == "viewport" || parent == "fx" || parent == "param"; // names: layers, viewports, effects, shader parameters
                    if (!userName && seg != seg.toLower()) { ++bad; qWarning("not lowercase: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString())); }
                    if (!userName && seg.endsWith("s") && seg != "fps") { ++bad; qWarning("plural: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString())); }
                    if (!userName && (seg.endsWith("enabled") || seg.endsWith("Enabled") || seg.endsWith("On") || seg == "master")) {
                        ++bad;
                        qWarning("switch not named enable: %s", qPrintable(it.value().toObject().value("FULL_PATH").toString()));
                    }
                    walk(it.value().toObject(), userName ? QStringLiteral("*") : seg);
                }
            };
            walk(root, QString());
            CHECK(nodes > 50 && bad == 0);
            const QString LF = "/layer/G/layer/Front_wall";
            CHECK(server.handleMessage({LF + "/tint/enable", "T", {true}}) && e.layer(V + 1)->color.tintOn);
            CHECK(server.handleMessage({LF + "/mask/invert", "T", {true}}) && e.layer(V + 1)->color.maskInvert);
            CHECK(server.handleMessage({LF + "/blend_mode", "s", {QStringLiteral("add")}}) && e.layer(V + 1)->blend == BlendMode::Add);
            CHECK(server.handleMessage({LF + "/rotation", "f", {30.0}}) &&
                  std::abs(e.layer(V + 1)->mapping.angle(e.compositionSize()) - 30.0) < 0.01);
            e.layer(V + 1)->blend = BlendMode::Normal;
        }
        const QJsonObject host = QJsonDocument::fromJson(server.httpGet("/?HOST_INFO", &status)).object();
        CHECK(host.value("OSC_PORT").toInt() == server.oscPort() && host.value("EXTENSIONS").toObject().value("LISTEN").toBool());

        auto pump = [&](const std::function<bool()> &done) {
            QElapsedTimer t;
            t.start();
            while (!done() && t.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(2);
            }
            return done();
        };
        // Real UDP message
        const QString F = "/layer/G/layer/Front_wall";
        QUdpSocket udp;
        udp.writeDatagram(osc::encode({F + "/opacity", "f", {0.75}}), QHostAddress::LocalHost, server.oscPort());
        CHECK(pump([&] { return std::abs(e.layer(V + 1)->opacity - 0.75f) < 1e-6; }));
        // Real HTTP request
        QTcpSocket http;
        http.connectToHost(QHostAddress::LocalHost, server.queryPort());
        CHECK(pump([&] { return http.state() == QAbstractSocket::ConnectedState; }));
        http.write(QByteArray("GET ") + (F + "/opacity?VALUE").toUtf8() + " HTTP/1.1\r\nHost: x\r\n\r\n");
        QByteArray reply;
        pump([&] {
            reply += http.readAll();
            return reply.contains("VALUE");
        });
        CHECK(reply.startsWith("HTTP/1.1 200") && reply.contains("[0.75]"));
        // WebSocket: LISTEN, then a change is pushed as a binary OSC message
        QTcpSocket ws;
        ws.connectToHost(QHostAddress::LocalHost, server.queryPort());
        CHECK(pump([&] { return ws.state() == QAbstractSocket::ConnectedState; }));
        ws.write("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        QByteArray wsIn;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            return wsIn.contains("\r\n\r\n");
        }));
        CHECK(wsIn.contains("101") && wsIn.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
        wsIn.clear();
        auto wsSend = [&](int opcode, const QByteArray &payload) { // masked client frame
            QByteArray f;
            f.append(char(0x80 | opcode));
            f.append(char(0x80 | payload.size()));
            const char mask[4] = {1, 2, 3, 4};
            f.append(mask, 4);
            for (int k = 0; k < payload.size(); ++k) f.append(char(payload[k] ^ mask[k & 3]));
            ws.write(f);
        };
        wsSend(1, QByteArray("{\"COMMAND\":\"LISTEN\",\"DATA\":\"") + (F + "/opacity").toUtf8() + "\"}");
        QCoreApplication::processEvents();
        wsSend(2, osc::encode({F + "/opacity", "f", {0.125}})); // an OSC message through the WebSocket
        osc::Message got;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            while (wsIn.size() >= 2) {
                const int len = wsIn[1] & 0x7F;
                if (wsIn.size() < 2 + len) break;
                std::vector<osc::Message> ms;
                if (osc::decode(wsIn.mid(2, len), ms) && !ms.empty()) got = ms.back();
                wsIn.remove(0, 2 + len);
            }
            return got.args.value(0).toDouble() == 0.125;
        }));
        CHECK(got.address == F + "/opacity" && got.types == "f");
        // A new layer: the clients are told to fetch the tree again
        e.addLayer("Late");
        bool changed = false;
        CHECK(pump([&] {
            wsIn += ws.readAll();
            changed |= wsIn.contains("PATH_CHANGED");
            return changed;
        }));
        {
            int st = 0;
            const QJsonObject layers = QJsonDocument::fromJson(server.httpGet("/layer", &st)).object();
            CHECK(layers["CONTENTS"].toObject().contains("Late") && layers["CONTENTS"].toObject()["Late"].toObject()["DESCRIPTION"].toString() == "Late");
        }
        ws.close();
        http.close();
        server.stop();

        // Zeroconf responder: answers to a PTR question for _oscjson._tcp.local (without the network)
        {
            Zeroconf z;
            z.start("Pinol (test)", {{"_oscjson._tcp", 5678, {{"txtvers", "1"}}}, {"_osc._udp", 1234, {}}});
            QByteArray q;
            const char head[12] = {0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
            q.append(head, 12);
            for (const char *label : {"_oscjson", "_tcp", "local"}) {
                q.append(char(std::strlen(label)));
                q.append(label);
            }
            q.append('\0');
            q.append("\x00\x0c\x00\x01", 4); // PTR, IN
            bool uni = false;
            const QByteArray a = z.answerQuery(q, &uni);
            CHECK(a.size() > 12 && quint8(a[0]) == 0x12 && quint8(a[2]) == 0x84 && a.contains("Pinol (test)"));
            CHECK(a.contains(QByteArray("\x16\x2e", 2))); // SRV port 5678 in the additional records
            CHECK(a.contains(z.hostName().toUtf8()) && a.contains("txtvers=1"));
            q[q.size() - 3] = 0x10; // TXT of another name: no answer
            CHECK(z.answerQuery(QByteArray("\x00\x00\x84\x00", 4) + q.mid(4)).isEmpty()); // a response is ignored
            CHECK(z.announcement(true).contains("_osc"));
            z.stop();
        }
        e.newProject();
    }
    } // osc

    if (area("snapshots")) {
    // 4e. Snapshots: capture, recall (instant and with a fade), recreation, lock, save / load
    {
        e.newProject();
        e.setCompositionSize(QSize(64, 32));
        const int a = e.addLayer("A");
        e.setLayerImage(a, tmp + "/redblue.png", &err);
        const int b = e.addLayer("B", V + 1);
        e.setLayerIsf(b, isf + "/generators/SolidColor.fs", &err);
        e.layer(V + 0)->opacity = 0.8f;
        e.layer(V + 1)->enabled = false;
        Engine::Snapshot m;
        m.name = "Look 1";
        m.fade = 0;
        m.layers = e.captureLayers();
        m.thumbnail = QImage(16, 9, QImage::Format_RGB32);
        m.thumbnail.fill(Qt::red);
        CHECK(e.addSnapshot(m) == 0 && e.snapshotCount() == 1);
        // Change things, then recall
        e.layer(V + 0)->opacity = 0.2f;
        e.layer(V + 0)->color.temp = 1000;
        e.layer(V + 0)->mapping.setCorner(0, QPointF(0.3, 0.3));
        e.layer(V + 1)->enabled = true;
        e.addEffect(V + 0, isf + "/effects/FlipCrop.fs", &err);
        e.recallSnapshot(0);
        CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6 && e.layer(V + 0)->color.temp == 0.0f);
        CHECK(QLineF(e.layer(V + 0)->mapping.corner(0), QPointF(0.3, 0.3)).length() > 1e-6); // the corner came back
        CHECK(e.layer(V + 0)->effects.empty() && !e.layer(V + 1)->enabled);
        // With a fade: halfway between, then the target; a layer becoming visible fades in from 0
        Engine::Snapshot m2 = m;
        m2.name = "Look 2";
        m2.fade = 0.4;
        e.layer(V + 0)->opacity = 0.0f;
        e.layer(V + 1)->enabled = true;
        m2.layers = e.captureLayers();
        e.addSnapshot(m2);
        e.recallSnapshot(0); // back to look 1, at once
        e.recallSnapshot(1);
        CHECK(e.isFading() && e.layer(V + 1)->enabled && e.layer(V + 1)->opacity < 0.05f);
        QElapsedTimer ft;
        ft.start();
        bool sawMiddle = false;
        while (e.isFading() && ft.elapsed() < 3000) {
            e.renderFrame();
            sawMiddle |= e.layer(V + 0)->opacity > 0.1f && e.layer(V + 0)->opacity < 0.7f;
            QThread::msleep(10);
        }
        CHECK(!e.isFading() && sawMiddle && e.layer(V + 0)->opacity == 0.0f && std::abs(e.layer(V + 1)->opacity - 1.0f) < 1e-6);
        // A layer removed since is recreated; a locked layer is left alone
        e.removeLayer(V + 1);
        e.layer(V + 0)->locked = true;
        e.layer(V + 0)->opacity = 0.5f;
        e.recallSnapshot(0);
        CHECK(e.layerCount() == V + 2 && e.layer(V + 1)->name == "B" && std::abs(e.layer(V + 0)->opacity - 0.5f) < 1e-6);
        e.layer(V + 0)->locked = false;
        // Undo of a recall
        {
            QUndoStack undo;
            e.layer(V + 0)->opacity = 0.33f;
            undo.push(new cmd::RecallSnapshot(&e, 0));
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6);
            undo.undo();
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.33f) < 1e-6);
        }
        // Each value on its own time: opacity cut, temperature 2 s, the others on the snapshot's fade (1 s)
        {
            const int ti = e.addLayer("Timed", e.layerCount());
            const quint64 tid = e.layerId(ti);
            auto L = [&] { return e.layer(e.indexOfId(tid)); };
            L()->opacity = 1.0f;
            L()->color.temp = 1000;
            L()->roi = QRectF(0, 0, 0.5, 1);
            QJsonObject state = e.layerJson(e.indexOfId(tid));
            state["timing"] = QJsonObject{{"opacity", 0}, {"temp", 2.0}};
            L()->opacity = 0.0f;
            L()->color.temp = 0;
            L()->roi = QRectF(0, 0, 1, 1);
            e.applyLayers(QJsonArray{state}, 1.0);
            CHECK(L()->opacity == 1.0f && L()->color.temp < 1 && std::abs(L()->roi.width() - 1.0) < 1e-9); // the cut, at once
            e.advanceFades(0.5);
            CHECK(L()->roi.width() < 0.99 && L()->roi.width() > 0.51 && L()->color.temp > 1 && L()->color.temp < 400);
            e.advanceFades(0.6); // 1.1 s: the fade is over, the temperature is not
            CHECK(std::abs(L()->roi.width() - 0.5) < 1e-9 && L()->color.temp > 400 && L()->color.temp < 999 && e.isFading());
            e.advanceFades(1.0);
            CHECK(std::abs(L()->color.temp - 1000) < 1e-3 && !e.isFading());
            // Hidden by a cut while the rest fades: at once
            state["enable"] = false;
            e.applyLayers(QJsonArray{state}, 1.0);
            CHECK(!L()->enabled && L()->opacity == 1.0f);
            // A time of its own with no snapshot fade at all
            L()->enabled = true;
            L()->color.temp = 0;
            e.applyLayers(QJsonArray{state.value("enable").toBool() ? state : [&] { QJsonObject x = state; x["enable"] = true; return x; }()}, 0.0);
            CHECK(L()->color.temp < 1 && e.isFading());
            e.advanceFades(2.0);
            CHECK(std::abs(L()->color.temp - 1000) < 1e-3 && !e.isFading());
            // A group's time: for all of its values that have none of their own
            CHECK(Engine::timingKeyOf(QJsonObject{{"roi", 1.0}}, "roi/right") == "roi" &&
                  Engine::timingKeyOf(QJsonObject{{"roi", 1.0}, {"roi/right", 0.0}}, "roi/right") == "roi/right" &&
                  Engine::timingKeyOf(QJsonObject{{"fx/blur", 1.0}}, "fx/blur/param/radius") == "fx/blur" &&
                  Engine::timingKeyOf(QJsonObject{{"position", 1.0}}, "position_x").isEmpty());
            // The pivot: on the snapshot's fade like the others, or on a time of its own (its group's here)
            {
                QJsonObject ps = e.layerJson(e.indexOfId(tid));
                QJsonObject pp = ps.value("params").toObject();
                pp["pivot/x"] = 0.0;
                pp["pivot/y"] = 0.0;
                ps["params"] = pp;
                ps.remove("timing");
                L()->mapping.pivot = QPointF(0.5, 0.5);
                e.applyLayers(QJsonArray{ps}, 1.0);
                CHECK((L()->mapping.pivot - QPointF(0.5, 0.5)).manhattanLength() < 1e-9 && e.isFading());
                e.advanceFades(0.5);
                CHECK(L()->mapping.pivot.x() > 0.05 && L()->mapping.pivot.x() < 0.45);
                e.advanceFades(1.0);
                CHECK((L()->mapping.pivot - QPointF(0, 0)).manhattanLength() < 1e-9);
                ps["timing"] = QJsonObject{{"pivot", 2.0}};
                pp["pivot/x"] = 100.0;
                pp["pivot/y"] = 100.0;
                ps["params"] = pp;
                e.applyLayers(QJsonArray{ps}, 0.0);
                CHECK((L()->mapping.pivot - QPointF(0, 0)).manhattanLength() < 1e-9 && e.isFading());
                e.advanceFades(1.0);
                CHECK(L()->mapping.pivot.x() > 0.05 && L()->mapping.pivot.x() < 0.95);
                e.advanceFades(1.5);
                CHECK((L()->mapping.pivot - QPointF(1, 1)).manhattanLength() < 1e-9 && !e.isFading());
            }
            // Text generator: the snapshot's text is typed over its time (erased back to what both share, then
            // typed); its style moves on its own times
            {
                const int xi = e.addLayer("Words", e.layerCount());
                const quint64 xid = e.layerId(xi);
                auto X = [&] { return e.layer(e.indexOfId(xid)); };
                CHECK(e.setLayerText(xi) && X()->type == SourceType::Text);
                e.setLayerTextContent(xi, "Hello");
                e.editLayerText(xi, [](Layer &l) {
                    l.text.size = 40;
                    l.text.color = Qt::white;
                });
                QJsonObject st = e.layerJson(e.indexOfId(xid));
                QJsonObject src = st.value("params").toObject();
                CHECK(st.value("source").toObject().value("type") == "text" && src.value("text/color/r").isDouble() &&
                      src.value("text/align") == "center");
                src["text/content"] = "Help me";
                src["text/size"] = 80;
                src["text/bold"] = true;
                src["text/color/g"] = 0.0;
                src["text/color/b"] = 0.0;
                st["params"] = src;
                st["timing"] = QJsonObject{{"text/size", 2.0}, {"text/color", 0}};
                e.applyLayers(QJsonArray{st}, 1.0);
                CHECK(X()->text.content == "Help me" && X()->text.shown() == "Hello" && X()->text.bold);
                CHECK(X()->text.color == QColor(255, 0, 0)); // a cut (its group's)
                e.advanceFades(0.5); // 6 steps (erase "lo", type "p me") at an even pace: 3 done
                CHECK(X()->text.shown() == "Help" && X()->text.size > 40 && X()->text.size < 80);
                e.advanceFades(0.6);
                CHECK(X()->text.shown() == "Help me" && X()->text.size < 80 && e.isFading());
                e.advanceFades(1.0);
                CHECK(X()->text.size == 80 && !e.isFading());
                // Typed by hand: no typewriter
                e.setLayerTextContent(xi, "Hi");
                CHECK(X()->text.shown() == "Hi");
                // A cut types nothing
                src["text/content"] = "Cut";
                st["params"] = src;
                st["timing"] = QJsonObject{{"text/content", 0}};
                e.applyLayers(QJsonArray{st}, 1.0);
                CHECK(X()->text.shown() == "Cut");
                e.advanceFades(3.0);
                // Kept in range when read from a file
                src["text/size"] = 100000;
                src["text/line_height"] = -3;
                st["params"] = src;
                st.remove("timing");
                e.applyLayers(QJsonArray{st}, 0.0);
                CHECK(X()->text.size == 1000 && X()->text.lineHeight > 0);
                e.removeLayer(e.indexOfId(xid));
            }
            // The composition in a snapshot: its opacity and the sound's volume fade on their times
            {
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
                QJsonObject c = e.captureComposition();
                CHECK(c.value("included").toBool() && c.value("opacity").toDouble() == 1.0 && c.contains("volume") && !c.contains("muted"));
                c["opacity"] = 0.0;
                c["volume"] = 0.5;
                c["timing"] = QJsonObject{{"volume", 0}};
                e.applyComposition(c, 1.0);
                CHECK(std::abs(e.audioVolume() - 0.5f) < 1e-6 && e.compositionOpacityTarget() > 0.99);
                e.advanceFades(0.5);
                CHECK(e.compositionOpacityTarget() > 0.05 && e.compositionOpacityTarget() < 0.95);
                e.advanceFades(0.6);
                CHECK(e.compositionOpacityTarget() == 0.0);
                // Left out: nothing moves
                c["included"] = false;
                c["opacity"] = 1.0;
                e.applyComposition(c, 0.0);
                CHECK(e.compositionOpacityTarget() == 0.0);
                // Saved with the project
                Engine::Snapshot mc;
                mc.name = "Comp";
                mc.composition = c;
                const int mi = e.addSnapshot(mc);
                CHECK(e.saveProject(tmp + "/comp.pinol", {}, &err));
                CHECK(readJson(tmp + "/comp.pinol").value("snapshots").toArray().at(mi).toObject().value("composition") == c);
                // Every key of a project is lowercase snake_case (parameter names of shaders and the times of a snapshot aside)
                {
                    int bad = 0;
                    std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &v) {
                        if (v.isArray()) {
                            for (const QJsonValue &x : v.toArray()) walk(x);
                        } else if (v.isObject()) {
                            const QJsonObject o = v.toObject();
                            for (auto it = o.begin(); it != o.end(); ++it) {
                                static const QRegularExpression ok("^[a-z0-9_]+$");
                                if (!ok.match(it.key()).hasMatch()) { ++bad; qWarning("project key: %s", qPrintable(it.key())); }
                                if (it.key() != "params" && it.key() != "timing" && it.key() != "easing") walk(it.value());
                            }
                        }
                    };
                    walk(readJson(tmp + "/comp.pinol"));
                    CHECK(bad == 0);
                }
                e.removeSnapshot(mi);
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
            }
            // Rotation and per-viewport opacity: stored, recalled, and faded
            {
                const quint64 vp = 4242;
                L()->enabled = true;
                L()->mapping.rotation = 40;
                L()->viewportOpacity[vp] = 0.2f;
                QJsonObject s2 = e.layerJson(e.indexOfId(tid));
                s2.remove("timing");
                L()->mapping.rotation = 0;
                L()->viewportOpacity.clear();
                e.applyLayers(QJsonArray{s2}, 0.0); // a cut
                CHECK(std::abs(L()->mapping.rotation - 40) < 1e-6);
                CHECK(L()->viewportOpacity.count(vp) && std::abs(L()->viewportOpacity.at(vp) - 0.2f) < 1e-6);
                L()->mapping.rotation = 0;
                L()->viewportOpacity.clear();
                e.applyLayers(QJsonArray{s2}, 1.0); // a fade
                e.advanceFades(0.5);
                CHECK(L()->mapping.rotation > 1 && L()->mapping.rotation < 39);
                CHECK(L()->viewportOpacity.count(vp) && L()->viewportOpacity.at(vp) > 0.21f && L()->viewportOpacity.at(vp) < 0.99f);
                e.advanceFades(0.6);
                CHECK(std::abs(L()->mapping.rotation - 40) < 1e-6 && std::abs(L()->viewportOpacity.at(vp) - 0.2f) < 1e-6);
                L()->mapping.rotation = 0;
            }
            e.removeLayer(e.indexOfId(tid));
        }
        // ISF speed (generator and effect) fades between snapshots instead of jumping
        {
            const int gi = e.addLayer("Pace", e.layerCount());
            const quint64 gid = e.layerId(gi);
            auto G = [&] { return e.layer(e.indexOfId(gid)); };
            CHECK(e.setLayerIsf(gi, QStringLiteral(TEST_DIR) + "/../isf/generators/TestPattern.fs", &err));
            G()->enabled = true;
            G()->generator->speed = 3.0;
            QJsonObject st = e.layerJson(e.indexOfId(gid));
            st.remove("timing");
            G()->generator->speed = 1.0;
            e.applyLayers(QJsonArray{st}, 1.0);
            CHECK(std::abs(G()->generator->speed - 1.0) < 1e-9); // starts from where it is
            e.advanceFades(0.5);
            CHECK(G()->generator->speed > 1.05 && G()->generator->speed < 2.95);
            e.advanceFades(0.6);
            CHECK(std::abs(G()->generator->speed - 3.0) < 1e-9);
            e.removeLayer(e.indexOfId(gid));
        }
        // Another source by a snapshot: the outgoing one stays, invisible, and a transition mixes the two
        {
            for (const auto &[name, rgb] : {std::pair<QString, QRgb>{"red", qRgb(255, 0, 0)}, {"blue", qRgb(0, 0, 255)}}) {
                QImage im(32, 16, QImage::Format_RGB32);
                im.fill(rgb);
                im.save(tmp + "/" + name + ".png");
            }
            const int ti = e.addLayer("Trans", V);
            const quint64 tid = e.layerId(ti);
            CHECK(e.setLayerImage(ti, tmp + "/red.png", &err));
            e.layer(ti)->mapping.resetCorners();
            e.layer(ti)->roi = QRectF(0, 0, 0.5, 1); // the snapshot's ROI: the incoming source gets there over the time
            QJsonObject red = e.layerJson(ti);
            e.layer(ti)->roi = QRectF(0, 0, 1, 1);
            red["included"] = true;
            CHECK(e.setLayerImage(ti, tmp + "/blue.png", &err));
            const quint64 vp = e.mainViewportId();
            auto center = [&] {
                e.renderFrame();
                const QImage g = e.grabViewport(vp);
                return g.pixelColor(g.width() / 2, g.height() / 2);
            };
            CHECK(center().blue() > 250);
            e.setFadesManual(true); // the frames below do not move the transition: only advanceFades
            e.applyLayers(QJsonArray{red}, 1.0);
            CHECK(e.isTransitioning(tid) && e.layerJson(e.indexOfId(tid)).value("source").toObject().value("path").toString().endsWith("red.png"));
            e.advanceFades(0.5);
            CHECK(std::abs(e.layer(e.indexOfId(tid))->transitionGain - 0.5f) < 0.01f); // the sound crosses too
            CHECK(e.layer(e.indexOfId(tid))->roi.width() > 0.6 && e.layer(e.indexOfId(tid))->roi.width() < 0.9); // no cut
            for (int k = 0; k < 3; ++k) e.renderFrame(); // the incoming picture uploaded (the fade does not move)
            QColor c = center();
            std::printf("      halfway: r %d b %d\n", c.red(), c.blue());
            CHECK(c.red() > 90 && c.red() < 170 && c.blue() > 90 && c.blue() < 170); // halfway: both
            e.advanceFades(0.6);
            c = center();
            CHECK(!e.isTransitioning(tid) && c.red() > 250 && c.blue() < 5 && e.layer(e.indexOfId(tid))->transitionGain == 1.0f);
            CHECK(std::abs(e.layer(e.indexOfId(tid))->roi.width() - 0.5) < 1e-6);
            // An ISF transition chosen for the layer: fade out, fade in — halfway, only what is beneath shows
            e.layer(e.indexOfId(tid))->enabled = false;
            const QColor under = center();
            e.layer(e.indexOfId(tid))->enabled = true;
            QJsonObject blue = e.layerJson(e.indexOfId(tid));
            blue["source"] = [&] {
                QJsonObject src = blue.value("source").toObject();
                src["path"] = tmp + "/blue.png";
                src["transition"] = root + "/../isf/transitions/FadeOutIn.fs";
                return src;
            }();
            e.applyLayers(QJsonArray{blue}, 1.0);
            e.advanceFades(0.5);
            c = center();
            CHECK(e.isTransitioning(tid) && std::abs(c.red() - under.red()) < 30 && std::abs(c.blue() - under.blue()) < 30);
            e.advanceFades(0.6);
            CHECK(center().blue() > 250);
            // The source's own time: a cut, no transition
            red["timing"] = QJsonObject{{"file", 0}};
            e.applyLayers(QJsonArray{red}, 1.0);
            CHECK(!e.isTransitioning(tid) && center().red() > 250);
            // A transition running when the layer goes: it goes with it
            blue["timing"] = QJsonObject{{"file", 2.0}};
            e.applyLayers(QJsonArray{blue}, 0.0); // its own time even with no snapshot fade
            CHECK(e.isTransitioning(tid));
            e.removeLayer(e.indexOfId(tid));
            CHECK(!e.isTransitioning(tid));
            e.setFadesManual(false);
        }
        // Saved with the project
        CHECK(e.saveProject(tmp + "/snapshots.pinol", {}, &err));
        CHECK(e.loadProject(tmp + "/snapshots.pinol", nullptr, &err));
        CHECK(e.snapshotCount() == 2 && e.snapshot(1).name == "Look 2" && std::abs(e.snapshot(1).fade - 0.4) < 1e-9);
        CHECK(e.snapshot(0).thumbnail.width() == 16 && e.snapshot(0).layers.size() == 2);
        e.recallSnapshot(0);
        CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6);
        // Snapshots have ids of their own, kept by edits and by the project
        CHECK(e.snapshot(0).id && e.snapshot(1).id && e.snapshot(0).id != e.snapshot(1).id);
        {
            const quint64 id0 = e.snapshot(0).id;
            Engine::Snapshot m = e.snapshot(0);
            m.id = 999;
            e.setSnapshot(0, m);
            CHECK(e.snapshot(0).id == id0 && e.indexOfSnapshot(id0) == 0 && e.indexOfSnapshot(12345) == -1);
            e.addSnapshot(e.snapshot(1)); // a copy: another id
            CHECK(e.snapshot(2).id != e.snapshot(1).id);
            e.removeSnapshot(2);
        }
        // Sequences: steps recalling snapshots, GO / GO BACK, loop, saved with the project
        {
            Sequencer::Sequence seq;
            seq.name = "Show";
            seq.steps = {{e.snapshot(1).id, "Act 1"}, {e.snapshot(0).id, "Act 2"}, {987654, "gone"}};
            CHECK(e.sequencer().add(seq) == 0 && e.sequencer().current() == 0 && e.sequencer().position() == -1);
            CHECK(e.sequencer().next() == 0 && e.sequencer().previous() == -1);
            e.layer(V + 0)->opacity = 0.1f;
            CHECK(e.sequencer().go() && e.sequencer().position() == 0); // snapshot 1 ("Look 2"): layer 0 at 0, with its fade
            CHECK(e.sequencer().go() && e.sequencer().position() == 1 && std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6); // snapshot 0, at once
            CHECK(e.sequencer().go() && e.sequencer().position() == 2); // its snapshot is gone: the position moves, nothing else
            CHECK(e.sequencer().next() == -1 && !e.sequencer().go()); // the end, no loop
            seq.loop = true;
            e.sequencer().setSequence(0, seq);
            CHECK(e.sequencer().next() == 0);
            CHECK(e.sequencer().back() && e.sequencer().position() == 1);
            e.sequencer().setPosition(0);
            CHECK(e.sequencer().previous() == 2); // loop: before the first, the last
            Sequencer::Sequence copy = e.sequencer().sequence(0);
            copy.name = "Copy";
            CHECK(e.sequencer().add(copy) == 1 && e.sequencer().current() == 0);
            e.sequencer().setCurrent(1);
            CHECK(e.sequencer().position() == -1);
            CHECK(e.sequencer().startupSequence() == -1); // opt-in per project
            e.sequencer().setStartupSequence(1);
            CHECK(e.saveProject(tmp + "/sequences.pinol", {}, &err));
            CHECK(readJson(tmp + "/sequences.pinol").value("startup_sequence").toInt(-1) == 1);
            CHECK(e.loadProject(tmp + "/sequences.pinol", nullptr, &err));
            CHECK(e.sequencer().count() == 2 && e.sequencer().current() == 1 && e.sequencer().sequence(0).loop &&
                  e.sequencer().sequence(0).steps.size() == 3 && e.sequencer().sequence(0).steps[0].text == "Act 1" &&
                  e.sequencer().sequence(0).steps[0].snapshot == e.snapshot(1).id);
            CHECK(e.sequencer().startupSequence() == 1);
            CHECK(e.sequencer().add(copy, 0) == 0 && e.sequencer().startupSequence() == 2);
            e.sequencer().remove(0);
            CHECK(e.sequencer().startupSequence() == 1);
            e.sequencer().remove(1);
            CHECK(e.sequencer().startupSequence() == -1); // deleting selected sequence disables autoplay
            e.sequencer().remove(0);
            CHECK(e.sequencer().count() == 0 && e.sequencer().current() == -1 && !e.sequencer().go());
        }
        // Independent sequences: editing/selecting B must not cancel A's pending
        // pre-wait or auto-follow. Each retains a separate playhead and Stop
        // only affects the currently selected sequence.
        {
            e.setFadesManual(true);
            std::vector<int> fired;
            e.sequencer().setRecaller([&](int mi) { fired.push_back(mi); });
            Sequencer::Sequence a, b, inserted;
            a.name = "Background A";
            Sequencer::Step a0, a1;
            a0.snapshot = e.snapshot(0).id;
            a0.preWait = 1.0;
            a0.postWait = 0.5;
            a0.next = Sequencer::StepContinue::Follow;
            a1.snapshot = e.snapshot(1).id;
            a.steps = {a0, a1};
            b.name = "Editing B";
            Sequencer::Step b0;
            b0.snapshot = e.snapshot(1).id;
            b0.preWait = 0.25;
            b.steps = {b0};
            const int ia = e.sequencer().add(a);
            e.sequencer().setCurrent(ia);
            CHECK(e.sequencer().go() && e.sequencer().running());
            // The "new sequence" UI does add + setCurrent; no callback is cancelled.
            const int ib = e.sequencer().add(b);
            e.sequencer().setCurrent(ib);
            CHECK(e.sequencer().position() == -1 && !e.sequencer().running() &&
                  e.sequencer().runs().empty() && fired.empty());
            CHECK(e.sequencer().active(ia) && !e.sequencer().active(ib));
            CHECK(e.sequencer().go() && e.sequencer().position() == 0);
            e.sequencer().advance(0.25);
            CHECK(fired == (std::vector<int>{1})); // B fired, A still pending
            e.sequencer().advance(0.75);
            CHECK(fired == (std::vector<int>{1, 0})); // A recalled while B selected
            e.sequencer().advance(0.5);
            CHECK(fired == (std::vector<int>{1, 0, 1})); // A followed autonomously
            CHECK(e.sequencer().position() == 0); // B's playhead has not moved
            e.sequencer().setCurrent(ia);
            CHECK(e.sequencer().position() == 1 && e.sequencer().next() == -1);
            // Insertion before the running owner must not redirect its pending GO.
            CHECK(e.sequencer().goTo(0));
            inserted.name = "New first";
            e.sequencer().add(inserted, 0);
            e.sequencer().setCurrent(0);
            e.sequencer().advance(1.0);
            CHECK(fired == (std::vector<int>{1, 0, 1, 0}));
            // Stop B only: A's next pre-wait is untouched.
            e.sequencer().setCurrent(1); // A shifted to index 1
            e.sequencer().stop(); // clear the prior test's unfinished follow first
            CHECK(e.sequencer().goTo(0));
            e.sequencer().setCurrent(2); // B shifted to index 2
            CHECK(e.sequencer().goTo(0));
            e.sequencer().stop();
            e.sequencer().advance(1.0);
            CHECK(fired.back() == 0); // A still fired; B's cancelled wait did not
            // Removing the selected unrelated sequence leaves the remaining
            // sequence's pending pre-wait alive, even if the indices shift.
            e.sequencer().setCurrent(1);
            e.sequencer().stop();
            CHECK(e.sequencer().goTo(0));
            const size_t beforeRemove = fired.size();
            e.sequencer().remove(0);
            e.sequencer().advance(1.0);
            CHECK(fired.size() == beforeRemove + 1 && fired.back() == 0);
            // AutoFollow also belongs to its originating sequence, not the UI
            // selection. Changing the selection during its action must preserve
            // the delayed second GO and update that sequence's own playhead.
            e.sequencer().setCurrent(0);
            e.sequencer().stop();
            Sequencer::Sequence autoSequence;
            autoSequence.name = "Auto background";
            Sequencer::Step autoFirst, autoSecond;
            autoFirst.snapshot = e.snapshot(0).id;
            autoFirst.preWait = 0.1;
            autoFirst.postWait = 0.1;
            autoFirst.next = Sequencer::StepContinue::AutoFollow;
            autoSecond.snapshot = e.snapshot(1).id;
            autoSequence.steps = {autoFirst, autoSecond};
            const int autoIndex = e.sequencer().add(autoSequence);
            e.sequencer().setCurrent(autoIndex);
            const size_t beforeAuto = fired.size();
            CHECK(e.sequencer().go());
            e.sequencer().setCurrent(1); // edit another sequence meanwhile
            e.sequencer().advance(0.1 + e.sequencer().stepDuration(autoFirst) + 0.1 + 0.05);
            CHECK(fired.size() == beforeAuto + 2 && fired[beforeAuto] == 0 && fired[beforeAuto + 1] == 1);
            e.sequencer().setCurrent(autoIndex);
            CHECK(e.sequencer().position() == 1);
            e.sequencer().stop();
            // A given sequence, whatever is selected (OSC /sequence/<n>/go, step, stop), and Stop all: the operator's
            // panic button must reach the sequences playing in the background too
            {
                Sequencer::Sequence p, q;
                Sequencer::Step w;
                w.snapshot = e.snapshot(0).id;
                w.preWait = 0.5;
                p.name = "Panel P";
                p.steps = {w, w};
                q.name = "Panel Q";
                q.steps = {w};
                const int ip = e.sequencer().add(p), iq = e.sequencer().add(q);
                e.sequencer().setCurrent(iq);
                CHECK(e.sequencer().goSequence(ip) && e.sequencer().positionOf(ip) == 0);
                CHECK(e.sequencer().positionOf(iq) == -1 && e.sequencer().position() == -1); // the selected one untouched
                CHECK(e.sequencer().active(ip) && !e.sequencer().running() && e.sequencer().anyActive());
                CHECK(e.sequencer().goToStepOf(ip, 1) && e.sequencer().positionOf(ip) == 1);
                CHECK(e.sequencer().go()); // the selected one: Q
                const size_t before = fired.size();
                e.sequencer().stopSequence(ip); // only P
                CHECK(!e.sequencer().active(ip) && e.sequencer().active(iq));
                CHECK(e.sequencer().goSequence(ip) || e.sequencer().positionOf(ip) == 1); // P at its end: nothing next
                CHECK(e.sequencer().goToStepOf(ip, 0));
                e.sequencer().stopAll();
                CHECK(!e.sequencer().anyActive() && !e.sequencer().active(ip) && !e.sequencer().active(iq));
                e.sequencer().advance(1.0);
                CHECK(fired.size() == before); // nothing fired after Stop all, in either sequence
                CHECK(!e.sequencer().goSequence(-1) && !e.sequencer().goSequence(e.sequencer().count()));
            }
            e.sequencer().setRecaller(nullptr);
            e.sequencer().clear();
            e.setFadesManual(false);
        }
        // Snapshots run side by side: a recall takes over only the values it holds, from where they are
        {
            e.setFadesManual(true);
            const int ai = e.addLayer("Side A", V);
            const quint64 ia = e.layerId(ai);
            const int bi = e.addLayer("Side B", V);
            const quint64 ib = e.layerId(bi);
            auto op = [&](quint64 id) { return e.layer(e.indexOfId(id))->opacity; };
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            {
                Engine::Lock lk(&e.mutex());
                for (quint64 id : {ia, ib}) {
                    e.layer(e.indexOfId(id))->opacity = 0;
                    e.layer(e.indexOfId(id))->enabled = true;
                }
            }
            // A snapshot holding one layer (the others are known, left alone)
            auto memWith = [&](quint64 id, double opacity, double fade) {
                Engine::Snapshot m;
                m.name = "Side";
                m.fade = fade;
                for (const QJsonValue &v : e.captureLayers()) {
                    QJsonObject o = v.toObject();
                    const bool me = o.value("id").toString().toULongLong() == id;
                    o["included"] = me;
                    if (me) setParam(o, "opacity", opacity);
                    m.layers.append(o);
                }
                return e.addSnapshot(m);
            };
            const int mA1 = memWith(ia, 1.0, 2.0), mB1 = memWith(ib, 1.0, 2.0), mA0 = memWith(ia, 0.0, 2.0);
            e.recallSnapshot(mA1);
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.5)); // eased: half way at half the time
            e.recallSnapshot(mB1);       // another layer: A goes on
            e.advanceFades(1.0);
            CHECK(near(op(ia), 1.0) && near(op(ib), 0.5));
            e.recallSnapshot(mA0); // A again: this one takes it over
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.5) && near(op(ib), 1.0));
            e.recallSnapshot(mA1); // in the middle of a fade: from where it is
            e.advanceFades(1.0);
            CHECK(near(op(ia), 0.75));
            e.advanceFades(5.0);
            CHECK(near(op(ia), 1.0) && !e.isFading());

            // The composition: the level and the volume each on their own clock
            {
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
                Engine::Snapshot lv, vol;
                lv.fade = vol.fade = 2.0;
                lv.composition = QJsonObject{{"included", true}, {"opacity", 0.0}};
                vol.composition = QJsonObject{{"included", true}, {"volume", 0.0}};
                const int ml = e.addSnapshot(lv), mv = e.addSnapshot(vol);
                e.recallSnapshot(ml);
                e.advanceFades(1.0);
                CHECK(near(e.compositionOpacityTarget(), 0.5));
                e.recallSnapshot(mv); // the volume only: the level goes on
                e.advanceFades(1.0);
                CHECK(near(e.compositionOpacityTarget(), 0.0) && near(e.audioVolume(), 0.5));
                e.advanceFades(2.0);
                e.removeSnapshot(mv);
                e.removeSnapshot(ml);
                e.fadeCompositionOpacity(1.0, 0);
                e.setAudioVolume(1.0f);
            }

            // A step's action is its snapshot's longest time
            CHECK(near(e.snapshotDuration(e.snapshot(mA1).id), 2.0) && e.snapshotDuration(987654) == 0);
            {
                Engine::Snapshot m = e.snapshot(mB1);
                QJsonArray ls = m.layers;
                for (int k = 0; k < ls.size(); ++k) {
                    QJsonObject o = ls[k].toObject();
                    if (o.value("included").toBool()) {
                        o["timing"] = QJsonObject{{"opacity", 5.0}};
                        o["easing"] = QJsonObject{{"opacity", "linear"}};
                    }
                    ls[k] = o;
                }
                m.layers = ls;
                e.setSnapshot(mB1, m);
                CHECK(near(e.snapshotDuration(m.id), 5.0));
                m.layers = e.snapshot(mA1).layers; // back to the snapshot's fade (2 s)
                for (int k = 0; k < m.layers.size(); ++k) {
                    QJsonObject o = m.layers[k].toObject();
                    const bool me = o.value("id").toString().toULongLong() == ib;
                    o["included"] = me;
                    setParam(o, "opacity", 1.0);
                    m.layers[k] = o;
                }
                e.setSnapshot(mB1, m);
            }

            // Pre-wait, post-wait, follow / auto-follow / wait
            std::vector<int> fired;
            e.sequencer().setRecaller([&](int mi) {
                fired.push_back(mi);
                e.recallSnapshot(mi);
            });
            Sequencer::Sequence seq;
            seq.name = "Waits";
            Sequencer::Step s0, s1, s2;
            s0.snapshot = e.snapshot(mA1).id;
            s0.preWait = 1.0;
            s0.postWait = 0.5;
            s0.next = Sequencer::StepContinue::Follow; // the next GO 0.5 s after this one is triggered
            s1.snapshot = e.snapshot(mB1).id;
            s1.postWait = 0.5;
            s1.next = Sequencer::StepContinue::AutoFollow; // the next GO 0.5 s after its 2 s action
            s2.snapshot = e.snapshot(mA0).id;               // Wait
            seq.steps = {s0, s1, s2};
            e.sequencer().setCurrent(e.sequencer().add(seq));
            CHECK(e.sequencer().go() && e.sequencer().position() == 0 && fired.empty()); // in its pre-wait
            CHECK(e.sequencer().running() && e.sequencer().runs().size() == 1 && !e.sequencer().runs()[0].fired);
            e.sequencer().advance(0.5);
            CHECK(fired.empty());
            e.sequencer().advance(0.5);
            CHECK(fired == std::vector<int>{mA1} && e.sequencer().position() == 0);
            e.sequencer().advance(0.4);
            CHECK(e.sequencer().position() == 0);
            e.sequencer().advance(0.1); // 1.5 s: pre-wait + post-wait, the next one without waiting for the action
            CHECK(fired == (std::vector<int>{mA1, mB1}) && e.sequencer().position() == 1);
            e.sequencer().advance(2.4);
            CHECK(e.sequencer().position() == 1);
            e.sequencer().advance(0.2); // its action (2 s) and its post-wait (0.5 s) over
            CHECK(fired == (std::vector<int>{mA1, mB1, mA0}) && e.sequencer().position() == 2);
            e.sequencer().advance(10.0);
            CHECK(fired.size() == 3 && !e.sequencer().running() && e.sequencer().runs().empty()); // Wait: GO is awaited
            // All at once: the time beyond each GO is carried over to the next
            fired.clear();
            e.sequencer().setPosition(-1);
            e.sequencer().go();
            e.sequencer().advance(10.0);
            CHECK(fired == (std::vector<int>{mA1, mB1, mA0}) && e.sequencer().position() == 2);
            // Stopped: what was still to come does not come
            fired.clear();
            e.sequencer().setPosition(-1);
            e.sequencer().go();
            e.sequencer().stop();
            e.sequencer().advance(10.0);
            CHECK(fired.empty() && e.sequencer().position() == 0);
            // GO during a pre-wait: the playhead has moved on, the next GO is the next step
            fired.clear();
            e.sequencer().setPosition(-1);
            e.sequencer().go();
            CHECK(e.sequencer().next() == 1);
            // GO BACK: at once, and the waits running are stopped
            e.sequencer().setPosition(2);
            CHECK(e.sequencer().back() && e.sequencer().position() == 1 && fired == std::vector<int>{mB1});
            e.sequencer().advance(10.0);
            CHECK(fired == std::vector<int>{mB1}); // neither step 0's pre-wait nor step 1's auto-follow
            // Saved with the project
            CHECK(e.saveProject(tmp + "/waits.pinol", {}, &err));
            CHECK(e.loadProject(tmp + "/waits.pinol", nullptr, &err));
            {
                const Sequencer::Sequence w = e.sequencer().sequence(e.sequencer().current());
                CHECK(w.steps.size() == 3 && near(w.steps[0].preWait, 1.0) && near(w.steps[0].postWait, 0.5) &&
                      w.steps[0].next == Sequencer::StepContinue::Follow &&
                      w.steps[1].next == Sequencer::StepContinue::AutoFollow && w.steps[2].next == Sequencer::StepContinue::Wait);
            }
            e.sequencer().setRecaller(nullptr);
            e.sequencer().remove(e.sequencer().current());
            for (int mi : {mA0, mB1, mA1}) e.removeSnapshot(mi); // the last first
            e.removeLayer(e.indexOfId(ia));
            e.removeLayer(e.indexOfId(ib));
            e.setFadesManual(false);
        }
        // The cue clock (audit P1): a pre-wait ends on time while the thread that played GO is blocked (the
        // interface busy, no event processed); the recall is made from the clock's thread, the interface told after
        {
            e.setFadesManual(false);
            const quint64 cid = e.layerId(e.addLayer("Cue", V));
            {
                Engine::Lock lk(&e.mutex());
                e.layer(e.indexOfId(cid))->opacity = 0;
            }
            Engine::Snapshot m;
            m.name = "Cue";
            m.fade = 0;
            for (const QJsonValue &v : e.captureLayers()) {
                QJsonObject o = v.toObject();
                const bool me = o.value("id").toString().toULongLong() == cid;
                o["included"] = me;
                if (me) setParam(o, "opacity", 1.0);
                m.layers.append(o);
            }
            const int mi = e.addSnapshot(m);
            std::atomic<qint64> firedAt{-1};
            std::atomic<QThread *> firedOn{nullptr};
            QElapsedTimer since;
            const QMetaObject::Connection watch = QObject::connect(&e, &Engine::snapshotRecalled, &e, [&](int i) {
                if (i != mi || firedAt.load() >= 0) return;
                firedAt = since.elapsed();
                firedOn = QThread::currentThread();
            }, Qt::DirectConnection);
            std::vector<int> recorded;
            bool beforeKept = false;
            e.sequencer().setRecallRecorder([&](int i, const Sequencer::RecallBefore &before) {
                recorded.push_back(i);
                beforeKept = !before.layers.isEmpty();
            });
            Sequencer::Sequence seq;
            seq.name = "Clock";
            Sequencer::Step st;
            st.snapshot = e.snapshot(mi).id;
            st.preWait = 0.1;
            seq.steps = {st};
            e.sequencer().setCurrent(e.sequencer().add(seq));
            e.sequencer().resetCueTiming();
            since.start();
            CHECK(e.sequencer().go() && firedAt.load() < 0); // in its pre-wait
            QThread::msleep(400); // busy: nothing processed on this thread
            std::printf("cue clock: recalled %lld ms after GO (pre-wait 100 ms)\n", (long long)firedAt.load());
            CHECK(firedAt.load() >= 95 && firedAt.load() <= 300); // on time, while this thread was blocked
            CHECK(firedOn.load() && firedOn.load() != QThread::currentThread());
            CHECK(recorded.empty()); // the interface is told afterwards, on its own thread
            for (int k = 0; k < 200 && recorded.empty(); ++k) {
                QCoreApplication::processEvents();
                QThread::msleep(5);
            }
            CHECK(recorded == std::vector<int>{mi} && beforeKept);
            {
                const Sequencer::CueTiming t = e.sequencer().cueTiming();
                std::printf("cue clock: %llu fired, late %.2f ms (max %.2f, p99 <= %.2f)\n", (unsigned long long)t.fired,
                            t.last * 1e3, t.max * 1e3, t.percentile(0.99) * 1e3);
                CHECK(t.fired == 1 && t.max < 0.2 && t.percentile(0.99) <= t.max + 1e-12);
                const Sequencer::CueWorkTiming w = e.sequencer().cueWorkTiming();
                const QJsonObject measured = e.frameTimingJson();
                std::printf("cue work: Undo capture %.3f ms, apply %.3f ms, total %.3f ms\n",
                            w.capture.max, w.recall.max, w.total.max);
                CHECK(w.capture.count == 1 && w.recall.count == 1 && w.total.count == 1);
                CHECK(w.total.max >= w.capture.max && w.total.max >= w.recall.max);
                CHECK(w.total.percentile(0.99) <= w.total.max + 1e-9);
                CHECK(measured.value("cue_capture").toObject().value("count").toInt() == 1 &&
                      measured.value("cue_recall").toObject().value("count").toInt() == 1 &&
                      measured.value("cue_execution").toObject().value("count").toInt() == 1);
                // Reset of /composition/timing must clear *both* clock lateness and expensive cue work.
                e.resetFrameTiming();
                CHECK(e.sequencer().cueTiming().fired == 0 &&
                      e.sequencer().cueWorkTiming().capture.count == 0 &&
                      e.sequencer().cueWorkTiming().total.count == 0);
            }
            // The undo of a recall made elsewhere: not recalled a second time when pushed, its state back on undo
            {
                e.renderFrame();
                CHECK(std::abs(e.layer(e.indexOfId(cid))->opacity - 1.0f) < 1e-6);
                QUndoStack undo;
                const QJsonArray before = [&] {
                    QJsonArray layers = e.captureLayers(true);
                    for (int k = 0; k < layers.size(); ++k) {
                        QJsonObject o = layers[k].toObject();
                        if (o.value("id").toString().toULongLong() == cid) setParam(o, "opacity", 0.25);
                        layers[k] = o;
                    }
                    return layers;
                }();
                firedAt = -1;
                undo.push(new cmd::RecallSnapshot(&e, mi, before, e.captureComposition(), e.captureInteractions()));
                CHECK(firedAt.load() < 0); // pushed: no second recall
                undo.undo();
                e.renderFrame();
                CHECK(std::abs(e.layer(e.indexOfId(cid))->opacity - 0.25f) < 1e-6);
            }
            for (int k = 0; k < 200 && e.sequencer().clockRunning(); ++k) QThread::msleep(5);
            CHECK(!e.sequencer().clockRunning()); // nothing on its way: the clock stops ticking
            QObject::disconnect(watch);
            e.sequencer().setRecallRecorder(nullptr);
            e.sequencer().remove(e.sequencer().current());
            e.removeSnapshot(mi);
            e.removeLayer(e.indexOfId(cid));
        }
        // Timelines: curves and oscillators on their own clock; the sequences drive their transport
        {
            using A = Engine::AnimAction;
            using L = Engine::AnimLoop;
            e.setFadesManual(true);
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            const quint64 tid = e.layerId(e.addLayer("Anim", V));
            auto val = [&](const QString &p) {
                double v = std::nan("");
                e.animParamValue(tid, p, &v);
                return v;
            };
            // A curve: eased from key to key (linear here), held, then its last value
            Engine::AnimTrack c;
            c.layer = tid;
            c.param = "opacity";
            c.keys = {{0, 0, 0}, {2, 1, 0}, {3, 0.2, Engine::kAnimHold}, {4, 0.8, 0}};
            CHECK(near(c.valueAt(1, 0), 0.5) && near(c.valueAt(3.5, 0), 0.2) && near(c.valueAt(5, 0), 0.8) &&
                  near(c.valueAt(-1, 0), 0));
            CHECK(std::isnan(Engine::AnimTrack().valueAt(0, 0))); // no key: no value
            // An oscillator: on the time played, center ± amplitude
            Engine::AnimTrack o;
            o.oscillator = true;
            o.period = 2;
            o.center = 10;
            o.amplitude = 5;
            CHECK(near(o.valueAt(0, 0.5), 15) && near(o.valueAt(0, 1.0), 10) && near(o.valueAt(0, 1.5), 5));
            o.wave = Engine::AnimWave::Triangle;
            CHECK(near(o.valueAt(0, 0.25), 12.5) && near(o.valueAt(0, 1.5), 5));
            o.wave = Engine::AnimWave::Saw;
            CHECK(near(o.valueAt(0, 0), 5) && near(o.valueAt(0, 1), 10));
            o.wave = Engine::AnimWave::Square;
            CHECK(near(o.valueAt(0, 0.2), 15) && near(o.valueAt(0, 1.2), 5));
            // Passes: loop, ping-pong, a number of them
            Engine::Animation a;
            a.name = "Pulse";
            a.duration = 4;
            a.loop = L::PingPong;
            CHECK(near(a.position(5), 3) && !std::isfinite(a.length()));
            a.loop = L::Loop;
            a.repeat = 2;
            CHECK(near(a.position(5), 1) && near(a.length(), 8) && near(a.position(9), 4));
            a.loop = L::Once;
            CHECK(near(a.position(9), 4) && near(a.length(), 4));
            // Playing: the curve repeats its pattern, the oscillator turns the mapping without a jump
            a.loop = L::Loop;
            a.repeat = 0;
            Engine::AnimTrack rot;
            rot.layer = tid;
            rot.param = "rotation";
            rot.oscillator = true;
            rot.wave = Engine::AnimWave::Saw;
            rot.period = 4;
            rot.center = 0;
            rot.amplitude = 180;
            a.tracks = {c, rot};
            const int ai = e.addAnimation(a);
            const quint64 aid = e.animation(ai).id;
            CHECK(aid != 0 && e.indexOfAnimation(aid) == ai && e.animationCount() >= 1);
            const QRectF before = e.layer(e.indexOfId(tid))->mapping.bounds();
            e.controlAnimation(aid, A::Play);
            CHECK(near(val("opacity"), 0) && e.animation(ai).state == Engine::AnimState::Playing);
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 0.5) && near(val("rotation"), -90));
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 1.0) && near(val("rotation"), 0));
            CHECK(near(e.layer(e.indexOfId(tid))->mapping.bounds().width(), before.width())); // turned, not shrunk
            e.advanceFades(2.5); // 4.5 s: the second pass of the pattern
            CHECK(near(val("opacity"), 0.25) && near(val("rotation"), -135));
            // Paused: its values are left alone
            e.controlAnimation(aid, A::Pause);
            e.layer(e.indexOfId(tid))->opacity = 0.9f;
            e.advanceFades(1.0);
            CHECK(near(val("opacity"), 0.9) && e.animation(ai).state == Engine::AnimState::Paused);
            // Seek: the values at once (the hold), then Play goes on from there
            e.controlAnimation(aid, A::Seek, 3.5);
            CHECK(near(val("opacity"), 0.2) && near(e.animation(ai).clock, 3.5));
            e.controlAnimation(aid, A::Play);
            e.advanceFades(0.25);
            CHECK(near(val("opacity"), 0.2) && near(e.animation(ai).clock, 3.75));
            // Stop: back to the start, the values stay
            e.controlAnimation(aid, A::Stop);
            e.advanceFades(1.0);
            CHECK(e.animation(ai).state == Engine::AnimState::Stopped && e.animation(ai).clock == 0 && near(val("opacity"), 0.2));
            // Twice, then over with its last values
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 2);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(9.0);
            CHECK(e.animation(ai).state == Engine::AnimState::Stopped && near(val("opacity"), 0.8));
            // The composition's level
            {
                Engine::Animation lv;
                Engine::AnimTrack t;
                t.param = "opacity";
                t.keys = {{0, 0.3, 0}};
                lv.tracks = {t};
                const quint64 lid = e.animation(e.addAnimation(lv)).id;
                e.controlAnimation(lid, A::Seek, 0);
                CHECK(near(e.compositionOpacityTarget(), 0.3));
                e.removeAnimation(e.indexOfAnimation(lid));
                e.fadeCompositionOpacity(1.0, 0);
            }
            CHECK(!e.animatableParams(tid).empty() && e.animatableParams(0).size() == 3);
            // Steps driving it: Play (its time, when it ends), Seek, Stop
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            Sequencer::Sequence sq;
            sq.name = "Timeline";
            Sequencer::Step p0, p1, p2;
            p0.timeline = aid;
            p0.next = Sequencer::StepContinue::AutoFollow;
            p1.timeline = aid;
            p1.action = A::Seek;
            p1.seekTime = 1.0;
            p2.timeline = aid;
            p2.action = A::Stop;
            Sequencer::Step p3;
            p3.timeline = aid;
            p3.action = A::Speed;
            p3.speed = 0.75;
            sq.steps = {p0, p1, p2, p3};
            e.sequencer().setCurrent(e.sequencer().add(sq));
            CHECK(near(e.sequencer().stepDuration(p0), 4) && e.sequencer().stepDuration(p1) == 0);
            CHECK(e.sequencer().go() && e.animation(ai).state == Engine::AnimState::Playing);
            e.advanceFades(3.9);
            e.sequencer().advance(3.9);
            CHECK(e.sequencer().position() == 0);
            e.advanceFades(0.2); // (the timeline and the sequence run together)
            e.sequencer().advance(0.2); // its 4 s over: the seek
            CHECK(e.sequencer().position() == 1 && near(e.animation(ai).clock, 1.0) &&
                  e.animation(ai).state == Engine::AnimState::Paused); // over, then sought: stays where it was sought
            CHECK(e.sequencer().go() && e.animation(ai).state == Engine::AnimState::Stopped);
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            CHECK(e.sequencer().stepDuration(p0) == 0); // endless: nothing to wait for
            using S = Engine::AnimState;
            // Loop → Once while it plays past its first pass: it ends the pass it is in, no jump to the end
            e.controlAnimation(aid, A::Play);
            e.advanceFades(9.0); // the third pass, 1 s in
            CHECK(near(e.animation(ai).position(), 1.0));
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 1.0));
            e.advanceFades(2.0);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 3.0));
            e.advanceFades(1.5);
            CHECK(e.animation(ai).state == S::Stopped);
            // Ping-pong on its way back, then Once: back to the start, then over
            e.controlAnimation(aid, A::LoopMode, 0, L::PingPong, 0);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(5.0);
            CHECK(near(e.animation(ai).position(), 3.0));
            e.controlAnimation(aid, A::LoopMode, 0, L::Once);
            e.advanceFades(1.0);
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 2.0));
            e.advanceFades(2.5);
            CHECK(e.animation(ai).state == S::Stopped);
            // The same from the timeline window (an edit of the whole timeline)
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            e.controlAnimation(aid, A::Play);
            e.advanceFades(6.0);
            {
                Engine::Animation x = e.animation(ai);
                x.loop = L::Once;
                e.setAnimation(ai, x);
            }
            CHECK(e.animation(ai).state == S::Playing && near(e.animation(ai).position(), 2.0));
            e.controlAnimation(aid, A::Stop);
            // Speed: twice as fast; a step waits for the real time left
            {
                Engine::Animation x = e.animation(ai);
                x.speed = 2;
                e.setAnimation(ai, x);
            }
            e.controlAnimation(aid, A::Play);
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).clock, 2.0) && near(e.sequencer().stepDuration(p0), 1.0));
            e.controlAnimation(aid, A::Stop);
            // Speed from a step (or OSC): it plays on from where it is
            e.controlAnimation(aid, A::Play);
            e.advanceFades(1.0);
            e.controlAnimation(aid, A::Speed, 0.5);
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).speed, 0.5) && near(e.animation(ai).clock, 2.5));
            e.controlAnimation(aid, A::Speed, 0); // frozen where it is, still playing
            e.advanceFades(1.0);
            CHECK(near(e.animation(ai).clock, 2.5) && e.animation(ai).state == Engine::AnimState::Playing &&
                  !std::isfinite(e.sequencer().stepDuration(p0)));
            e.controlAnimation(aid, A::Stop);
            // An auto-follow Play on a frozen timeline waits; its speed raised meanwhile (OSC), it goes on, and the
            // next step comes once the timeline is really over
            {
                const int keepSeq = e.sequencer().current();
                Engine::Animation x = e.animation(ai);
                x.loop = L::Once;
                x.speed = 0;
                e.setAnimation(ai, x);
                Sequencer::Sequence fq;
                fq.name = "Frozen";
                Sequencer::Step f0, f1;
                f0.timeline = aid;
                f0.next = Sequencer::StepContinue::AutoFollow;
                f1.timeline = aid;
                f1.action = A::Pause;
                fq.steps = {f0, f1};
                const int fi = e.sequencer().add(fq);
                e.sequencer().setCurrent(fi);
                CHECK(!std::isfinite(e.sequencer().stepDuration(f0)));
                CHECK(e.sequencer().go() && e.animation(ai).state == Engine::AnimState::Playing);
                for (int k = 0; k < 3; ++k) {
                    e.advanceFades(10.0);
                    e.sequencer().advance(10.0);
                }
                CHECK(e.sequencer().position() == 0 && near(e.animation(ai).clock, 0)); // still waiting
                e.controlAnimation(aid, A::Speed, 2);
                e.sequencer().advance(0.01); // it sees the speed
                e.advanceFades(1.0);
                e.sequencer().advance(1.0);
                CHECK(e.sequencer().position() == 0 && near(e.animation(ai).clock, 2.0));
                e.controlAnimation(aid, A::Speed, 0); // frozen again: it waits again
                e.advanceFades(5.0);
                e.sequencer().advance(5.0);
                CHECK(e.sequencer().position() == 0 && near(e.animation(ai).clock, 2.0));
                e.controlAnimation(aid, A::Speed, 2);
                e.sequencer().advance(0.01);
                e.advanceFades(1.5);
                e.sequencer().advance(1.5); // its 4 s played: over, and the next step comes
                CHECK(e.sequencer().position() == 1);
                e.sequencer().stop();
                e.sequencer().setCurrent(keepSeq);
                e.sequencer().remove(fi);
                e.controlAnimation(aid, A::Stop);
                x.speed = 2;
                e.setAnimation(ai, x);
            }
            // Undo: the arrows of a number merge into one step
            {
                QUndoStack st;
                const Engine::Animation b = e.animation(ai);
                Engine::Animation a1 = b, a2 = b;
                a1.speed = 1.5;
                a2.speed = 1;
                st.push(new cmd::SetAnimation(&e, aid, b, a1, "Speed", "speed"));
                st.push(new cmd::SetAnimation(&e, aid, a1, a2, "Speed", "speed"));
                CHECK(st.count() == 1 && near(e.animation(ai).speed, 1));
                st.undo();
                CHECK(near(e.animation(ai).speed, 2));
                st.redo();
                auto *add = new cmd::AddAnimation(&e, Engine::Animation(), e.animationCount(), "New");
                st.push(add);
                const quint64 nid = add->animationId();
                CHECK(nid != 0 && e.indexOfAnimation(nid) >= 0);
                st.push(new cmd::RemoveAnimation(&e, e.indexOfAnimation(nid)));
                CHECK(e.indexOfAnimation(nid) < 0);
                st.undo();
                CHECK(e.indexOfAnimation(nid) >= 0);
                st.undo();
                CHECK(e.indexOfAnimation(nid) < 0);
            }
            e.controlAnimation(aid, A::LoopMode, 0, L::Loop, 0);
            // A first key "current value": the number starts from where it is, no jump
            quint64 cid = 0;
            {
                e.layer(e.indexOfId(tid))->opacity = 0.6f;
                Engine::Animation cv;
                cv.name = "From here";
                cv.duration = 2;
                cv.loop = L::Once;
                Engine::AnimTrack t;
                t.layer = tid;
                t.param = "opacity";
                t.keys = {{0, 0, 0}, {2, 1, 0}};
                t.keys[0].isCurrentValue = true;
                cv.tracks = {t};
                cid = e.animation(e.addAnimation(cv)).id;
                e.controlAnimation(cid, A::Play);
                CHECK(near(val("opacity"), 0.6));
                e.advanceFades(1.0);
                CHECK(near(val("opacity"), 0.8));
                e.advanceFades(1.5);
                CHECK(near(val("opacity"), 1.0));
                e.layer(e.indexOfId(tid))->opacity = 0.2f;
                e.controlAnimation(cid, A::Play); // from 0.2 now
                e.advanceFades(1.0);
                CHECK(near(val("opacity"), 0.6));
                e.controlAnimation(cid, A::Stop);
            }
            // Saved with the project
            CHECK(e.saveProject(tmp + "/timelines.pinol", {}, &err));
            CHECK(e.loadProject(tmp + "/timelines.pinol", nullptr, &err));
            {
                const int k = e.indexOfAnimation(aid);
                CHECK(k >= 0);
                const Engine::Animation x = e.animation(k);
                CHECK(x.name == "Pulse" && near(x.duration, 4) && x.loop == L::Loop && x.tracks.size() == 2 &&
                      x.tracks[0].keys.size() == 4 && x.tracks[0].keys[2].curve == Engine::kAnimHold &&
                      x.tracks[0].keys[1].curve == 0 && x.tracks[1].oscillator &&
                      x.tracks[1].wave == Engine::AnimWave::Saw && near(x.tracks[1].amplitude, 180) &&
                      x.tracks[0].layer == tid && x.state == Engine::AnimState::Stopped);
                const int ck = e.indexOfAnimation(cid);
                CHECK(ck >= 0 && e.animation(ck).tracks[0].keys[0].isCurrentValue && !e.animation(ck).tracks[0].keys[1].isCurrentValue &&
                      e.animation(ck).tracks[0].keys[0].curve == 0);
                if (ck >= 0) e.removeAnimation(ck);
                const Sequencer::Sequence w = e.sequencer().sequence(e.sequencer().current());
                CHECK(w.steps.size() == 4 && w.steps[3].action == A::Speed && near(w.steps[3].speed, 0.75) &&
                      w.steps[0].timeline == aid && w.steps[0].snapshot == 0 &&
                      w.steps[0].action == A::Play && w.steps[1].action == A::Seek && near(w.steps[1].seekTime, 1.0) &&
                      w.steps[2].action == A::Stop);
            }
            e.sequencer().remove(e.sequencer().current());
            e.removeAnimation(e.indexOfAnimation(aid));
            e.removeLayer(e.indexOfId(tid));
            e.setFadesManual(false);
        }
        // Waves that are random, and keys joined by Bézier curves
        {
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            Engine::AnimTrack r;
            r.oscillator = true;
            r.wave = Engine::AnimWave::Random;
            r.period = 1;
            r.center = 0;
            r.amplitude = 1;
            r.seed = 7;
            const double r0 = r.valueAt(0, 0.1), r1 = r.valueAt(0, 1.1);
            CHECK(near(r.valueAt(0, 0.9), r0) && std::abs(r0) <= 1 && std::abs(r1) <= 1 && !near(r0, r1)); // held, then another
            CHECK(near(r.valueAt(0, 1.1), r1)); // the same at the same time: drawn and sought alike
            Engine::AnimTrack r2 = r;
            r2.seed = 8;
            CHECK(!near(r2.valueAt(0, 0.1), r0));
            r.wave = Engine::AnimWave::SmoothRandom; // from one value to the next, without a jump
            CHECK(near(r.valueAt(0, 0.0), r0) && near(r.valueAt(0, 0.9999), r1) && near(r.valueAt(0, 1.0), r1));
            Engine::AnimTrack b;
            b.keys = {{0, 0, Engine::kAnimBezier}, {2, 1, 0}};
            // Automatic handles: flat, a third of the way — an S, symmetric
            CHECK(near(b.valueAt(1, 0), 0.5) && b.valueAt(0.5, 0) < 0.2 && b.valueAt(1.5, 0) > 0.8);
            b.keys[0].outDt = 0.5;
            b.keys[0].outDv = 2; // a handle pulled up: it rises fast, beyond the linear way
            CHECK(b.valueAt(0.5, 0) > 0.4);
            double t1, v1, t2, v2;
            b.keys[0].outDt = 5; // never beyond the next key: the curve stays a function of time
            b.bezierHandles(0, &t1, &v1, &t2, &v2);
            CHECK(near(t1, 2) && near(t2, 2 - 2.0 / 3) && near(v2, 1));
        }
        // The animations of a layer's numbers: each one on its own clock, on as soon as it is made, saved with the layer
        {
            using A = Engine::AnimAction;
            e.setFadesManual(true);
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            const quint64 wid = e.layerId(e.addLayer("Wave", V));
            auto L = [&] { return e.layer(e.indexOfId(wid)); };
            L()->opacity = 1.0f;
            // Declared once: what can be animated is an attribute of each number
            {
                Engine::Lock lk(&e.mutex());
                QStringList paths;
                for (const Parameter *p : L()->parameters()) paths << p->path();
                CHECK(paths.contains("opacity") && paths.contains("rotation") && !paths.contains("soft_edge/left/width") && paths.contains("roi/left") &&
                      !paths.contains("volume") && !paths.contains("width")); // no sound, not a viewport
                Parameter *op = L()->parameter("opacity");
                CHECK(op && op->setNumber(2) && op->number() == 1.0); // kept within its limits
                CHECK(!L()->parameter("width") && L()->parameter("blend_mode")->setValue("add") && L()->blend == BlendMode::Add &&
                      !L()->parameter("blend_mode")->setValue("nothing")); // a choice: one of its keys only
                CHECK(L()->parameter("blend_mode")->reset() && L()->blend == BlendMode::Normal &&
                      L()->parameter("rotation")->info().ramp == ParamInfo::Ramp::Angle &&
                      !L()->parameter("color/enable")->info().animatable);
            }
            // A sine around the value, from Animate ▸ Sine
            Engine::Animation s = e.makeLayerAnim(wid, "opacity", int(Engine::AnimWave::Sine));
            CHECK(s.tracks.size() == 1 && s.tracks[0].oscillator && s.tracks[0].layer == wid &&
                  s.tracks[0].center - s.tracks[0].amplitude >= -1e-9 && s.tracks[0].center + s.tracks[0].amplitude <= 1 + 1e-9 &&
                  s.tracks[0].amplitude > 0.1); // at a bound: the whole range
            s.tracks[0].center = 0.5;
            s.tracks[0].amplitude = 0.5;
            s.tracks[0].period = 2;
            e.setLayerAnims(wid, {s});
            Engine::Animation now;
            CHECK(e.layerAnim(wid, "opacity", &now) && now.state == Engine::AnimState::Playing);
            e.advanceFades(0.5);
            CHECK(near(L()->opacity, 1.0));
            e.advanceFades(1.0);
            CHECK(near(L()->opacity, 0.0));
            // An edit goes on from where it is (no restart)
            s.tracks[0].amplitude = 0.25;
            e.setLayerAnims(wid, {s});
            CHECK(e.layerAnim(wid, "opacity", &now) && near(now.clock, 1.5) && now.state == Engine::AnimState::Playing);
            // Off: the number stays where it is; on again: from the start
            s.tracks[0].enabled = false;
            e.setLayerAnims(wid, {s});
            L()->opacity = 0.3f;
            e.advanceFades(0.5);
            CHECK(near(L()->opacity, 0.3));
            s.tracks[0].enabled = true;
            e.setLayerAnims(wid, {s});
            CHECK(e.layerAnim(wid, "opacity", &now) && near(now.clock, 0) && near(L()->opacity, 0.5));
            // One animation per number; keys from where the number is, out and back
            Engine::Animation k = e.makeLayerAnim(wid, "rotation", -1);
            CHECK(!k.tracks[0].oscillator && k.tracks[0].keys.size() == 3 && near(k.tracks[0].keys[0].v, 0));
            e.setLayerAnims(wid, {s, k, s});
            CHECK(e.layerAnims(wid).size() == 2);
            // Its transport, and the view of its card (not an edit: kept by the next edit)
            e.controlLayerAnim(wid, "opacity", A::Pause);
            CHECK(e.layerAnim(wid, "opacity", &now) && now.state == Engine::AnimState::Paused);
            e.setLayerAnimView(wid, "opacity", true, true);
            e.setLayerAnims(wid, {s, k});
            CHECK(e.layerAnim(wid, "opacity", &now) && now.pinned && now.folded);
            e.controlLayerAnim(wid, "opacity", A::Play);
            // A locked layer: its animations play on (they are its content)
            L()->locked = true;
            e.advanceFades(0.5);
            CHECK(near(L()->opacity, 0.75));
            L()->locked = false;
            // Undo: one step per edit, the arrows of a number merge; the other numbers' animations are left alone
            {
                QUndoStack st;
                Engine::Animation b0;
                e.layerAnim(wid, "opacity", &b0);
                Engine::Animation b1 = b0, b2 = b0;
                b1.speed = 2;
                b2.speed = 3;
                st.push(new cmd::SetLayerAnim(&e, wid, "opacity", b0, b1, "Speed", "speed"));
                st.push(new cmd::SetLayerAnim(&e, wid, "opacity", b1, b2, "Speed", "speed"));
                CHECK(st.count() == 1 && e.layerAnim(wid, "opacity", &now) && near(now.speed, 3));
                e.setLayerAnimOn(wid, "rotation", false); // meanwhile, by OSC
                st.undo();
                CHECK(e.layerAnim(wid, "opacity", &now) && near(now.speed, 1) && e.layerAnim(wid, "rotation", &now) &&
                      !now.tracks[0].enabled);
                e.setLayerAnimOn(wid, "rotation", true);
                // Removed, then back at its place
                Engine::Animation r;
                e.layerAnim(wid, "opacity", &r);
                st.push(new cmd::SetLayerAnim(&e, wid, "opacity", r, std::nullopt, "Remove"));
                CHECK(!e.layerAnim(wid, "opacity", nullptr) && e.layerAnims(wid).size() == 1);
                st.undo();
                CHECK(e.layerAnimIndex(wid, "opacity") == 0 && e.layerAnims(wid).size() == 2);
            }
            // OSC: /layer/<name>/anim/<the number>/enable, speed, rewind
            {
                OscServer server(&e);
                CHECK(server.handleMessage({"/layer/Wave/opacity/anim/speed", "f", {0.5}}) && e.layerAnim(wid, "opacity", &now) &&
                      near(now.speed, 0.5));
                CHECK(server.handleMessage({"/layer/Wave/rotation/anim/enable", "F", {false}}) &&
                      e.layerAnim(wid, "rotation", &now) && !now.tracks[0].enabled);
                CHECK(server.handleMessage({"/layer/Wave/opacity/anim/rewind", "N", {}}) && e.layerAnim(wid, "opacity", &now) &&
                      near(now.clock, 0));
                CHECK(!server.handleMessage({"/layer/Wave/roi/left/anim/enable", "T", {true}}));
                // The animation's settings, under its parameter
                CHECK(server.handleMessage({"/layer/Wave/rotation/anim/duration", "f", {6}}) &&
                      server.handleMessage({"/layer/Wave/rotation/anim/loop_mode", "s", {QStringLiteral("ping-pong")}}) &&
                      e.layerAnim(wid, "rotation", &now) && near(now.duration, 6) && now.loop == Engine::AnimLoop::PingPong);
                CHECK(server.handleMessage({"/layer/Wave/rotation/anim/wave", "s", {QStringLiteral("saw")}}) &&
                      server.handleMessage({"/layer/Wave/rotation/anim/period", "f", {3}}) && e.layerAnim(wid, "rotation", &now) &&
                      now.tracks[0].oscillator && now.tracks[0].wave == Engine::AnimWave::Saw && near(now.tracks[0].period, 3));
                CHECK(server.handleMessage({"/layer/Wave/rotation/anim/wave", "s", {QStringLiteral("keys")}}) &&
                      server.handleMessage({"/layer/Wave/rotation/anim/pause", "N", {}}) && e.layerAnim(wid, "rotation", &now) &&
                      !now.tracks[0].oscillator && now.tracks[0].keys.size() == 3);
                CHECK(!server.handleMessage({"/layer/Wave/rotation/anim/loop_mode", "s", {QStringLiteral("sideways")}}));
                {
                    Engine::Animation r;
                    e.layerAnim(wid, "rotation", &r);
                    r.duration = 4;
                    r.loop = Engine::AnimLoop::Loop;
                    e.setLayerAnim(wid, "rotation", &r);
                }
                // Every number at its own address (the one of the timelines and the animations), and the ones that go
                // together at their common address too; their limits are the declared ones
                CHECK(server.handleMessage({"/layer/Wave/position/x", "f", {16}}) &&
                      server.handleMessage({"/layer/Wave/position", "ff", {20, 10}}));
                {
                    double x = 0, y = 0;
                    e.animParamValue(wid, "position/x", &x);
                    e.animParamValue(wid, "position/y", &y);
                    CHECK(near(x, 20) && near(y, 10));
                }
                CHECK(server.handleMessage({"/layer/Wave/add", "fff", {0.1, 0.2, 0.3}}) &&
                      server.handleMessage({"/layer/Wave/add/g", "f", {0.5}}) && near(L()->color.add[0], 0.1) &&
                      near(L()->color.add[1], 0.5));
                CHECK(server.handleMessage({"/layer/Wave/blend_mode", "s", {QStringLiteral("difference")}}) &&
                      L()->blend == BlendMode::Difference);
                int status = 0;
                const QJsonObject op = QJsonDocument::fromJson(server.httpGet("/layer/Wave/opacity", &status)).object();
                CHECK(status == 200 && op.value("RANGE").toArray().at(0).toObject().value("MAX").toDouble() == 1 &&
                      op.value("TYPE").toString() == "f");
                L()->blend = BlendMode::Normal;
                L()->color.add[0] = L()->color.add[1] = L()->color.add[2] = 0;
            }
            // Saved with the layer: duplicated with it (on the copy), and back when the project is opened
            const QJsonObject lj = e.layerJson(e.indexOfId(wid));
            CHECK(lj.value("anims").toArray().size() == 2 &&
                  lj.value("anims").toArray().at(0).toObject().value("param").toString() == "opacity" &&
                  lj.value("anims").toArray().at(0).toObject().value("pinned").toBool());
            const int copy = e.duplicateLayer(e.indexOfId(wid));
            CHECK(copy >= 0 && e.layerAnims(e.layerId(copy)).size() == 2 &&
                  e.layerAnims(e.layerId(copy))[0].tracks[0].layer == e.layerId(copy));
            e.removeLayer(copy);
            // In a snapshot: their settings and their transport. One that plays in both goes on (no restart), with the
            // snapshot's settings; one the snapshot does not have stops and stays; even with another source
            {
                const QJsonArray cap = e.captureLayers();
                QJsonObject st;
                for (const QJsonValue &x : cap)
                    if (x.toObject().value("id").toString().toULongLong() == wid) st = x.toObject();
                CHECK(st.value("anims").toArray().size() == 2);
                // The values they drive are not held (the animations give them); the others are, and every one in
                // the undo of a recall
                const QJsonObject held = st.value("params").toObject();
                CHECK(!held.contains("opacity") && !held.contains("rotation") && held.contains("position/x"));
                for (const QJsonValue &x : e.captureLayers(true))
                    if (x.toObject().value("id").toString().toULongLong() == wid)
                        CHECK(x.toObject().value("params").toObject().contains("opacity"));
                e.advanceFades(0.3);
                const double clock0 = (e.layerAnim(wid, "opacity", &now), now.clock);
                QJsonArray an = st.value("anims").toArray();
                QJsonObject op = an.at(0).toObject();
                CHECK(op.value("param") == "opacity" && op.value("state") == "playing");
                op["speed"] = 2.0; // its settings change, it goes on
                an[0] = op;
                an.removeAt(1); // no rotation: it stops, it stays
                st["anims"] = an;
                e.controlLayerAnim(wid, "rotation", A::Play);
                e.applyLayers(QJsonArray{st}, 0.0);
                CHECK(e.layerAnim(wid, "opacity", &now) && now.state == Engine::AnimState::Playing && near(now.speed, 2) &&
                      now.clock >= clock0 - 1e-9);
                CHECK(e.layerAnims(wid).size() == 2 && e.layerAnim(wid, "rotation", &now) && now.state == Engine::AnimState::Stopped);
                // Paused where the snapshot has it, through a cut to another source
                op["state"] = "paused";
                op["clock"] = 0.25;
                an[0] = op;
                st["anims"] = an;
                st["source"] = QJsonObject{{"type", "image"}, {"path", tmp + "/redblue.png"}};
                e.applyLayers(QJsonArray{st}, 0.0); // the layer is made again
                CHECK(e.layer(e.indexOfId(wid))->type == SourceType::Image && e.layerAnims(wid).size() == 2 &&
                      e.layerAnim(wid, "opacity", &now) && now.state == Engine::AnimState::Paused && near(now.clock, 0.25));
                e.controlLayerAnim(wid, "opacity", A::Play);
                e.setLayerAnimOn(wid, "rotation", false);
            }
            CHECK(e.saveProject(tmp + "/anims.pinol", {}, &err));
            CHECK(e.loadProject(tmp + "/anims.pinol", nullptr, &err));
            CHECK(e.layerAnims(wid).size() == 2 && e.layerAnim(wid, "opacity", &now) && now.state == Engine::AnimState::Playing &&
                  near(now.speed, 2) && now.tracks[0].layer == wid && e.layerAnim(wid, "rotation", &now) &&
                  !now.tracks[0].enabled && now.state == Engine::AnimState::Stopped && now.tracks[0].keys.size() == 3);
            e.removeLayer(e.indexOfId(wid));
            e.setFadesManual(false);
        }
        // A snapshot, then a timeline on one of its numbers: the timeline ends where it ends, the rest fades on
        {
            using A = Engine::AnimAction;
            e.setFadesManual(true);
            auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
            const quint64 lid = e.layerId(e.addLayer("Chained", V));
            auto L = [&] { return e.layer(e.indexOfId(lid)); };
            L()->opacity = 1.0f;
            L()->color.temp = 0;
            QJsonObject state = e.layerJson(e.indexOfId(lid));
            setParam(state, "opacity", 0.0);
            setParam(state, "temp", 1000.0);
            e.applyLayers(QJsonArray{state}, 4.0); // a slow fade: opacity to 0, temperature to 1000
            e.advanceFades(0.5);
            Engine::Animation t;
            t.name = "Up";
            t.duration = 1;
            t.loop = Engine::AnimLoop::Once;
            Engine::AnimTrack tr;
            tr.layer = lid;
            tr.param = "opacity";
            tr.keys = {{0, 0.2, 0}, {1, 0.6, 0}};
            t.tracks = {tr};
            const quint64 tid = e.animation(e.addAnimation(t)).id;
            e.controlAnimation(tid, A::Play);
            e.advanceFades(0.5);
            CHECK(near(L()->opacity, 0.4));
            e.advanceFades(1.0); // over
            CHECK(near(L()->opacity, 0.6) && e.animation(e.indexOfAnimation(tid)).state == Engine::AnimState::Stopped);
            e.advanceFades(1.0); // the snapshot's fade goes on, but not on the opacity any more
            CHECK(near(L()->opacity, 0.6) && L()->color.temp > 600 && L()->color.temp < 999);
            e.advanceFades(2.0);
            CHECK(near(L()->opacity, 0.6) && near(L()->color.temp, 1000));
            // A snapshot recalled afterwards drives it again
            setParam(state, "opacity", 1.0);
            e.applyLayers(QJsonArray{state}, 1.0);
            e.advanceFades(1.0);
            CHECK(near(L()->opacity, 1.0));
            e.removeAnimation(e.indexOfAnimation(tid));
            e.removeLayer(e.indexOfId(lid));
            e.setFadesManual(false);
        }
        // A recall shows what was stored: a layer the snapshot does not know fades out and is hidden
        {
            const int xi = e.addLayer("Extra", V);
            const quint64 xid = e.layerId(xi);
            e.layer(xi)->opacity = 0.7f;
            Engine::Snapshot m = e.snapshot(0);
            const double keptFade = m.fade;
            m.fade = 0.5;
            e.setSnapshot(0, m);
            e.setFadesManual(true);
            e.recallSnapshot(0);
            CHECK(e.layer(e.indexOfId(xid))->enabled);
            e.advanceFades(0.25);
            CHECK(e.layer(e.indexOfId(xid))->opacity < 0.7f && e.layer(e.indexOfId(xid))->opacity > 0.0f);
            e.advanceFades(0.3);
            CHECK(!e.layer(e.indexOfId(xid))->enabled && std::abs(e.layer(e.indexOfId(xid))->opacity - 0.7f) < 1e-6);
            // Applying states (undo of a recall) leaves the others alone
            e.layer(e.indexOfId(xid))->enabled = true;
            e.applyLayers(e.snapshot(0).layers, 0);
            CHECK(e.layer(e.indexOfId(xid))->enabled);
            e.setFadesManual(false);
            e.removeLayer(e.indexOfId(xid));
            m.fade = keptFade;
            e.setSnapshot(0, m);
        }

        // The inspector edits what a snapshot holds (its layers' JSON): the recall then applies the new values,
        // and the composition does not move until it is recalled.
        {
            Engine::Snapshot edited = e.snapshot(0);
            QJsonObject l0 = edited.layers[0].toObject();
            setParam(l0, "opacity", 0.25);
            setParam(l0, "blend_mode", "screen");
            QJsonObject l1 = edited.layers[1].toObject();
            for (int c = 0; c < 4; ++c) setParam(l1, QStringLiteral("param/color/") + QChar("rgba"[c]), 0.25 * (c + 1)); // SolidColor
            l1["enable"] = true;
            edited.layers[0] = l0;
            edited.layers[1] = l1;
            e.layer(V + 0)->opacity = 0.8f;
            e.setSnapshot(0, edited);
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.8f) < 1e-6); // editing a snapshot does not touch the layers
            CHECK(std::abs(paramOf(e.snapshot(0).layers[0].toObject(), "opacity").toDouble() - 0.25) < 1e-9);
            e.recallSnapshot(0);
            CHECK(std::abs(e.layer(V + 0)->opacity - 0.25f) < 1e-6 && e.layer(V + 0)->blend == BlendMode::Screen);
            CHECK(e.layer(V + 1)->enabled);
            const IsfInstance *gen = e.layer(V + 1)->generator.get();
            double r = -1;
            for (const IsfInput &in : gen->inputs())
                if (in.name == "color") r = in.cValue[0];
            CHECK(std::abs(r - 0.25) < 1e-6);
            // Still edited after a save / load round trip
            CHECK(e.saveProject(tmp + "/snapshots.pinol", {}, &err));
            CHECK(e.loadProject(tmp + "/snapshots.pinol", nullptr, &err));
            CHECK(std::abs(paramOf(e.snapshot(0).layers[0].toObject(), "opacity").toDouble() - 0.25) < 1e-9);
        }
        e.newProject();
        CHECK(e.snapshotCount() == 0);
    }
    // Snapshots arranged in groups (nested): their order is the tree's; saved and loaded
    {
        e.newProject();
        e.setFadesManual(true);
        Engine::Snapshot m;
        m.fade = 0;
        quint64 ids[3];
        for (int k = 0; k < 3; ++k) {
            m.name = QStringLiteral("S%1").arg(k);
            ids[k] = e.snapshot(e.addSnapshot(m)).id;
        }
        CHECK(e.snapshotTree().size() == 3 && e.snapshotTree()[2].snapshot == ids[2]);
        using N = Engine::SnapshotNode;
        auto leaf = [](quint64 id) {
            N n;
            n.snapshot = id;
            return n;
        };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-3; };
        N inner; // a group in a group
        inner.name = "Inner";
        inner.children = {leaf(ids[0])};
        N outer;
        outer.name = "Outer";
        outer.children = {leaf(ids[2]), inner};
        e.setSnapshotTree({outer, leaf(ids[1]), leaf(999)}); // 999: unknown, dropped
        const auto t = e.snapshotTree();
        CHECK(t.size() == 2 && t[0].group && t[0].children.size() == 2 && t[0].children[1].group &&
              t[0].children[1].group != t[0].group);
        CHECK(e.snapshot(0).id == ids[2] && e.snapshot(1).id == ids[0] && e.snapshot(2).id == ids[1]); // depth first
        // A snapshot stored after one in a group goes in that group, right after it
        m.name = "S3";
        const int at = e.addSnapshot(m, 2); // after S0 (index 1, in Inner)
        CHECK(at == 2 && e.snapshotTree()[0].children[1].children.size() == 2);
        e.removeSnapshot(e.indexOfSnapshot(ids[0]));
        CHECK(e.snapshotTree()[0].children[1].children.size() == 1 && e.snapshotCount() == 3);
        CHECK(e.saveProject(tmp + "/tree.pinol", {}, &err));
        const auto saved = e.snapshotTree();
        e.newProject();
        CHECK(e.snapshotTree().empty());
        CHECK(e.loadProject(tmp + "/tree.pinol", nullptr, &err));
        CHECK(e.snapshotTree() == saved);

        // Presets: a few values, recalled with their fade (each its own time), the others left alone
        const int l = e.addLayer("P");
        e.setLayerImage(l, tmp + "/redblue.png", &err);
        const quint64 lid = e.layerId(l);
        Engine::Preset p;
        p.name = "Half";
        p.fade = 1.0;
        e.layer(l)->opacity = 0.5f;
        e.layer(l)->mapping.rotation = 90;
        p.values.push_back(e.presetValue(lid, "opacity"));
        Engine::PresetValue rot = e.presetValue(lid, "rotation");
        rot.time = 0; // a cut
        p.values.push_back(rot);
        p.values.push_back(e.presetValue(lid, "blend_mode"));
        CHECK(p.values[0].value.toDouble() == 0.5 && p.indexOf(lid, "rotation") == 1 && p.indexOf(lid, "scale/x") < 0);
        const int pi = e.addPreset(p);
        CHECK(pi == 0 && e.presetCount() == 1 && e.preset(0).id);
        e.layer(l)->opacity = 1.0f;
        e.layer(l)->mapping.rotation = 0;
        e.layer(l)->mapping.position = QPointF(0.2, 0.2);
        e.recallPreset(0);
        CHECK(near(e.layer(l)->mapping.rotation, 90) && near(e.layer(l)->opacity, 1.0));
        e.advanceFades(0.5);
        CHECK(e.layer(l)->opacity < 0.95f && e.layer(l)->opacity > 0.55f);
        e.advanceFades(0.6);
        CHECK(near(e.layer(l)->opacity, 0.5) && near(e.layer(l)->mapping.position.x(), 0.2)); // not in it: left alone
        // The composition's values too
        Engine::Preset c;
        c.fade = 0;
        Engine::PresetValue level = e.presetValue(0, "opacity");
        level.value = 0.3;
        c.values.push_back(level);
        e.addPreset(c);
        e.recallPreset(1);
        e.renderFrame(); // the level is set at the frame
        CHECK(near(e.compositionOpacity(), 0.3));
        e.fadeCompositionOpacity(1.0, 0);
        // Saved and loaded
        CHECK(e.saveProject(tmp + "/presets.pinol", {}, &err));
        e.newProject();
        CHECK(e.presetCount() == 0);
        CHECK(e.loadProject(tmp + "/presets.pinol", nullptr, &err));
        CHECK(e.presetCount() == 2 && e.preset(0).name == "Half" && e.preset(0).values.size() == 3 &&
              e.preset(0).values[1].time == 0 && e.preset(0).values[0].layer == lid);
        e.setFadesManual(false);
        e.newProject();
    }
    // The transport in snapshots and presets: played or paused, "restart on recall", the in / out points; a sound
    // crosses its levels when a snapshot changes its file, whatever the layer's transition
    {
        e.newProject();
        e.setFadesManual(true);
        auto params = [&](quint64 id) {
            QSet<QString> out;
            for (const ParamInfo &i : e.parameters(id)) out.insert(i.path);
            return out;
        };
        const int vi = e.addLayer(QStringLiteral("Clip"));
        CHECK(e.setLayerVideo(vi, root + "/media/h264.mp4", &err));
        const quint64 vid = e.layerId(vi);
        {
            const QSet<QString> p = params(vid);
            CHECK(p.contains("play") && p.contains("restart") && p.contains("in") && p.contains("out"));
        }
        auto playing = [&] {
            Engine::Lock lk(&e.mutex());
            return e.layer(e.indexOfId(vid))->playing;
        };
        auto position = [&] {
            Engine::Lock lk(&e.mutex());
            return e.layer(e.indexOfId(vid))->position();
        };
        auto snapshotOf = [&](const QString &name, std::function<void(QJsonObject &)> edit = {}) {
            Engine::Snapshot m;
            m.name = name;
            m.fade = 0;
            for (const QJsonValue &v : e.captureLayers()) {
                QJsonObject o = v.toObject();
                const bool me = o.value("id").toString().toULongLong() == vid;
                o["included"] = me;
                if (me && edit) edit(o);
                m.layers.append(o);
            }
            return e.addSnapshot(m);
        };
        e.setLayerPlaying(vi, true);
        const int sPlay = snapshotOf(QStringLiteral("Playing"));
        e.setLayerPlaying(vi, false);
        const int sPause = snapshotOf(QStringLiteral("Paused"));
        for (const QJsonValue &v : e.snapshot(sPause).layers) // the paused state is held
            if (v.toObject().value("id").toString().toULongLong() == vid)
                CHECK(v.toObject().value("params").toObject().value("play") == QJsonValue(false));
        e.recallSnapshot(sPlay);
        CHECK(playing());
        e.recallSnapshot(sPause);
        CHECK(!playing());
        // Restart on recall: from the in point, playing, wherever it was and paused or not
        const int sRestart = snapshotOf(QStringLiteral("Restart"), [](QJsonObject &o) {
            QJsonObject p = o.value("params").toObject();
            p["restart"] = true;
            p["play"] = false; // a restart plays all the same
            o["params"] = p;
        });
        e.seekLayer(e.indexOfId(vid), 2.5);
        e.setLayerPlaying(e.indexOfId(vid), false);
        CHECK(position() > 2.0);
        e.recallSnapshot(sRestart);
        std::printf("       restart on recall: position %.3f s, playing %d\n", position(), int(playing()));
        CHECK(playing() && position() < 0.1);
        CHECK(e.parameterValue(vid, "restart").toBool() == false); // an action, not a state
        // In / out points and the play state in a preset (recalled at once)
        Engine::Preset pr;
        pr.name = QStringLiteral("Range");
        pr.fade = 1.0;
        pr.values.push_back({vid, QStringLiteral("in"), QJsonValue(1.0), -1.0, QString()});
        pr.values.push_back({vid, QStringLiteral("out"), QJsonValue(2.0), -1.0, QString()});
        pr.values.push_back({vid, QStringLiteral("play"), QJsonValue(false), -1.0, QString()});
        const int pi = e.addPreset(pr);
        e.recallPreset(pi);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(e.indexOfId(vid));
            CHECK(std::abs(l->inPoint - 1.0) < 1e-6 && std::abs(l->outPoint - 2.0) < 1e-6 && !l->playing);
            CHECK(l->position() >= 1.0 - 1e-6 && l->position() <= 2.0 + 1e-6);
        }
        // A restart in a preset: from the new in point
        pr.values.push_back({vid, QStringLiteral("restart"), QJsonValue(true), -1.0, QString()});
        e.setPreset(pi, pr);
        e.seekLayer(e.indexOfId(vid), 1.8);
        e.recallPreset(pi);
        CHECK(playing() && std::abs(position() - 1.0) < 0.05);
        // Saved and opened: the play state comes back
        e.setLayerPlaying(e.indexOfId(vid), false);
        CHECK(e.saveProject(tmp + "/transport.pinol", {}, &err) && e.loadProject(tmp + "/transport.pinol", nullptr, &err));
        CHECK(!playing());

        // A sound whose file a snapshot changes: the levels cross, the layer's ISF transition is not used
        e.newProject();
        e.setDefaultTransition(root + "/../isf/transitions/Dissolve.fs");
        const QString tone2 = tmp + "/tone_b.wav";
        QFile::remove(tone2);
        CHECK(QFile::copy(root + "/media/tone.wav", tone2));
        const int ai = e.addLayer(QStringLiteral("Sound"));
        CHECK(e.setLayerAudio(ai, root + "/media/tone.wav", &err));
        const quint64 aid = e.layerId(ai);
        const int ii = e.addLayer(QStringLiteral("Picture"));
        CHECK(e.setLayerVideo(ii, root + "/media/h264.mp4", &err));
        const quint64 iid = e.layerId(ii);
        Engine::Snapshot sw;
        sw.name = QStringLiteral("Other files");
        sw.fade = 1.0;
        for (const QJsonValue &v : e.captureLayers()) {
            QJsonObject o = v.toObject();
            QJsonObject src = o.value("source").toObject();
            const quint64 id = o.value("id").toString().toULongLong();
            if (id == aid) src["path"] = tone2;
            if (id == iid) src["path"] = root + "/media/av.mp4";
            src.remove("relative_path");
            o["source"] = src;
            sw.layers.append(o);
        }
        e.recallSnapshot(e.addSnapshot(sw));
        CHECK(e.isTransitioning(aid) && e.isTransitioning(iid));
        std::printf("       sound transition: \"%s\", picture: \"%s\"\n", qPrintable(e.transitionShader(aid)),
                    qPrintable(QFileInfo(e.transitionShader(iid)).fileName()));
        CHECK(e.transitionShader(aid).isEmpty()); // a crossfade of the levels
        CHECK(QFileInfo(e.transitionShader(iid)).fileName() == QStringLiteral("Dissolve.fs")); // the default, for a picture
        e.setDefaultTransition(QString());
        e.setFadesManual(false);
        e.newProject();
    }
    } // snapshots

    if (area("masks")) {
    // 4e. Shape masks (Masks.h): their alpha, JSON, SVG, and the picture through them
    {
        // Raster: a rectangle keeps its inside; inverted, it cuts it out; feathering softens the edge where asked
        Mask r = Mask::rectangle(QRectF(0.25, 0.25, 0.5, 0.5));
        r.name = "R";
        auto gray = [](const QImage &img, int x, int y) { return qGray(img.pixel(x, y)); };
        QImage a = masks::raster({r}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(!a.isNull() && gray(a, 50, 50) == 255 && gray(a, 5, 5) == 0);
        CHECK(masks::raster({r}, Mask::Attach::Composition, QSize(100, 100)).isNull()); // none there
        Mask inv = r;
        inv.invert = true;
        a = masks::raster({inv}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 50, 50) == 0 && gray(a, 5, 5) == 255);
        a = masks::raster({r, Mask::rectangle(QRectF(0.4, 0.4, 0.2, 0.2))}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 50, 50) == 255); // added
        std::vector<Mask> hole = {r, Mask::rectangle(QRectF(0.4, 0.4, 0.2, 0.2))};
        hole[1].mode = Mask::Mode::Subtract;
        a = masks::raster(hole, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 50, 50) == 0 && gray(a, 30, 30) == 255 && gray(a, 5, 5) == 0); // a hole in it
        // Combine modes, in the list's order: a first Subtract starts from the whole picture
        Mask cut = Mask::rectangle(QRectF(0.4, 0.4, 0.2, 0.2));
        cut.mode = Mask::Mode::Subtract;
        a = masks::raster({cut}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 50, 50) == 0 && gray(a, 5, 5) == 255);
        Mask left = Mask::rectangle(QRectF(0.2, 0.2, 0.4, 0.6)), right = Mask::rectangle(QRectF(0.4, 0.2, 0.4, 0.6));
        right.mode = Mask::Mode::Intersect;
        a = masks::raster({left, right}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 30, 50) == 0 && gray(a, 50, 50) == 255 && gray(a, 70, 50) == 0); // only where both are
        right.mode = Mask::Mode::Multiply;
        a = masks::raster({left, right}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 30, 50) == 0 && gray(a, 50, 50) == 255);
        right.mode = Mask::Mode::Difference;
        a = masks::raster({left, right}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 30, 50) == 255 && gray(a, 50, 50) == 0 && gray(a, 70, 50) == 255); // one or the other
        right.mode = Mask::Mode::Add;
        right.invert = true; // inside out, then added: all but what is right of the left one's
        a = masks::raster({left, right}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 5, 5) == 255 && gray(a, 70, 50) == 0);
        right.invert = false;
        right.mode = Mask::Mode::Subtract;
        left.enabled = false; // off: the list starts with the cut
        a = masks::raster({left, right}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 30, 50) == 255 && gray(a, 50, 50) == 0);
        left.enabled = true;
        CHECK(masks::modeName(Mask::Mode::Intersect) == "Intersect" && masks::modeSymbol(Mask::Mode::Subtract) == QString(QChar(0x2212)));
        Mask soft = r;
        soft.feather = 0.2; // 20 px over 100
        a = masks::raster({soft}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(std::abs(gray(a, 25, 50) - 128) < 40 && gray(a, 50, 50) == 255 && gray(a, 10, 50) < 30);
        soft.featherMode = Mask::Feather::Inner;
        a = masks::raster({soft}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 25, 50) < 40 && gray(a, 50, 50) > 230);
        soft.featherMode = Mask::Feather::Outer;
        a = masks::raster({soft}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 26, 50) > 215 && gray(a, 8, 50) < 60);
        Mask e2 = Mask::ellipse(QRectF(0, 0, 1, 1));
        a = masks::raster({e2}, Mask::Attach::Surface, QSize(100, 100));
        CHECK(gray(a, 50, 50) == 255 && gray(a, 2, 2) == 0 && gray(a, 50, 3) > 200); // round, touching the edges

        // JSON and names
        soft.name = "Soft";
        soft.mode = Mask::Mode::Difference;
        const std::vector<Mask> back = masks::fromJson(masks::toJson({r, soft, e2}));
        CHECK(back.size() == 3 && back[0] == r && back[1] == soft && back[2] == e2);
        CHECK(masks::uniqueName({r}) == "Mask-1");
        Mask named = r;
        named.name = "Mask-1";
        CHECK(masks::uniqueName({named}) == "Mask-2");

        // SVG: there and back, and a file from elsewhere (rect, circle, path with curves and relative moves, polygon)
        QString note;
        const std::vector<Mask> svgBack = masks::fromSvg(masks::toSvg({r, soft, e2}, QSize(640, 360)).toUtf8(), &note);
        CHECK(svgBack.size() == 3 && note.isEmpty());
        if (svgBack.size() == 3) {
            CHECK(svgBack[1].name == "Soft" && std::abs(svgBack[1].feather - 0.2) < 1e-6 && svgBack[1].featherMode == Mask::Feather::Outer &&
                  svgBack[1].mode == Mask::Mode::Difference);
            bool same = svgBack[2].nodes.size() == e2.nodes.size();
            for (size_t i = 0; same && i < e2.nodes.size(); ++i)
                same = (svgBack[2].nodes[i].p - e2.nodes[i].p).manhattanLength() < 1e-3
                       && (svgBack[2].nodes[i].out - e2.nodes[i].out).manhattanLength() < 1e-3;
            CHECK(same);
        }
        const QByteArray other = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 200 100'>"
                                 "<rect x='0' y='0' width='100' height='50'/>"
                                 "<circle cx='150' cy='50' r='25' fill='black'/>"
                                 "<path d='M10 60 l 40 0 C 60 70, 60 90, 50 100 L10,100z'/>"
                                 "<polygon points='120,90 140,60 160,90'/></svg>";
        const std::vector<Mask> got = masks::fromSvg(other, &note);
        CHECK(got.size() == 4);
        if (got.size() == 4) {
            CHECK(got[0].shape == Mask::Shape::Rectangle && (got[0].nodes[2].p - QPointF(0.5, 0.5)).manhattanLength() < 1e-9);
            CHECK(got[1].invert && got[1].shape == Mask::Shape::Ellipse);
            CHECK(got[2].nodes.size() == 4 && (got[2].nodes[1].p - QPointF(0.25, 0.6)).manhattanLength() < 1e-9 &&
                  (got[2].nodes[1].out - QPointF(0.3, 0.7)).manhattanLength() < 1e-9);
            CHECK(got[3].nodes.size() == 3);
        }
        CHECK(masks::fromSvg("<svg/>", &note).empty() && !note.isEmpty());

        // The picture through them: a red layer over the composition, its left half kept, then a hole in the
        // composition's middle; a viewport's own mask
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 32));
        QImage red(32, 16, QImage::Format_RGB32);
        red.fill(qRgb(255, 0, 0));
        red.save(tmp + "/sred.png");
        const int li = e.addLayer("Shaped");
        CHECK(e.setLayerImage(li, tmp + "/sred.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 lid = e.layerId(li);
        auto px = [&](double fx, double fy) {
            e.renderFrame();
            const QImage g = e.grabOutput();
            return g.pixelColor(std::min(g.width() - 1, int(g.width() * fx)), std::min(g.height() - 1, int(g.height() * fy)));
        };
        CHECK(px(0.75, 0.5).red() > 250);
        e.setLayerMasks(lid, {Mask::rectangle(QRectF(0, 0, 0.5, 1))});
        CHECK(px(0.25, 0.5).red() > 250 && px(0.75, 0.5).red() < 5);
        Mask middle = Mask::rectangle(QRectF(0.25, 0, 0.5, 1));
        middle.attach = Mask::Attach::Composition;
        middle.invert = true;
        e.setLayerMasks(lid, {middle});
        CHECK(px(0.1, 0.5).red() > 250 && px(0.5, 0.5).red() < 5 && px(0.9, 0.5).red() > 250);
        // A composition mask stays when the layer moves; a surface mask goes with it
        e.layer(li)->mapping.translate(QPointF(0.25, 0)); // its left edge at the middle
        CHECK(px(0.5, 0.5).red() < 5 && px(0.85, 0.5).red() > 250);
        e.layer(li)->mapping.translate(QPointF(-0.25, 0));
        // Saved with the layer
        const QJsonObject lj = e.layerJson(e.indexOfId(lid));
        CHECK(lj.value("masks").toArray().size() == 1);
        // In snapshots: the same shapes move there over the fade (points, handles, soft edge), others are set at once
        {
            e.setLayerMasks(lid, {Mask::rectangle(QRectF(0, 0, 0.5, 1))});
            const QJsonObject before = e.layerJson(e.indexOfId(lid)); // the half on the left, sharp
            auto M = [&] { return e.layer(e.indexOfId(lid))->masks; };
            Mask moved = Mask::rectangle(QRectF(0.5, 0, 0.5, 1));
            moved.attach = Mask::Attach::Composition;
            moved.invert = true;
            moved.feather = 0.2;
            e.setLayerMasks(lid, {moved});
            e.applyLayers(QJsonArray{before}, 1.0);
            CHECK(e.isFading() && std::abs(M()[0].nodes[0].p.x() - 0.5) < 1e-9 && std::abs(M()[0].feather - 0.2) < 1e-9);
            CHECK(!M()[0].invert && M()[0].attach == Mask::Attach::Surface); // what does not move: at once
            e.advanceFades(0.5);
            CHECK(M()[0].nodes[0].p.x() > 0.05 && M()[0].nodes[0].p.x() < 0.45 && M()[0].feather > 0.01 && M()[0].feather < 0.19);
            e.advanceFades(0.6);
            CHECK(M()[0] == masks::fromJson(before.value("masks").toArray())[0] && !e.isFading());
            CHECK(px(0.25, 0.5).red() > 250 && px(0.75, 0.5).red() < 5);
            // Its own time (a cut here)
            QJsonObject cut = before;
            cut["timing"] = QJsonObject{{"masks", 0}};
            e.setLayerMasks(lid, {moved});
            e.applyLayers(QJsonArray{cut}, 1.0); // (the rest of the recall still fades: its routing)
            CHECK(std::abs(M()[0].nodes[0].p.x()) < 1e-9);
            e.advanceFades(0.3);
            CHECK(std::abs(M()[0].nodes[0].p.x()) < 1e-9);
            e.advanceFades(1.0);
            // Another shape (as many masks, not as many points): at once
            e.setLayerMasks(lid, {Mask::triangle(QRectF(0.5, 0, 0.5, 1))});
            e.applyLayers(QJsonArray{before}, 1.0);
            CHECK(M().size() == 1 && M()[0].nodes.size() == 4 && std::abs(M()[0].nodes[0].p.x()) < 1e-9);
            e.advanceFades(0.3);
            CHECK(M().size() == 1 && std::abs(M()[0].nodes[0].p.x()) < 1e-9);
            e.advanceFades(1.0);
            // A snapshot without masks takes them away; set by hand during a fade, they stay as set
            QJsonObject none = before;
            none.remove("masks");
            e.applyLayers(QJsonArray{none}, 1.0);
            CHECK(M().empty());
            e.setLayerMasks(lid, {moved});
            e.applyLayers(QJsonArray{before}, 1.0);
            e.advanceFades(0.3);
            e.setLayerMasks(lid, {moved});
            e.advanceFades(1.0);
            CHECK(std::abs(M()[0].nodes[0].p.x() - 0.5) < 1e-9);
            e.setLayerMasks(lid, masks::fromJson(before.value("masks").toArray()));
        }
        e.setLayerMasks(lid, {});
        CHECK(px(0.5, 0.5).red() > 250 && !e.layerJson(e.indexOfId(lid)).contains("masks"));
        // A viewport's mask: its picture, color included (its screen shows it opaque)
        const quint64 vp = e.mainViewportId();
        Mask vm = Mask::rectangle(QRectF(0, 0, 0.5, 1));
        e.setLayerMasks(vp, {vm});
        e.renderFrame();
        const QImage vg = e.grabViewport(vp);
        CHECK(!vg.isNull() && vg.pixelColor(vg.width() / 4, vg.height() / 2).red() > 250 &&
              vg.pixelColor(vg.width() * 3 / 4, vg.height() / 2).red() < 5);
        e.setLayerMasks(vp, {});
        CHECK(e.maskFrame(vp) == e.layer(e.indexOfId(vp))->viewportSize());
    }
    // 4f. Effect masks: an effect applies where another layer's picture is white
    {
        QFile inv(tmp + "/Invert.fs");
        inv.open(QIODevice::WriteOnly);
        inv.write("/*{ \"ISFVSN\": \"2\", \"INPUTS\": [ { \"NAME\": \"inputImage\", \"TYPE\": \"image\" } ] }*/\n"
                  "void main() { vec4 c = IMG_THIS_PIXEL(inputImage); gl_FragColor = vec4(1.0 - c.rgb, c.a); }\n");
        inv.close();
        QImage red(32, 16, QImage::Format_RGB32), half(32, 16, QImage::Format_RGB32), grey(32, 16, QImage::Format_RGB32);
        red.fill(qRgb(255, 0, 0));
        grey.fill(qRgb(128, 128, 128));
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 32; ++x) half.setPixel(x, y, x < 16 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        red.save(tmp + "/mred.png");
        half.save(tmp + "/mhalf.png");
        grey.save(tmp + "/mgrey.png");
        const int mi = e.addLayer("MaskSrc", V);
        CHECK(e.setLayerImage(mi, tmp + "/mhalf.png", &err));
        e.layer(mi)->enabled = false; // a hidden layer still masks
        const quint64 maskId = e.layerId(mi);
        const int li = e.addLayer("Masked", V);
        CHECK(e.setLayerImage(li, tmp + "/mred.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 lid = e.layerId(li);
        CHECK(e.addEffect(li, tmp + "/Invert.fs") == 0);
        auto at = [&](double fx) {
            e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            return g.pixelColor(int(g.width() * fx), g.height() / 2);
        };
        CHECK(at(0.25).red() < 5 && at(0.75).red() < 5); // no mask: everywhere
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err));
        QColor l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.green() > 250 && r.red() > 250 && r.green() < 5); // white: the effect, black: the input
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, true, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && r.red() < 5); // inverted
        CHECK(e.layerJson(e.indexOfId(lid)).value("fx").toArray().at(0).toObject().value("mask").toString() ==
              QString::number(maskId));
        // Mask picture before or after the mask layer's effects (no effect on the mask here: same picture)
        CHECK(e.setEffectMaskTap(e.indexOfId(lid), 0, true));
        CHECK(e.layerJson(e.indexOfId(lid)).value("fx").toArray().at(0).toObject().value("mask_tap").toString() == "prefx");
        l = at(0.25);
        CHECK(l.red() > 250 && at(0.75).red() < 5);
        CHECK(e.setEffectMaskTap(e.indexOfId(lid), 0, false));
        // Grey: in proportion
        CHECK(e.setLayerImage(e.indexOfId(maskId), tmp + "/mgrey.png", &err));
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err));
        l = at(0.5);
        CHECK(l.red() > 100 && l.red() < 160 && l.green() > 100 && l.green() < 160);
        // Refused: itself, a viewport, a layer that already depends on this one
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, lid, false));
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, e.mainViewportId(), false));
        CHECK(e.layerDependsOn(lid, maskId)); // a mask is a dependency: the mask layer cannot take this one as source
        CHECK(!e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        CHECK(e.setEffectMask(e.indexOfId(lid), 0, 0, false, &err));
        CHECK(e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        err.clear();
        CHECK(!e.setEffectMask(e.indexOfId(lid), 0, maskId, false, &err) && err.contains("feed back"));
        // The mask layer gone: the effect applies everywhere again
        e.removeLayer(e.indexOfId(maskId));
        CHECK(at(0.25).red() < 5 && at(0.75).red() < 5);
        e.removeLayer(e.indexOfId(lid));
    }
    // 4f bis. The color section through a mask: red with all its red removed, only where the mask is white
    {
        const int mi = e.addLayer("ColorMaskSrc", V);
        CHECK(e.setLayerImage(mi, tmp + "/mhalf.png", &err));
        e.layer(mi)->enabled = false;
        const quint64 maskId = e.layerId(mi);
        const int li = e.addLayer("Colored", V);
        CHECK(e.setLayerImage(li, tmp + "/mred.png", &err));
        e.layer(li)->mapping.resetCorners();
        const quint64 lid = e.layerId(li);
        e.layer(li)->color.remove[0] = 1.0f; // red filtered out: black
        e.layer(li)->color.add[2] = 1.0f;    // blue light added
        auto at = [&](double fx) {
            e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            return g.pixelColor(int(g.width() * fx), g.height() / 2);
        };
        QColor l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.blue() > 250 && r.red() < 5 && r.blue() > 250); // no mask: everywhere
        CHECK(e.setColorMask(e.indexOfId(lid), maskId, false, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() < 5 && l.blue() > 250 && r.red() > 250 && r.blue() < 5); // white: colored, black: as it was
        CHECK(e.setColorMask(e.indexOfId(lid), maskId, true, &err));
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && l.blue() < 5 && r.red() < 5 && r.blue() > 250); // inverted
        // The section switched off: no color, mask or not
        e.layer(e.indexOfId(lid))->color.enabled = false;
        l = at(0.25), r = at(0.75);
        CHECK(l.red() > 250 && r.red() > 250);
        e.layer(e.indexOfId(lid))->color.enabled = true;
        // Saved, read back, recalled by a snapshot
        const QJsonObject json = e.layerJson(e.indexOfId(lid));
        CHECK(json.value("mask").toString() == QString::number(maskId) && paramOf(json, "mask/invert").toBool());
        CHECK(e.setColorMask(e.indexOfId(lid), 0, false, &err));
        e.replaceLayerJson(e.indexOfId(lid), json);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *x = e.layer(e.indexOfId(lid));
            CHECK(x && x->color.maskLayer == maskId && x->color.maskInvert);
        }
        // Refused: itself, a viewport, a feedback; a mask counts as a dependency
        CHECK(!e.setColorMask(e.indexOfId(lid), lid, false));
        CHECK(!e.setColorMask(e.indexOfId(lid), e.mainViewportId(), false));
        CHECK(e.layerDependsOn(lid, maskId));
        CHECK(!e.setLayerSourceLayer(e.indexOfId(maskId), lid, LayerTap::PostFx, &err));
        // The mask layer gone: the color applies everywhere again, the reference dropped on reading
        e.removeLayer(e.indexOfId(maskId));
        l = at(0.25), r = at(0.75);
        CHECK(l.blue() > 250 && r.blue() > 250);
        e.fixLayerReferences();
        CHECK(e.layer(e.indexOfId(lid))->color.maskLayer == 0);
        e.removeLayer(e.indexOfId(lid));
    }
    } // masks

    if (area("rendering")) {
    // A delayed GPU share reads the old front at the end of this tick.
    // The renderer must not write into that buffer on the next tick.
    {
        int oldFront = 0, back = 1;
        for (int k = 0; k < 12; ++k) {
            const int next = Layer::nextViewportBack(oldFront, back, true);
            CHECK(next >= 0 && next < 3 && next != oldFront && next != back);
            oldFront = back;
            back = next;
        }
        CHECK(Layer::nextViewportBack(0, 1, false) == 0);
        CHECK(Layer::nextViewportBack(1, 2, false) == 1);
        // Toggling modes after a triple-buffer tick must never select the front.
        CHECK(Layer::nextViewportBack(2, 0, false) == 2);
    }
    // Test card: a viewport shows it instead of its picture: the corners 1 red, 2 green, 3 blue, 4 yellow clockwise from
    // the top left, a dark plate in the middle, and motion
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        const quint64 vp = e.mainViewportId();
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp))->testCard = true;
        }
        e.renderFrame();
        const QImage a = e.grabViewport(vp);
        CHECK(!a.isNull());
        if (!a.isNull()) {
            const int w = a.width(), h = a.height(), d = std::max(3, std::min(w, h) / 40);
            const QColor tl = a.pixelColor(d, d), tr = a.pixelColor(w - 1 - d, d), br = a.pixelColor(w - 1 - d, h - 1 - d),
                         bl = a.pixelColor(d, h - 1 - d);
            CHECK(tl.red() > 200 && tl.green() < 80 && tl.blue() < 80);
            CHECK(tr.green() > 150 && tr.red() < 80 && tr.blue() < 120);
            CHECK(br.blue() > 200 && br.red() < 80);
            CHECK(bl.red() > 200 && bl.green() > 180 && bl.blue() < 80);
            QThread::msleep(300);
            e.renderFrame();
            CHECK(e.grabViewport(vp) != a); // it moves
        }
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp))->testCard = false;
        }
        e.renderFrame();
        const QImage off = e.grabViewport(vp);
        CHECK(!off.isNull() && off.pixelColor(2, 2).red() < 5); // its picture again (an empty composition)
        const QImage card = TestCard::still(QStringLiteral("Front"), QSize(320, 180));
        CHECK(card.size() == QSize(320, 180) && card.pixelColor(4, 4).red() > 200);
    }
    // Live inputs: a layer's source by URI (a camera, NDI, OMT, Syphon); the test one is a solid color
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 36));
        const int li = e.addLayer("Live");
        const quint64 lid = e.layerId(li);
        CHECK(e.setLayerInput(li, "input:test:0,255,0", &err));
        for (int k = 0; k < 3; ++k) e.renderFrame();
        QImage out = e.grabOutput();
        CHECK(out.pixelColor(32, 18).green() > 250 && out.pixelColor(32, 18).red() < 5);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(e.indexOfId(lid));
            CHECK(l->type == SourceType::Input && l->srcWidth == 64 && l->srcHeight == 36);
        }
        const QJsonObject src = e.layerJson(e.indexOfId(lid)).value("source").toObject();
        CHECK(src.value("type") == "input" && src.value("uri") == "input:test:0,255,0");
        CHECK(liveInputKind("input:ndi:HOST (Cam)") == "NDI" && liveInputName("input:ndi:HOST (Cam)") == "HOST (Cam)" &&
              isLiveInputUri("input:camera:x") && !isLiveInputUri("/media/a.mp4"));
        // Not there: refused, with why; an unknown kind too
        CHECK(!e.setLayerInput(li, "input:nothing:x", &err) && !err.isEmpty());
        // Saved and loaded; a snapshot holding another input changes the source
        CHECK(e.saveProject(tmp + "/input.pinol", {}, &err));
        e.newProject();
        CHECK(e.loadProject(tmp + "/input.pinol", nullptr, &err));
        CHECK(e.indexOfId(lid) >= 0);
        QJsonObject state = e.layerJson(e.indexOfId(lid));
        QJsonObject s2 = state.value("source").toObject();
        s2["uri"] = "input:test:255,0,0";
        state["source"] = s2;
        e.applyLayers(QJsonArray{state}, 0.0);
        for (int k = 0; k < 3; ++k) e.renderFrame();
        out = e.grabOutput();
        CHECK(out.pixelColor(32, 18).red() > 250 && out.pixelColor(32, 18).green() < 5);
        // Listing does not fail without devices or libraries
        (void)Engine::liveInputs();
        e.newProject();
    }
    // 4d'. Blend modes that take away: Subtract and Difference (against what is drawn below)
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(16, 16));
        QImage up(16, 16, QImage::Format_RGB32), down(16, 16, QImage::Format_RGB32);
        up.fill(qRgb(50, 200, 20));
        down.fill(qRgb(200, 120, 50));
        up.save(tmp + "/bl_up.png");
        down.save(tmp + "/bl_down.png");
        const int low = e.addLayer("Bottom");
        const quint64 bottomId = e.layerId(low);
        const int high = e.addLayer("Top"); // new layers go to the top of the list
        const quint64 topId = e.layerId(high);
        CHECK(e.setLayerImage(e.indexOfId(topId), tmp + "/bl_up.png", &err));
        CHECK(e.setLayerImage(e.indexOfId(bottomId), tmp + "/bl_down.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(topId))->mapping.resetCorners();
            e.layer(e.indexOfId(bottomId))->mapping.resetCorners();
        }
        auto grab = [&](BlendMode m) {
            {
                Engine::Lock lk(&e.mutex());
                e.layer(e.indexOfId(topId))->blend = m;
            }
            for (int k = 0; k < 2; ++k) e.renderFrame();
            return e.grabOutput().pixelColor(8, 8);
        };
        auto near = [](const QColor &c, int r, int g, int b) {
            return std::abs(c.red() - r) <= 3 && std::abs(c.green() - g) <= 3 && std::abs(c.blue() - b) <= 3;
        };
        CHECK(near(grab(BlendMode::Subtract), 150, 0, 30));   // below minus above, never under zero
        CHECK(near(grab(BlendMode::Difference), 150, 80, 30)); // |below - above|
        // Allocation/validation failure of Difference's destination-copy
        // target must never blit into framebuffer 0. Preserve the picture by
        // falling back to ordinary source-over for that frame only.
        Engine::failNextDifferenceCopyForTests();
        e.renderFrame();
        // renderPass() prepares the viewport before composite() assembles the
        // master output. The one-shot failure affects that FIRST Difference
        // pass, so inspect the viewport (not the later master output).
        const QImage viewportOnFailure = e.grabViewport(e.mainViewportId());
        const QColor fallbackPixel = viewportOnFailure.isNull()
                                         ? QColor() : viewportOnFailure.pixelColor(viewportOnFailure.width() / 2,
                                                                                   viewportOnFailure.height() / 2);
        std::printf("Difference copy unavailable -> viewport RGB %d,%d,%d\n",
                    fallbackPixel.red(), fallbackPixel.green(), fallbackPixel.blue());
        CHECK(!viewportOnFailure.isNull() && near(fallbackPixel, 50, 200, 20));
        CHECK(near(grab(BlendMode::Difference), 150, 80, 30)); // mode recovers next frame
        CHECK(near(grab(BlendMode::Normal), 50, 200, 20));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(topId))->opacity = 0.5f;
        }
        CHECK(near(grab(BlendMode::Difference), 175, 20, 40)); // the layer at half strength (its color halfway to black)
        CHECK(blendModeFromKey(blendModeKey(BlendMode::Subtract)) == BlendMode::Subtract &&
              blendModeFromKey(blendModeKey(BlendMode::Difference)) == BlendMode::Difference);
    }

    // 4d-bis. Color depth: the render targets follow the project's setting
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(16, 16));
        QImage col(16, 16, QImage::Format_RGB32);
        col.fill(qRgb(200, 100, 50));
        col.save(tmp + "/dp_col.png");
        const int li = e.addLayer("Depth");
        CHECK(e.setLayerImage(li, tmp + "/dp_col.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
            e.layer(li)->opacity = 0.5f;
        }
        { // nothing chosen on this machine: 16 bits
            Engine::RenderSettings none = e.renderDefaults();
            none.depth = -1;
            e.setRenderDefaults(none);
            for (int k = 0; k < 2; ++k) e.renderFrame();
            CHECK(e.effectiveRender().depth == 10 && renderBits() == 16);
            none.depth = 8; // 8 bits chosen
            e.setRenderDefaults(none);
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        const QColor c8 = e.grabOutput().pixelColor(8, 8);
        CHECK(renderBits() == 8);
        Engine::RenderSettings rs = e.renderSettings();
        rs.depth = 10;
        e.setRenderSettings(rs);
        for (int k = 0; k < 3; ++k) e.renderFrame();
        const QColor c10 = e.grabOutput().pixelColor(8, 8);
        CHECK(renderBits() == 16 && e.effectiveRender().depth == 10);
        CHECK(std::abs(c10.red() - c8.red()) <= 1 && std::abs(c10.green() - c8.green()) <= 1 && std::abs(c10.blue() - c8.blue()) <= 1);
        CHECK(std::abs(c10.red() - 100) <= 1 && std::abs(c10.green() - 50) <= 1); // half of (200, 100, 50)
        // Rendering is the machine's (Settings): a project does not keep it
        const QString p = tmp + "/depth.pinol";
        CHECK(e.saveProject(p, QJsonObject(), &err));
        e.newProject();
        CHECK(e.renderSettings().depth == -1);
        CHECK(e.loadProject(p, nullptr, &err));
        CHECK(e.renderSettings().depth == -1 && !readJson(p).contains("render"));
        Engine::RenderSettings d = e.renderDefaults();
        d.depth = 10;
        e.setRenderDefaults(d);
        e.newProject();
        CHECK(e.renderSettings().depth == -1 && e.effectiveRender().depth == 10);
        d.depth = 8;
        e.setRenderDefaults(d);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        CHECK(renderBits() == 8);
    }

    // 4e. Viewports: windows onto one composition, routing, groups inside groups
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 16));
        const int v1 = e.viewports().first();
        e.setViewportSize(v1, QSize(32, 16));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(v1)->mapping; // left half of the composition
            m.position = {0.25, 0.5};
            m.size = {0.5, 1};
        }
        const int v2i = e.addViewport(QStringLiteral("Right"), QSize(32, 16));
        const quint64 vp1 = e.layerId(e.viewports().first()), vp2 = e.layerId(v2i);
        {
            Engine::Lock lk(&e.mutex());
            const QRectF r = e.layer(e.indexOfId(vp2))->mapping.bounds(); // placed to the right of the first
            CHECK(std::abs(r.left() - 0.5) < 1e-9 && std::abs(r.width() - 0.5) < 1e-9);
        }
        CHECK(e.viewports().size() == 2 && e.viewports()[0] == 0 && e.viewports()[1] == 1); // they come first
        // One layer over the whole composition: red on the left half, blue on the right half
        QImage wide(64, 16, QImage::Format_RGB32);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 64; ++x) wide.setPixel(x, y, x < 32 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        wide.save(tmp + "/wide.png");
        const int li = e.addLayer("Wide");
        CHECK(li == 2 && e.setLayerImage(li, tmp + "/wide.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
        }
        for (int k = 0; k < 3; ++k) e.renderFrame();
        QImage a = e.grabViewport(vp1), b = e.grabViewport(vp2);
        CHECK(a.size() == QSize(32, 16) && b.size() == QSize(32, 16));
        CHECK(a.pixelColor(16, 8).red() > 250 && a.pixelColor(16, 8).blue() < 5); // each sees its part
        CHECK(b.pixelColor(16, 8).blue() > 250 && b.pixelColor(16, 8).red() < 5);
        // Each viewport in a window of its own: both windows get their picture (each one has its own context)
        {
            QWindow w1, w2;
            int x = 100;
            for (QWindow *w : {&w1, &w2}) { // side by side: a window under another one would be grabbed as it
                w->setSurfaceType(QSurface::OpenGLSurface);
                w->setFormat(QSurfaceFormat::defaultFormat());
                w->setGeometry(x, 100, 64, 32);
                x += 200;
                w->create();
                w->show();
            }
            QElapsedTimer t;
            t.start();
            while ((!w1.isExposed() || !w2.isExposed()) && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            e.setViewportWindow(vp1, &w1);
            e.setViewportWindow(vp2, &w2);
            e.setViewportExposed(vp1, true, w1.size() * w1.devicePixelRatio());
            e.setViewportExposed(vp2, true, w2.size() * w2.devicePixelRatio());
            for (int k = 0; k < 3; ++k) {
                e.renderFrame();
                QCoreApplication::processEvents();
            }
            const QImage g1 = w1.screen()->grabWindow(w1.winId()).toImage(), g2 = w2.screen()->grabWindow(w2.winId()).toImage();
            CHECK(!g1.isNull() && !g2.isNull());
            if (!g1.isNull() && !g2.isNull()) {
                const QColor c1 = g1.pixelColor(g1.width() / 2, g1.height() / 2), c2 = g2.pixelColor(g2.width() / 2, g2.height() / 2);
                CHECK(c1.red() > 200 && c1.blue() < 50);
                CHECK(c2.blue() > 200 && c2.red() < 50);
            }
            e.setViewportWindow(vp1, nullptr);
            e.setViewportWindow(vp2, nullptr);
        }
        // Publishing follows a viewport changed without going through setPublishSettings (undo, snapshots, load)
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->vpPublish[PublishKind::Syphon].enabled = true;
        }
        e.renderFrame();
        CHECK(e.publishState(vp2, PublishKind::Syphon).level != PublishState::Off);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->vpPublish[PublishKind::Syphon].enabled = false;
        }
        e.renderFrame();
        CHECK(e.publishState(vp2, PublishKind::Syphon).level == PublishState::Off);
        // Left out of the second viewport: the first still shows it
        e.setOpacityIn(li, vp2, 0.0f);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        b = e.grabViewport(vp2);
        CHECK(a.pixelColor(16, 8).red() > 250 && b.pixelColor(16, 8).blue() < 5 && b.pixelColor(16, 8).red() < 5);
        CHECK(e.layerJson(li).value("viewports").toObject().size() == 1);
        // Half of it in the second viewport: the blue is halfway to black, the first viewport is unchanged
        e.setOpacityIn(li, vp2, 0.5f);
        for (int k = 0; k < 2; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        b = e.grabViewport(vp2);
        CHECK(std::abs(b.pixelColor(16, 8).blue() - 128) < 4 && a.pixelColor(16, 8).red() > 250);
        e.setOpacityIn(li, vp2, 1.0f);
        CHECK(e.layerJson(li).value("viewports").toObject().isEmpty());
        // A viewport has its own color and opacity
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->opacity = 0.5f;
        }
        for (int k = 0; k < 2; ++k) e.renderFrame();
        b = e.grabViewport(vp2);
        CHECK(std::abs(b.pixelColor(16, 8).blue() - 128) < 8);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(vp2))->opacity = 1.0f;
        }
        // Groups inside groups: the layer goes into an inner group, held by an outer group
        const quint64 lid = e.layerId(li);
        const quint64 outer = e.layerId(e.addGroup("Outer", e.layerCount()));
        const quint64 inner = e.layerId(e.addGroup("Inner", e.layerCount()));
        e.setStructure(tree::intoGroup(e.structure(), {inner}, outer));
        e.setStructure(tree::intoGroup(e.structure(), {lid}, inner));
        CHECK(e.layer(e.indexOfId(lid))->parent == inner && e.layer(e.indexOfId(inner))->parent == outer);
        CHECK(tree::depthOf(e.structure(), lid) == 2);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->opacity = 0.5f;
        }
        for (int k = 0; k < 3; ++k) e.renderFrame();
        a = e.grabViewport(vp1);
        CHECK(std::abs(a.pixelColor(16, 8).red() - 128) < 8); // through both groups, at the outer's opacity
        // Routing belongs to the top: a layer inside a group cannot be routed on its own
        e.setOpacityIn(e.indexOfId(lid), vp1, 0.0f);
        CHECK(e.layer(e.indexOfId(lid))->viewportOpacity.empty());
        // Removing a group: its contents go up one level only
        e.removeLayer(e.indexOfId(inner));
        CHECK(e.layer(e.indexOfId(lid))->parent == outer);
        // Snapshots leave the viewports alone, but recall the routing of the layers
        e.setOpacityIn(e.indexOfId(outer), vp2, 0.0f);
        Engine::Snapshot mem;
        mem.layers = e.captureLayers();
        bool hasViewport = false;
        for (const QJsonValue &v : mem.layers) hasViewport |= v.toObject().value("viewport").toBool();
        CHECK(!hasViewport);
        e.setOpacityIn(e.indexOfId(outer), vp2, 1.0f);
        e.applyLayers(mem.layers, 0);
        CHECK(!e.layer(e.indexOfId(outer))->shownIn(vp2));
        // Saved and read back: sizes, places, screens, routing
        e.setViewportOutput(e.indexOfId(vp2), QStringLiteral("HDMI-1"), 2);
        CHECK(e.saveProject(tmp + "/viewports.pinol", {}, &err));
        const QJsonObject saved = readJson(tmp + "/viewports.pinol");
        CHECK(e.loadProject(tmp + "/viewports.pinol", nullptr, &err));
        CHECK(e.viewports().size() == 2);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *v = e.layer(e.indexOfId(vp2));
            CHECK(v && v->isViewport && v->viewportSize() == QSize(32, 16) && v->vpScreen == "HDMI-1" && v->vpMode == 2);
            CHECK(std::abs(v->mapping.bounds().left() - 0.5) < 1e-9);
            CHECK(!e.layer(e.indexOfId(outer))->shownIn(vp2));
        }
        CHECK(e.saveProject(tmp + "/viewports2.pinol", {}, &err));
        CHECK(saved.value("layers") == readJson(tmp + "/viewports2.pinol").value("layers"));
        // A hidden outer group silences what is inside its inner groups too
        {
            const quint64 in2 = e.layerId(e.addGroup("Inner 2", e.layerCount()));
            e.setStructure(tree::intoGroup(e.structure(), {in2}, outer));
            e.setStructure(tree::intoGroup(e.structure(), {lid}, in2));
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->enabled = false;
        }
        e.renderFrame();
        CHECK(!e.layer(e.indexOfId(lid))->parentEnabled);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(outer))->enabled = true;
        }
        e.renderFrame();
        CHECK(e.layer(e.indexOfId(lid))->parentEnabled);
        // Undoing the deletion of the first viewport puts it back first
        {
            const QJsonObject first = e.layerJson(0);
            e.removeLayer(0);
            CHECK(e.layerId(0) == vp2);
            e.insertLayerJson(0, first);
            CHECK(e.layerId(0) == vp1 && e.layerId(1) == vp2);
        }
        // A viewport can go as long as another one stays
        e.removeLayer(e.indexOfId(vp2));
        CHECK(e.viewports().size() == 1);
        e.removeLayer(e.viewports().first());
        CHECK(e.viewports().size() == 1);
        // A new project shows its composition through one viewport
        e.newProject();
        CHECK(e.viewports().size() == 1 && e.layerCount() == 1);
    }

    // 4g. Antialiasing: multisampled edges, mipmaps for pictures drawn smaller; the project's choice or the default
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setBlackout(false, 0);
        e.setCompositionSize(QSize(64, 64));
        e.setViewportSize(e.viewports().first(), QSize(64, 64));
        QImage white(64, 64, QImage::Format_RGB32), checker(64, 64, QImage::Format_RGB32);
        white.fill(Qt::white);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) checker.setPixel(x, y, (x + y) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        white.save(tmp + "/aawhite.png");
        checker.save(tmp + "/aachecker.png");
        const int li = e.addLayer("Edge");
        CHECK(e.setLayerImage(li, tmp + "/aawhite.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(li)->mapping; // a slanted edge
            m.setCorner(0, QPointF(0.1, 0.0)), m.setCorner(1, QPointF(0.6, 0.0)), m.setCorner(2, QPointF(0.9, 1.0)),
                m.setCorner(3, QPointF(0.1, 1.0));
        }
        auto greys = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            int n = 0;
            for (int y = 0; y < g.height(); ++y)
                for (int x = 0; x < g.width(); ++x) {
                    const int v = qGray(g.pixel(x, y));
                    n += v > 25 && v < 230;
                }
            return n;
        };
        Engine::RenderSettings rs;
        rs.samples = 0;
        e.setRenderSettings(rs);
        const int hard = greys();
        rs.samples = 4;
        e.setRenderSettings(rs);
        const int soft = greys();
        std::printf("       edge pixels between black and white: %d without antialiasing, %d with 4x\n", hard, soft);
        CHECK(soft > hard + 20);
        // The project says "default": the machine's default applies
        rs.samples = -1;
        e.setRenderSettings(rs);
        e.setRenderDefaults({0, 4, 0});
        CHECK(e.effectiveRender().samples == 4 && greys() > hard + 20);
        e.setRenderDefaults({0, 0, 0});
        // Mipmaps: a fine checker drawn at an eighth of its size averages to grey instead of aliasing
        CHECK(e.setLayerImage(li, tmp + "/aachecker.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            Mapping &m = e.layer(li)->mapping;
            m.setCorner(0, QPointF(0, 0)), m.setCorner(1, QPointF(0.125, 0)), m.setCorner(2, QPointF(0.125, 0.125)),
                m.setCorner(3, QPointF(0, 0.125));
        }
        auto spread = [&] {
            for (int k = 0; k < 2; ++k) e.renderFrame();
            const QImage g = e.grabViewport(e.mainViewportId());
            int lo = 255, hi = 0;
            for (int y = 1; y < 7; ++y)
                for (int x = 1; x < 7; ++x) {
                    lo = std::min(lo, qGray(g.pixel(x, y)));
                    hi = std::max(hi, qGray(g.pixel(x, y)));
                }
            return std::pair<int, int>{lo, hi};
        };
        rs = Engine::RenderSettings{-1, 0, 1};
        e.setRenderSettings(rs);
        const auto [lo, hi] = spread();
        CHECK(lo > 90 && hi < 165); // grey everywhere
        // Made once per frame of the render, however many viewports draw the layer (audit P2; before: each one)
        {
            const int v2 = e.addViewport(QStringLiteral("Twin"), QSize(64, 64));
            {
                Engine::Lock lk(&e.mutex());
                e.layer(v2)->mapping = e.layer(e.viewports().first() == v2 ? e.viewports()[1] : e.viewports().first())->mapping;
            }
            e.renderFrame();
            const quint64 before = e.mipmapsMade();
            for (int k = 0; k < 3; ++k) e.renderFrame();
            std::printf("       mipmaps made in 3 frames with 2 viewports: %llu\n", (unsigned long long)(e.mipmapsMade() - before));
            CHECK(e.mipmapsMade() - before == 3);
            CHECK(spread().first > 90 && spread().second < 165); // still grey in the first viewport
            e.removeLayer(e.indexOfId(e.layerId(v2)));
        }
        // The frame rate is the project's (composition/fps); samples, mipmaps and depth are the machine's
        rs = Engine::RenderSettings{30, 8, 1};
        e.setRenderSettings(rs);
        CHECK(e.saveProject(tmp + "/render.pinol", {}, &err));
        e.newProject();
        CHECK(e.renderSettings().frameRate < 0 && e.renderSettings().samples < 0); // a new project: the defaults
        CHECK(e.loadProject(tmp + "/render.pinol", nullptr, &err));
        CHECK(e.renderSettings().frameRate == 30 && e.renderSettings().samples < 0 && e.renderSettings().mipmaps < 0);
        e.setRenderSettings({});
    }
    } // rendering

    // 5. Render thread
    auto waitFrames = [&](quint64 n) {
        const quint64 target = e.frameCount() + n;
        QElapsedTimer t;
        t.start();
        while (e.frameCount() < target && t.elapsed() < 10000) QThread::msleep(5);
        return e.frameCount() >= target;
    };
    // macOS: Qt renders OpenGL on the main thread only (AppKit), so the engine renders on the UI thread there and
    // what needs the render thread is left to the other platforms
    if (!QOpenGLContext::supportsThreadedOpenGL() && !Engine::metalOutput()) {
        std::printf("SKIP render thread, sound and threaded video: no OpenGL on another thread on this platform\n");
    } else {
    e.newProject();
    // The render thread cannot make its OpenGL context current: what waits on it returns at once (nothing hangs), and
    // a new start works
    {
        Engine::failNextRenderStartForTests();
        CHECK(e.start());
        QElapsedTimer t;
        t.start();
        while (!e.renderFailed() && t.elapsed() < 5000) QThread::msleep(5);
        bool ran = false;
        e.runGl([&] { ran = true; }, true);
        CHECK(e.renderFailed() && !ran);
        e.stop();
        CHECK(!e.isThreaded());
    }
    CHECK(e.start());
    CHECK(e.isThreaded() && !e.renderFailed());
    CHECK(waitFrames(5));
    if (area("thread")) {
    { // A window gets the viewport's picture from the render thread: through Metal on macOS, OpenGL elsewhere
        QWindow w;
        w.setSurfaceType(Engine::metalOutput() ? QSurface::MetalSurface : QSurface::OpenGLSurface);
        if (w.surfaceType() == QSurface::OpenGLSurface) w.setFormat(QSurfaceFormat::defaultFormat());
        w.setGeometry(100, 100, 160, 90);
        w.create();
        w.show();
        QElapsedTimer t;
        t.start();
        while (!w.isExposed() && t.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const quint64 vp = e.mainViewportId();
        e.setViewportWindow(vp, &w);
        e.setViewportExposed(vp, true, w.size() * w.devicePixelRatio(), 50.0); // its screen: 50 Hz
        CHECK(waitFrames(10));
        std::printf("       window frames: %llu (%s)\n", (unsigned long long)e.windowFrames(vp),
                    Engine::metalOutput() ? "Metal" : "OpenGL");
        CHECK(e.windowFrames(vp) >= 5);
        // The output in the frame timing (sampled once a second): its screen's rate is the render's pace, its pictures
        // shown and those not shown by cause
        QThread::msleep(1200);
        {
            const QJsonObject t = e.frameTimingJson();
            const QJsonArray outs = t.value("outputs").toArray();
            QJsonObject mine;
            for (const QJsonValue &v : outs)
                if (v.toObject().value("shown").toBool()) mine = v.toObject();
            std::printf("       output: %s\n", QJsonDocument(mine).toJson(QJsonDocument::Compact).constData());
            CHECK(!mine.isEmpty() && mine.value("screen_hz").toDouble() == 50.0 && mine.value("presented").toDouble() >= 1 &&
                  mine.contains("no_drawable") && mine.contains("not_render_thread"));
            CHECK(t.value("pace_hz").toDouble() == 50.0 && t.value("main_screen_hz").toDouble() > 0);
        }
        e.setViewportWindow(vp, nullptr);
    }
    {
        // The main thread "blocks": output keeps being produced.
        const quint64 before = e.frameCount();
        QThread::msleep(500);
        std::printf("       %llu frames during 500 ms of main-thread blocking\n",
                    static_cast<unsigned long long>(e.frameCount() - before));
        CHECK(e.frameCount() - before >= 2); // still rendering (the rate depends on the machine: a software GPU gives ~8)
    }
    // The frame rate chosen paces the render thread
    {
        e.setRenderSettings({4, -1, -1});
        QThread::msleep(300);
        const quint64 before = e.frameCount();
        QThread::msleep(1000);
        const quint64 n = e.frameCount() - before;
        std::printf("       %llu frames in 1 s at 4 fps\n", static_cast<unsigned long long>(n));
        CHECK(n >= 2 && n <= 6);
        e.setRenderSettings({});
    }
    // The image changes over time (animated generator) in threaded mode
    {
        int pl = e.addLayer("anim");
        e.setLayerIsf(pl, root + "/../isf/generators/Plasma.fs", &err);
        CHECK(waitFrames(5));
        const QImage a = e.grabOutput();
        QThread::msleep(400);
        CHECK(waitFrames(5));
        const QImage b = e.grabOutput();
        std::printf("       identical frames 400 ms apart: %s\n", a == b ? "yes" : "no");
        CHECK(a != b);
        e.removeLayer(pl);
    }
    // Concurrent changes during rendering (small composition: the software GPU used for tests is slow)
    {
        e.setCompositionSize(QSize(320, 180));
        QElapsedTimer t;
        t.start();
        int ops = 0;
        while (t.elapsed() < 3000) {
            int a = e.addLayer("stress");
            e.setLayerIsf(a, root + "/../isf/generators/Plasma.fs", &err);
            e.addEffect(a, root + "/../isf/effects/Blur.fs", &err);
            {
                Engine::Lock lk(&e.mutex());
                Layer *l = e.layer(a);
                l->mapping.setCorner(1, QPointF(0.7, 0.1));
                l->mapping.resetMesh(6, 5);
                l->effects[0]->inputs()[1].fValue = 12;
                l->opacity = 0.8f;
            }
            int b = e.insertLayerJson(V + 0, e.layerJson(a + 1 > e.layerCount() - 1 ? a : a));
            e.setEffectsJson(b, QJsonArray());
            if (e.layerCount() > 6) {
                e.removeLayer(e.layerCount() - 1);
                e.removeLayer(V + 0);
            }
            e.fadeCompositionOpacity(ops % 2 ? 1.0 : 0.5, 0.1);
            ++ops;
        }
        std::printf("       %d rounds of concurrent changes\n", ops);
        CHECK(ops >= 1); // a software GPU (CI) is slow: a round is enough to cross the render thread
        CHECK(waitFrames(5));
    }
    // Video in threaded mode
    {
        e.newProject();
        e.setCompositionSize(QSize(640, 360));
        int v2 = e.addLayer("v");
        CHECK(e.setLayerVideo(v2, root + "/media/h264.mp4", &err));
        CHECK(waitFrames(20));
        Engine::Lock lk(&e.mutex());
        CHECK(e.layer(v2)->videoTex && e.layer(v2)->videoTex->width() == 1280);
    }
    // Master: full blackout
    {
        e.newProject();
        int c = e.addLayer("white");
        e.setLayerIsf(c, root + "/../isf/generators/SolidColor.fs", &err);
        e.fadeCompositionOpacity(1.0, 0);
        CHECK(waitFrames(4));
        QImage lit = e.grabOutput();
        CHECK(!lit.isNull() && qGray(lit.pixel(lit.width() / 2, lit.height() / 2)) > 240);
        e.fadeCompositionOpacity(0.0, 0);
        CHECK(waitFrames(4));
        QImage dark = e.grabOutput();
        CHECK(!dark.isNull() && qGray(dark.pixel(dark.width() / 2, dark.height() / 2)) < 2);
        e.fadeCompositionOpacity(1.0, 1.0); // fade back up in 1 s
        QThread::msleep(500);
        const double mid = e.compositionOpacity();
        std::printf("       composition opacity at mid-fade: %.2f\n", mid);
        CHECK(mid > 0.1 && mid < 0.9); // on its way (frames of a software GPU come late)
    }
    // Color switches: a parameter that is off keeps its value but is not applied
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        const int w = e.addLayer("white");
        e.setLayerIsf(w, root + "/../isf/generators/SolidColor.fs", &err);
        CHECK(waitFrames(4));
        auto center = [&] {
            const QImage im = e.grabOutput();
            return im.isNull() ? QColor() : QColor(im.pixel(im.width() / 2, im.height() / 2));
        };
        CHECK(center().green() > 240);
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(w);
            l->color.remove[1] = 1.0f; // green removed
            l->color.removeOn = false;
        }
        CHECK(waitFrames(4));
        CHECK(center().green() > 240); // switched off: the picture is left alone
        {
            Engine::Lock lk(&e.mutex());
            e.layer(w)->color.removeOn = true;
        }
        CHECK(waitFrames(4));
        CHECK(center().green() < 40 && center().red() > 240);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(w)->color.enabled = false; // the whole section
        }
        CHECK(waitFrames(4));
        CHECK(center().green() > 240);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(std::abs(e.layer(w)->color.remove[1] - 1.0f) < 1e-6); // the value is kept
        }
        // Switches saved with the project (a project without them has everything on)
        CHECK(e.saveProject(tmp + "/colorswitch.pinol", {}, &err));
        CHECK(e.loadProject(tmp + "/colorswitch.pinol", nullptr, &err));
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(V + 0);
            CHECK(l && !l->color.enabled && l->color.removeOn && std::abs(l->color.remove[1] - 1.0f) < 1e-6);
        }
        QJsonObject legacy = e.layerJson(V + 0);
        QJsonObject params = legacy.value("params").toObject();
        for (const QString &k : {QStringLiteral("color/enable"), QStringLiteral("remove/enable")}) params.remove(k);
        legacy["params"] = params;
        e.replaceLayerJson(V + 0, legacy);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(V + 0)->color.enabled && e.layer(V + 0)->color.removeOn);
        }
    }
    } // thread
    if (area("publishing")) {
    // 6. Publishing: GPU readback -> send thread (BGRA, rows top to bottom)
    {
        e.newProject();
        e.fadeCompositionOpacity(1.0, 0);
        e.setCompositionSize(QSize(64, 32));
        e.setViewportSize(e.viewports().first(), QSize(64, 32)); // what is published is the viewport's picture
        QImage img(64, 32, QImage::Format_RGBA8888);
        img.fill(QColor(255, 0, 0));
        for (int y = 16; y < 32; ++y)
            for (int x = 0; x < 64; ++x) img.setPixelColor(x, y, QColor(0, 0, 255));
        img.save(tmp + "/topred.png");
        int li = e.addLayer("img");
        CHECK(e.setLayerImage(li, tmp + "/topred.png", &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(li)->mapping.resetCorners();
        }
        // Shared with the send thread, which may still deliver a frame after the tap is removed
        struct TapState {
            std::mutex m;
            CpuFrame last;
            int frames = 0;
        };
        auto tap = std::make_shared<TapState>();
        e.setTestTap([tap](const CpuFrame &f) {
            std::lock_guard<std::mutex> lk(tap->m);
            tap->last = f;
            ++tap->frames;
        });
        CHECK(waitFrames(20));
        // The send thread runs on its own: on a loaded machine it may come later than the frames rendered
        QElapsedTimer sent;
        sent.start();
        while (sent.elapsed() < 5000) {
            {
                std::lock_guard<std::mutex> lk(tap->m);
                if (tap->frames >= 10) break;
            }
            waitFrames(1);
        }
        std::lock_guard<std::mutex> lk(tap->m);
        const CpuFrame &last = tap->last;
        const int frames = tap->frames;
        std::printf("       %d frames received by the send thread\n", frames);
        CHECK(frames >= 10);
        CHECK(last.width == 64 && last.height == 32 && last.stride == 256);
        if (last.bgra.size() < size_t(last.stride) * 32) return 1; // nothing to read the pixels of
        auto px = [&](int x, int y) { const uint8_t *p = last.bgra.data() + y * last.stride + x * 4; return QColor(p[2], p[1], p[0]); };
        std::printf("       top pixel %s, bottom %s\n", qPrintable(px(10, 2).name()), qPrintable(px(10, 29).name()));
        CHECK(px(10, 2) == QColor(255, 0, 0));  // top: red
        CHECK(px(10, 29) == QColor(0, 0, 255)); // bottom: blue
        CHECK(last.timestamp100ns > 0);
    }
    e.setTestTap(nullptr);
    {
        // Settings: disabled by default, Syphon/Spout reported according to platform, JSON round trip
        PublishSettings ps;
        ps[PublishKind::Ndi].enabled = true;
        ps[PublishKind::Ndi].name = "Stage — Main";
        ps[PublishKind::Syphon].enabled = true;
        ps.omtQuality = 50;
        CHECK(PublishSettings::fromJson(ps.toJson()) == ps);
        const quint64 vp = e.mainViewportId(); // each viewport publishes its own picture
        e.setPublishSettings(vp, ps);
        CHECK(waitFrames(3));
        const PublishState syphon = e.publishState(vp, PublishKind::Syphon);
        std::printf("       Syphon: %s\n       NDI: %s\n", qPrintable(syphon.text), qPrintable(e.publishState(vp, PublishKind::Ndi).text));
        CHECK(publishCompiledIn(PublishKind::Syphon) ? syphon.level != PublishState::Unavailable
                                                     : syphon.level == PublishState::Unavailable);
        CHECK(e.publishState(vp, PublishKind::Omt).level == PublishState::Off);
        e.setPublishSettings(vp, PublishSettings());
        CHECK(waitFrames(3));
        CHECK(e.publishState(vp, PublishKind::Ndi).level == PublishState::Off);
    }
    {
        // Syphon / Spout one frame later (the default): the picture shared at the end of frame f is the previous
        // frame's, and Syphon copies it in a context of its own that the render thread cannot wait for. So the frame
        // after must not draw into it: the texture shared at frame f is never the one shared at frame f + 2 (the
        // picture drawn at f + 1). With two buffers it would be. Recorded by a stand-in publisher, on every platform.
        struct Shares {
            std::mutex m;
            std::map<quint64, unsigned> byFrame; // frame -> texture shared at its end
        };
        class RecordingPublisher : public GpuPublisher
        {
        public:
            RecordingPublisher(Engine *e, std::shared_ptr<Shares> s) : m_e(e), m_s(std::move(s)) {}
            bool start(const QString &, QString *) override { return true; }
            void publish(unsigned int texture, int, int) override
            {
                std::lock_guard<std::mutex> lk(m_s->m);
                m_s->byFrame[m_e->frameCount()] = texture; // render thread: the frame being finished
            }

        private:
            Engine *m_e;
            std::shared_ptr<Shares> m_s;
        };
        auto shares = std::make_shared<Shares>();
        Engine *engine = &e;
        e.setGpuPublisherFactoryForTests([engine, shares](PublishKind) -> std::unique_ptr<GpuPublisher> {
            return std::make_unique<RecordingPublisher>(engine, shares);
        });
        const bool wasLate = e.gpuShareOneFrameLate();
        const quint64 vp = e.mainViewportId();
        for (const bool delayed : {true, false}) {
            e.setGpuShareOneFrameLate(delayed);
            {
                std::lock_guard<std::mutex> lk(shares->m);
                shares->byFrame.clear();
            }
            PublishSettings ps;
            ps[PublishKind::Syphon].enabled = true;
            ps[PublishKind::Syphon].name = "Share order test";
            e.setPublishSettings(vp, ps);
            CHECK(waitFrames(16));
            CHECK(e.publishState(vp, PublishKind::Syphon).level == PublishState::Ok);
            e.setPublishSettings(vp, PublishSettings());
            CHECK(waitFrames(2));
            std::map<quint64, unsigned> got;
            {
                std::lock_guard<std::mutex> lk(shares->m);
                got = shares->byFrame;
            }
            int pairs = 0, overwritten = 0;
            for (const auto &[frame, tex] : got) {
                const auto later = got.find(frame + 2);
                if (later == got.end()) continue;
                ++pairs;
                if (later->second == tex) ++overwritten;
            }
            std::printf("       GPU share %s: %d frames shared, %d pairs checked, %d overwritten\n",
                        delayed ? "one frame later" : "at once", int(got.size()), pairs, overwritten);
            CHECK(got.size() >= 8);
            if (delayed) CHECK(pairs >= 4 && overwritten == 0);
            for (const auto &[frame, tex] : got) CHECK(tex != 0);
        }
        e.setGpuShareOneFrameLate(wasLate);
        e.setGpuPublisherFactoryForTests(nullptr);
    }
    {
        // DeckLink: its settings (the first port by default), the modes, the picture in 4:2:2, a port that is not there
        PublishSettings ps;
        CHECK(ps[PublishKind::DeckLink].name.isEmpty() && !ps[PublishKind::DeckLink].enabled);
        ps[PublishKind::DeckLink].enabled = true;
        ps.decklinkMode = "Hp25";
        CHECK(PublishSettings::fromJson(ps.toJson()) == ps && PublishSettings::fromJson(QJsonObject()) == PublishSettings());
        CHECK(publishKindName(PublishKind::DeckLink) == "DeckLink");
        std::vector<decklink::Mode> modes = {{"Hp25", "1080p25", 1920, 1080, 25, false},
                                             {"Hi50", "1080i50", 1920, 1080, 25, true},
                                             {"Hp30", "1080p30", 1920, 1080, 30, false},
                                             {"Hp29", "1080p29.97", 1920, 1080, 30000.0 / 1001, false},
                                             {"hp50", "720p50", 1280, 720, 50, false},
                                             {"4k25", "2160p25", 3840, 2160, 25, false}};
        CHECK(decklink::bestMode(modes, QSize(1920, 1080), 25)->key == "Hp25");
        CHECK(decklink::bestMode(modes, QSize(1920, 1080), 60)->key == "Hp30"); // the closest rate, a whole one first
        CHECK(decklink::bestMode(modes, QSize(1280, 720), 60)->key == "hp50");
        CHECK(decklink::bestMode(modes, QSize(1000, 500), 25)->key == "Hp25"); // no such size: 1080
        // The driver is asked for its ports once, not at every look (the interface looks every two seconds): a
        // rescan asks again. Without the SDK or a driver, nothing is asked at all
        {
            const int before = decklink::lookups();
            for (int k = 0; k < 20; ++k) {
                decklink::outputPorts();
                decklink::inputPorts();
                decklink::outputModes(QStringLiteral("no such port"));
            }
            const int asked = decklink::lookups() - before;
            CHECK(asked <= 1);
            decklink::rescan();
            decklink::outputPorts();
            decklink::outputPorts();
            CHECK(decklink::lookups() - before <= asked + 1);
            CHECK(!decklink::compiledIn() || decklink::lookups() - before == asked + 1);
        }
        CHECK(decklink::bestMode({}, QSize(1920, 1080), 25) == nullptr);
        // White, black and red (BT.709, video range): Cb Y Cr Y
        auto uyvy = [](QColor c, int h) {
            std::vector<uint8_t> in(2 * 4), out(4);
            for (int x = 0; x < 2; ++x) {
                in[size_t(x * 4)] = uint8_t(c.blue());
                in[size_t(x * 4 + 1)] = uint8_t(c.green());
                in[size_t(x * 4 + 2)] = uint8_t(c.red());
                in[size_t(x * 4 + 3)] = 255;
            }
            decklink::bgraToUyvy(in.data(), 8, 2, 1, out.data(), 4);
            (void)h;
            return out;
        };
        auto near = [](const std::vector<uint8_t> &v, std::initializer_list<int> want) {
            size_t i = 0;
            for (int w : want)
                if (std::abs(int(v[i++]) - w) > 1) return false;
            return true;
        };
        CHECK(near(uyvy(Qt::white, 1080), {128, 235, 128, 235}));
        CHECK(near(uyvy(Qt::black, 1080), {128, 16, 128, 16}));
        // (the conversion takes the matrix from the picture's height: one line here is SD, BT.601)
        CHECK(near(uyvy(QColor(255, 0, 0), 1), {90, 82, 240, 82}));
        // No such port (no card, or no driver on the test machine): an error, not a crash
        const quint64 vp = e.mainViewportId();
        ps[PublishKind::DeckLink].name = "p-not-a-port";
        e.setPublishSettings(vp, ps);
        CHECK(waitFrames(3));
        const PublishState st = e.publishState(vp, PublishKind::DeckLink);
        std::printf("       DeckLink: %s (driver %s)\n", qPrintable(st.text),
                    qPrintable(decklink::driverVersion().isEmpty() ? QStringLiteral("none") : decklink::driverVersion()));
        CHECK(decklink::compiledIn() ? st.level == PublishState::Error : st.level == PublishState::Unavailable);
        e.setPublishSettings(vp, PublishSettings());
        CHECK(waitFrames(3));
        CHECK(e.publishState(vp, PublishKind::DeckLink).level == PublishState::Off);
    }
    } // publishing

    if (area("media")) {
    // 7. Media: usage, missing file kept, relink, media bin
    {
        e.newProject();
        QFile::copy(root + "/media/h264.mp4", tmp + "/clip.mp4");
        int v = e.addLayer("Clip");
        CHECK(e.setLayerVideo(v, tmp + "/clip.mp4", &err));
        int g = e.addLayer("Mask");
        e.setLayerIsf(g, root + "/../isf/generators/TestPattern.fs", &err);
        int fx = e.addEffect(g, root + "/../isf/effects/Mask.fs", &err);
        CHECK(fx == 0);
        {
            IsfInstance *inst;
            {
                Engine::Lock lk(&e.mutex());
                inst = e.layer(g)->effects[0].get();
            }
            CHECK(e.setIsfImageInput(inst, 1, tmp + "/topred.png", &err));
        }
        e.addBinItems({root + "/media/bars.png", tmp + "/clip.mp4"});
        auto usage = e.mediaUsage();
        CHECK(usage.size() == 3);
        auto find = [&](const QString &p) -> const Engine::MediaRef * {
            for (const auto &r : usage) if (r.path == p) return &r;
            return nullptr;
        };
        const Engine::MediaRef *clip = find(tmp + "/clip.mp4");
        CHECK(clip && clip->video && clip->users == QStringList{"Clip"} && clip->imported && !clip->missing);
        const Engine::MediaRef *mask = find(tmp + "/topred.png");
        CHECK(mask && !mask->video && mask->users.size() == 1 && mask->users[0].contains("Mask"));
        CHECK(e.layersUsingMedia(tmp + "/topred.png") == QList<int>{g});

        // Save, delete the video, reopen: the path is kept and reported
        CHECK(e.saveProject(tmp + "/media.pinol", {}, &err));
        e.newProject(); // the video closed (Windows does not move a file that is open)
        CHECK(QFile::rename(tmp + "/clip.mp4", tmp + "/clip_moved.mp4"));
        QString warn;
        CHECK(e.loadProject(tmp + "/media.pinol", nullptr, &warn));
        std::printf("       warning: %s\n", qPrintable(warn));
        CHECK(warn.contains("not found"));
        int vi = -1;
        for (int i = 0; i < e.layerCount(); ++i) if (e.layer(i)->name == "Clip") vi = i;
        CHECK(vi >= 0);
        usage = e.mediaUsage();
        clip = find(tmp + "/clip.mp4");
        CHECK(clip && clip->missing && clip->video);
        CHECK(e.layerJson(vi).value("source").toObject().value("type").toString() == "video");
        CHECK(paramOf(e.layerJson(vi), "play_mode").isString()); // its transport's values are kept for when it comes back
        CHECK(e.binItems().size() == 2);
        // Relink: the video comes back, the mapping is kept
        Mapping before = e.layer(vi)->mapping;
        CHECK(e.relinkLayerMedia(vi, tmp + "/clip.mp4", tmp + "/clip_moved.mp4", &err));
        e.relinkBinItem(tmp + "/clip.mp4", tmp + "/clip_moved.mp4");
        CHECK(e.layer(vi)->type == SourceType::Video && e.layer(vi)->error.isEmpty());
        CHECK(e.layer(vi)->mapping == before);
        CHECK(paramOf(e.layerJson(vi), "play_mode").isString() && e.layer(vi)->extraParams.isEmpty());
        e.layer(vi)->extraParams = QJsonObject{{"speed", -1.0}}; // kept from a missing file: not for another media
        CHECK(e.setLayerImage(vi, root + "/media/bars.png", &err) && e.layer(vi)->extraParams.isEmpty());
        CHECK(e.binItems().contains(tmp + "/clip_moved.mp4"));
        QFile::remove(tmp + "/clip_moved.mp4");
        VideoDecoder::Info info;
        CHECK(VideoDecoder::probe(root + "/media/prores.mov", &info) && info.width == 1920 && info.codec == "prores");
    }
    } // media

    if (area("sound")) {
    // --- Sound: audio layer, sound of a video layer, synchronized with the playhead. Not with Apple's software
    // renderer (CI's macOS): one core for everything, its timings mean nothing there
    if (e.glRenderer().contains(QLatin1String("Apple Software Renderer"))) {
        std::printf("SKIP sound timings: Apple Software Renderer\n");
    } else {
        CHECK(e.startAudio(QString(), &err, true)); // null device: no sound card, but the callback runs in real time
        std::mutex capMutex;
        std::vector<float> cap; // last second of mixed output (stereo)
        std::atomic<long long> tapFrames{0};
        QElapsedTimer tapClock;
        tapClock.start();
        e.audioOutput().setTap([&](const float *s, int n) {
            tapFrames += n;
            std::lock_guard<std::mutex> lk(capMutex);
            cap.insert(cap.end(), s, s + size_t(n) * 2);
            const size_t keep = 48000 * 2;
            if (cap.size() > keep) cap.erase(cap.begin(), cap.begin() + long(cap.size() - keep));
        });
        auto last = [&](double seconds) { // left channel of the last `seconds`
            std::lock_guard<std::mutex> lk(capMutex);
            const size_t n = std::min(cap.size() / 2, size_t(seconds * 48000));
            std::vector<float> l(n);
            for (size_t i = 0; i < n; ++i) l[i] = cap[cap.size() - 2 * n + 2 * i];
            return l;
        };
        auto rms = [](const std::vector<float> &v) {
            double a = 0;
            for (float x : v) a += double(x) * x;
            return v.empty() ? 0.0 : std::sqrt(a / double(v.size()));
        };
        auto freq = [](const std::vector<float> &v) { // zero crossings
            int z = 0;
            for (size_t i = 1; i < v.size(); ++i) z += (v[i - 1] < 0) != (v[i] < 0);
            return v.empty() ? 0.0 : z / 2.0 / (double(v.size()) / 48000);
        };
        auto settle = [](int ms) { QThread::msleep(ms); };
        // Waits (up to 2 s) for a condition on the output: robust on slow machines where frames are late
        auto waitFor = [&](const std::function<bool()> &ok) {
            QElapsedTimer t;
            t.start();
            while (!ok() && t.elapsed() < 2000) QThread::msleep(50);
            return ok();
        };
        auto silent = [&] { return waitFor([&] { return rms(last(0.1)) < 1e-4; }); };
        auto tone = [&](double hz, double tol) {
            return waitFor([&] { return std::abs(freq(last(0.15)) - hz) < tol && rms(last(0.15)) > 0.05; });
        };

        const int a = e.addLayer("Tone");
        CHECK(e.setLayerAudio(a, root + "/media/tone.wav", &err));
        CHECK(e.layer(a)->type == SourceType::Audio && e.layer(a)->audio && std::abs(e.layer(a)->duration() - 4.0) < 0.05);
        CHECK(tone(440, 15));
        settle(400); // let the stream settle before measuring the synchronization
        double worst = 0;
        for (int k = 0; k < 20; ++k) {
            worst = std::max(worst, std::abs(e.layer(a)->audio->syncError()));
            QThread::msleep(50);
        }
        // The test sound card is clocked in software: on an overloaded machine (CI) it runs late, as a real card
        // would during dropouts. A real card is clocked by its quartz (within a few ppm).
        const double cardSpeed = double(tapFrames) / 48000.0 / (tapClock.nsecsElapsed() / 1e9); // since the start
        const bool accurateCard = std::abs(cardSpeed - 1.0) < 0.005;
        std::printf("      worst sync error over 1 s: %.1f ms (test sound card speed %.4f)\n", worst * 1000, cardSpeed);
        // Accurate clock: within 12 ms. Late clock: below the realignment threshold (40 ms), far under the
        // lip-sync detectability threshold (about 45 ms ahead / 125 ms behind, ITU-R BT.1359).
        CHECK(worst < (accurateCard ? 0.012 : 0.045));

        e.seekLayer(a, 3.0); // the sound follows the playhead
        CHECK(tone(880, 20));
        e.seekLayer(a, 0.5);
        CHECK(tone(440, 15));

        e.setLayerPlaying(a, false); // pause: silence, the position stays put
        CHECK(silent());
        double paused;
        {
            Engine::Lock lk(&e.mutex());
            paused = e.layer(a)->position();
        }
        settle(200);
        {
            Engine::Lock lk(&e.mutex());
            CHECK(std::abs(e.layer(a)->position() - paused) < 1e-9);
        }
        e.setLayerPlaying(a, true);
        waitFor([&] { return rms(last(0.2)) > 0.2; });
        settle(100);
        const double full = rms(last(0.2));
        CHECK(full > 0.2);

        e.setLayerVolume(a, 0.5f);
        waitFor([&] { return std::abs(rms(last(0.2)) / full - 0.5) < 0.05; });
        const double half = rms(last(0.2));
        std::printf("      volume 0.5: rms ratio %.3f\n", half / full);
        CHECK(std::abs(half / full - 0.5) < 0.05);
        e.setLayerVolume(a, 1.0f);
        e.setLayerMuted(a, true);
        CHECK(silent());
        e.setLayerMuted(a, false);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->enabled = false; // layer off = no sound either
        }
        CHECK(silent());
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->enabled = true;
        }
        e.setAudioVolume(0.0f); // master
        CHECK(silent());
        e.setAudioVolume(1.0f);
        CHECK(waitFor([&] { return rms(last(0.2)) > 0.2; }));

        // Loop: across the end of the file, no gap and no resync
        e.seekLayer(a, 3.6);
        settle(250);
        const int resyncs = e.layer(a)->audio->resyncCount();
        settle(500); // wraps around at 4 s
        std::vector<float> wrapped = last(0.5);
        double minRms = 1;
        for (size_t k = 0; k + 2400 <= wrapped.size(); k += 2400)
            minRms = std::min(minRms, rms(std::vector<float>(wrapped.begin() + long(k), wrapped.begin() + long(k + 2400))));
        CHECK(minRms > 0.2);
        CHECK(e.layer(a)->audio->resyncCount() == resyncs);
        CHECK(std::abs(freq(last(0.1)) - 440) < 20);

        // Speed: tape-style, the pitch follows
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->speed = 1.5;
        }
        e.seekLayer(a, 0.2);
        CHECK(tone(660, 25));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(a)->speed = 1.0;
        }

        // Save / load
        const QJsonObject j = e.layerJson(a);
        CHECK(j.value("source").toObject().value("type").toString() == "audio");
        CHECK(paramOf(j, "volume").isDouble() && paramOf(j, "mute").isBool());

        // Video with a sound track
        e.removeLayer(a);
        CHECK(silent());
        const int v = e.addLayer("AV");
        CHECK(e.setLayerVideo(v, root + "/media/av.mp4", &err));
        CHECK(e.layer(v)->video && e.layer(v)->audio);
        CHECK(tone(660, 20));
        settle(500);
        CHECK(std::abs(e.layer(v)->audio->syncError()) < 0.045);
        const int nv = e.addLayer("Silent video");
        CHECK(e.setLayerVideo(nv, root + "/media/h264.mp4", &err));
        CHECK(!e.layer(nv)->audio); // no audio track: no stream
        e.removeLayer(nv);

        // Media bin
        e.addBinItems({root + "/media/tone.wav"});
        bool sawAudio = false;
        for (const auto &r : e.mediaUsage()) sawAudio |= r.path.endsWith("tone.wav") && r.audio && !r.video;
        CHECK(sawAudio);
        e.removeBinItem(root + "/media/tone.wav");
        AudioStream::Info ai;
        CHECK(AudioStream::probe(root + "/media/tone.wav", &ai) && ai.sampleRate == 44100 && ai.channels == 1);
        CHECK(!AudioStream::probe(root + "/media/h264.mp4", &ai));

        // The audio callback takes no lock: streams added and removed from another thread while it mixes, and
        // streams seeking all the time, never stop it (it keeps its pace) and nothing is used after removal
        {
            AudioOutput out;
            CHECK(out.start(QString(), &err, true));
            std::atomic<long long> mixed{0};
            out.setTap([&](const float *, int n) { mixed += n; });
            std::vector<std::shared_ptr<AudioStream>> streams;
            for (int k = 0; k < 4; ++k) {
                auto st = std::make_shared<AudioStream>();
                CHECK(st->open(root + "/media/tone.wav", AudioOutput::kSampleRate, &err));
                streams.push_back(st);
            }
            QElapsedTimer t;
            t.start();
            uint64_t id = 1;
            int cycles = 0;
            while (t.elapsed() < 1500) {
                for (auto &st : streams) {
                    // A new timeline id each time: the callback asks its stream to seek
                    st->setTransport(double(cycles % 7) * 0.3, true, 1.0, Timeline{}, ++id, 1.0f);
                    out.addStream(st);
                }
                QThread::msleep(3);
                for (auto &st : streams) out.removeStream(st.get());
                ++cycles;
            }
            const double expected = t.elapsed() / 1000.0 * AudioOutput::kSampleRate;
            CHECK(cycles > 50 && double(mixed.load()) > expected * 0.8); // the callback kept its pace
            for (auto &st : streams) st->close(); // removed: closing cannot race the callback
            out.setTap(nullptr);
            out.stop();
        }

        // --- Play modes (the video with sound of the previous test is muted meanwhile)
        e.setLayerMuted(v, true);
        { // Video ping-pong: the backward leg delivers the frames in reverse order (across GOPs)
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::PingPong;
            d.setTimeline(tl);
            d.seek(0);
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0;
            const double dt = 1.0 / 25, span = 2.0;
            for (int k = 0; k < 150; ++k) { // forward, backward, forward again
                const double t = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(t, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const double x = std::fmod(t, 2 * span);
                const int expected = x < span ? int(x / dt) : std::min(49, int((2 * span - x) / dt));
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      t=%.2f frame %d expected %d\n", t, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0);
        }
        { // Sound in ping-pong: backwards after the end (880 Hz part), then the start (440 Hz), no resync at the turns
            const int p = e.addLayer("Ping-pong");
            CHECK(e.setLayerAudio(p, root + "/media/tone.wav", &err));
            e.setLayerPlayMode(p, PlayMode::PingPong);
            e.seekLayer(p, 3.5);
            CHECK(tone(880, 20));
            settle(200);
            const int resyncs = e.layer(p)->audio->resyncCount();
            QElapsedTimer t;
            t.start();
            double minRms = 1;
            while (t.elapsed() < 1000) { // the turn at 4 s
                minRms = std::min(minRms, rms(last(0.05)));
                QThread::msleep(40);
            }
            double phase;
            {
                Engine::Lock lk(&e.mutex());
                phase = e.layer(p)->clock;
                CHECK(e.layer(p)->position() > 2.5 && e.layer(p)->position() < 3.9); // on the way back
            }
            std::printf("      ping-pong clock %.2f, min rms %.3f\n", phase, minRms);
            CHECK(minRms > 0.2);
            CHECK(std::abs(freq(last(0.15)) - 880) < 20);
            CHECK(tone(440, 15)); // backwards below 2 s
            CHECK(e.layer(p)->audio->resyncCount() == resyncs);
            e.removeLayer(p);
        }
        { // Negative speed, video: backwards from 1 s, then the loop goes on backwards from the end
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::Loop;
            tl.origin = 1.0;
            tl.dir = -1;
            d.setTimeline(tl);
            d.seek(0);
            tl.duration = 2.0;
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0;
            const double dt = 1.0 / 25;
            for (int k = 0; k < 120; ++k) {
                const double c = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(c, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const int expected = std::min(49, int(tl.position(c) / dt));
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      reverse c=%.2f frame %d expected %d\n", c, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0);
        }
        { // Negative speed, sound: backwards (880 Hz part, then 440 Hz), across 0 in Loop, then forwards again
            const int r = e.addLayer("Reverse");
            CHECK(e.setLayerAudio(r, root + "/media/tone.wav", &err));
            {
                // Backwards from its start: round to its end (Loop). Under the lock: the render thread, playing it
                // already, would otherwise move it off its start in between
                Engine::Lock lk(&e.mutex());
                e.seekLayer(r, 0);
                e.setLayerSpeed(r, -1.0);
                CHECK(e.layer(r)->dir == -1 && e.layer(r)->position() > 3.9);
            }
            e.seekLayer(r, 2.8);
            CHECK(tone(880, 20));
            CHECK(tone(440, 15)); // below 2 s, backwards
            double p1, p2;
            {
                Engine::Lock lk(&e.mutex());
                p1 = e.layer(r)->position();
            }
            settle(300);
            {
                Engine::Lock lk(&e.mutex());
                p2 = e.layer(r)->position();
            }
            std::printf("      backwards: %.2f -> %.2f\n", p1, p2);
            CHECK(p2 < p1 - 0.2);
            e.seekLayer(r, 0.3); // Loop: after 0, backwards from the end (880 Hz)
            CHECK(tone(440, 15));
            settle(100);
            const int resyncs = e.layer(r)->audio->resyncCount();
            CHECK(tone(880, 20));
            std::printf("      resyncs over the wrap: %d\n", e.layer(r)->audio->resyncCount() - resyncs);
#ifdef Q_OS_WIN
            // miniaudio's null device on Windows paces its callbacks with the system's coarse timer (about 15 ms):
            // the backwards wrap may be caught once there; a real device calls back on time
            CHECK(e.layer(r)->audio->resyncCount() - resyncs <= 1);
#else
            CHECK(e.layer(r)->audio->resyncCount() == resyncs); // the wrap is seamless
#endif
            // Direction change keeps the position
            e.seekLayer(r, 2.5);
            settle(150);
            double before, after;
            {
                Engine::Lock lk(&e.mutex());
                before = e.layer(r)->position();
            }
            e.setLayerSpeed(r, 1.0);
            {
                Engine::Lock lk(&e.mutex());
                after = e.layer(r)->position();
                CHECK(e.layer(r)->dir == 1);
            }
            CHECK(std::abs(after - before) < 0.05);
            settle(400);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(r)->position() > after + 0.25);
            }
            CHECK(waitFor([&] { return rms(last(0.1)) > 0.2; }));
            // One-shot backwards: stops at the start
            e.setLayerPlayMode(r, PlayMode::OneShot);
            e.setLayerSpeed(r, -2.0);
            e.seekLayer(r, 0.4);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(r)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(r)->position() < 1e-6);
            }
            CHECK(paramOf(e.layerJson(r), "speed").toDouble() == -2.0);
            e.removeLayer(r);
        }
        { // In / out points, video: the loop stays within [0.4, 1.2] s, frame by frame
            VideoDecoder d;
            CHECK(d.open(root + "/media/index.mp4", &err));
            Timeline tl;
            tl.mode = Timeline::Loop;
            tl.in = 0.4;
            tl.out = 1.2;
            tl.origin = 0.4;
            d.setTimeline(tl);
            d.seek(0);
            tl.duration = 2.0;
            std::vector<uint8_t> buf;
            int w = 0, h = 0, wrong = 0, missing = 0, outside = 0;
            const double dt = 1.0 / 25;
            for (int k = 0; k < 60; ++k) { // 3 loops of 0.8 s
                const double c = k * dt + dt / 2;
                QElapsedTimer tm;
                tm.start();
                bool got = false;
                while (!(got = d.fetchRgba(c, buf, &w, &h)) && tm.elapsed() < 3000) QThread::msleep(1);
                if (!got) {
                    ++missing;
                    continue;
                }
                const int expected = int(tl.position(c) / dt);
                const int r = buf[size_t((h / 2) * w + w / 2) * 4];
                const int frame = int(std::lround((r * 219.0 / 255.0 + 16.0 - 20.0) / 4.0));
                if (frame < 10 - 1 || frame > 29 + 1) ++outside;
                if (std::abs(frame - expected) > 1) {
                    if (wrong < 5) std::printf("      in/out c=%.2f frame %d expected %d\n", c, frame, expected);
                    ++wrong;
                }
            }
            CHECK(missing == 0);
            CHECK(wrong == 0 && outside == 0);
        }
        { // In / out points, sound: loop over [1.5, 2.5] s straddling the 440 / 880 Hz change, seamless
            const int a2 = e.addLayer("Range");
            CHECK(e.setLayerAudio(a2, root + "/media/tone.wav", &err));
            e.setLayerInOut(a2, 1.5, 2.5);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(a2)->position() >= 1.5 - 1e-9 && e.layer(a2)->position() <= 2.5 + 1e-9);
            }
            e.seekLayer(a2, 1.6);
            CHECK(tone(440, 15));
            settle(150);
            const int resyncs = e.layer(a2)->audio->resyncCount();
            bool saw880 = false, saw440After = false, outside = false;
            double minRms = 1;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 1800) { // more than one loop
                double pos;
                {
                    Engine::Lock lk(&e.mutex());
                    pos = e.layer(a2)->position();
                }
                outside |= pos < 1.5 - 1e-6 || pos > 2.5 + 1e-6;
                const double f = freq(last(0.08));
                if (std::abs(f - 880) < 30) saw880 = true;
                if (saw880 && std::abs(f - 440) < 20) saw440After = true; // back to the in point
                minRms = std::min(minRms, rms(last(0.05)));
                QThread::msleep(40);
            }
            CHECK(!outside);
            CHECK(saw880 && saw440After);
            CHECK(minRms > 0.2);
            CHECK(e.layer(a2)->audio->resyncCount() == resyncs);
            // One-shot stops at the out point; backwards, at the in point
            e.setLayerPlayMode(a2, PlayMode::OneShot);
            e.seekLayer(a2, 2.3);
            e.setLayerPlaying(a2, true);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(a2)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(std::abs(e.layer(a2)->position() - 2.5) < 1e-6);
            }
            e.setLayerSpeed(a2, -1.0);
            e.setLayerPlaying(a2, true); // at its end: starts again from the out point, backwards
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(a2)->playing;
            }));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(std::abs(e.layer(a2)->position() - 1.5) < 1e-6);
            }
            const QJsonObject j = e.layerJson(a2);
            CHECK(std::abs(paramOf(j, "in").toDouble() - 1.5) < 1e-9 && std::abs(paramOf(j, "out").toDouble() - 2.5) < 1e-9);
            CHECK(e.setLayerAudio(a2, root + "/media/tone.wav", &err)); // a new media is played whole
            CHECK(e.layer(a2)->inPoint == 0 && e.layer(a2)->outPoint < 0);
            CHECK(paramOf(e.layerJson(a2), "out").toDouble() == -1.0); // to its end, whatever its length
            {
                Engine::Lock lk(&e.mutex());
                const Parameter *in = e.layer(a2)->parameter("in");
                CHECK(in && !in->animatable() && in->info().ramp == ParamInfo::Ramp::Cut); // each change repositions the media
            }
            e.removeLayer(a2);
        }
        { // Stop: black and silent at the end; One-shot: last frame kept
            const int s1 = e.addLayer("Stop");
            CHECK(e.setLayerVideo(s1, root + "/media/av.mp4", &err));
            e.setLayerPlayMode(s1, PlayMode::Stop);
            e.seekLayer(s1, 3.7);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return e.layer(s1)->ended;
            }));
            settle(100);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(!e.layer(s1)->playing && e.layer(s1)->finalTex == 0);
            }
            CHECK(silent());
            e.setLayerPlayMode(s1, PlayMode::OneShot);
            e.seekLayer(s1, 3.7);
            e.setLayerPlaying(s1, true);
            CHECK(waitFor([&] {
                Engine::Lock lk(&e.mutex());
                return !e.layer(s1)->playing;
            }));
            settle(100);
            {
                Engine::Lock lk(&e.mutex());
                CHECK(!e.layer(s1)->ended && e.layer(s1)->finalTex != 0);
            }
            CHECK(paramOf(e.layerJson(s1), "play_mode").toString() == "oneshot");
            // Default mode (preference) given to a newly loaded video
            e.setDefaultPlayMode(PlayMode::PingPong);
            CHECK(e.setLayerVideo(s1, root + "/media/h264.mp4", &err));
            CHECK(e.layer(s1)->mode == PlayMode::PingPong);
            e.setDefaultPlayMode(PlayMode::Loop);
            e.removeLayer(s1);
        }
        e.setLayerMuted(v, false);

        e.removeLayer(e.layerCount() > v ? v : 0);
        e.audioOutput().setTap(nullptr);
        e.stopAudio();
    }
    } // sound

    e.stop();
    CHECK(!e.isThreaded());
    } // threaded
    e.renderFrame(); // back to manual mode
    CHECK(!e.grabOutput().isNull());

    if (area("timecode")) {
    // Timecode: numbers and drop frame, LTC frames through audio (each rate, two sample rates, inverted, quieter), MTC
    {
        const Timecode a{0, 1, 0, 2, TcRate::Fps2997Df};
        CHECK(a.valid() && a.toFrames() == 1800 && Timecode::fromFrames(1800, TcRate::Fps2997Df) == a);
        CHECK(!(Timecode{0, 1, 0, 0, TcRate::Fps2997Df}.valid()) && (Timecode{0, 10, 0, 0, TcRate::Fps2997Df}.valid()));
        CHECK(Timecode::fromFrames(17982, TcRate::Fps2997Df) == (Timecode{0, 10, 0, 0, TcRate::Fps2997Df}));
        bool round = true;
        for (int64_t n = 0; n < 200000 && round; n += 7) {
            const Timecode t = Timecode::fromFrames(n, TcRate::Fps2997Df);
            round = t.valid() && t.toFrames() == n;
        }
        CHECK(round);
        CHECK(Timecode::fromFrames(-1, TcRate::Fps25) == (Timecode{23, 59, 59, 24, TcRate::Fps25}));
        CHECK((Timecode{1, 2, 3, 4, TcRate::Fps2997Df}.toString()) == "01:02:03;04");
        CHECK(Timecode::parse("10:00:00:12", TcRate::Fps25) == (Timecode{10, 0, 0, 12, TcRate::Fps25}) &&
              !Timecode::parse("10:00:00:25", TcRate::Fps25));
        for (TcRate r : {TcRate::Fps24, TcRate::Fps25, TcRate::Fps2997Df, TcRate::Fps30})
            for (int sr : {48000, 44100})
                for (float gain : {1.0f, -0.3f}) {
                    ltc::Encoder enc(sr);
                    ltc::Decoder dec(sr);
                    std::vector<float> audio;
                    const int64_t s0 = Timecode{10, 59, 58, 0, r}.toFrames();
                    int got = 0, wrong = 0;
                    int64_t expect = -1;
                    for (int k = 0; k < 120; ++k) {
                        audio.clear();
                        enc.frame(Timecode::fromFrames(s0 + k, r), audio);
                        for (float &x : audio) x *= gain;
                        dec.push(audio.data(), audio.size());
                        if (auto t = dec.take()) {
                            ++got;
                            if ((expect >= 0 && t->toFrames() != expect) || t->rate != r) ++wrong;
                            expect = t->toFrames() + 1;
                        }
                    }
                    CHECK(got >= 115 && wrong == 0);
                }
        mtc::Decoder md;
        const Timecode q{1, 2, 3, 4, TcRate::Fps25};
        bool changed = false;
        for (int p = 0; p < 8; ++p) {
            const uint8_t msg[2] = {0xF1, mtc::quarterFrame(q, p)};
            changed = md.push(msg, 2);
        }
        CHECK(changed && md.current() && *md.current() == (Timecode{1, 2, 3, 6, TcRate::Fps25})); // two frames on
        const std::vector<uint8_t> ff = mtc::fullFrame(Timecode{23, 59, 59, 29, TcRate::Fps30});
        CHECK(md.push(ff.data(), ff.size()) && *md.current() == (Timecode{23, 59, 59, 29, TcRate::Fps30}));
    }
    // The Timecode generator: a text layer showing its clock; saved, restored, its settings changed
    {
        e.newProject();
        const int li = e.addLayer("TC");
        TimecodeSettings ts;
        ts.internal = TimecodeSettings::Internal::FreeRun;
        ts.offset = Timecode{1, 0, 0, 0, TcRate::Fps25}.toFrames();
        CHECK(e.setLayerTimecode(li, ts));
        CHECK(e.layerTimecode(li) && *e.layerTimecode(li) == ts);
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(li);
            CHECK(l->type == SourceType::Text && l->tcClock && l->text.width == 1600);
            const QString shown = l->tcClock->display();
            CHECK(shown.startsWith("01:00:0") && shown.size() == 11);
        }
        for (int k = 0; k < 3; ++k) e.renderFrame();
        CHECK(e.layerTimecodeStatus(li).startsWith("Free run"));
        const QJsonObject j = e.layerJson(li);
        CHECK(j.value("source").toObject().value("timecode").toObject().value("internal") == "free_run");
        ts.source = TimecodeSettings::Source::Mtc;
        CHECK(e.setLayerTimecode(li, ts) && e.layerTimecodeStatus(li).startsWith("No MTC"));
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(li);
            CHECK(l->text.width == 1600 && l->tcClock->display() == "--:--:--:--"); // its style kept; nothing received
            l->tcClock->received(Timecode{10, 0, 0, 0, TcRate::Fps25});
            CHECK(l->tcClock->display().startsWith("11:00:00")); // with its offset
        }
        e.replaceLayerJson(li, j); // back as it was saved
        CHECK(e.layerTimecode(li) && e.layerTimecode(li)->source == TimecodeSettings::Source::Internal);
        CHECK(e.setLayerImage(li, tmp + "/redblue.png", &err) && !e.layerTimecode(li)); // another source: no clock
        CHECK(TimecodeSettings::fromJson(ts.toJson()) == ts);
        ts.output = 42;
        CHECK(TimecodeSettings::fromJson(ts.toJson()) == ts && TimecodeSettings::fromJson(QJsonObject()) == TimecodeSettings());
    }
    // Sending: the sequence runs on evenly, jumps when the clock does; LTC and MTC as a receiver reads them
    {
        TcSequence seq;
        const Timecode t0{10, 0, 0, 0, TcRate::Fps25};
        auto at = [&](int64_t n) { return Timecode::fromFrames(t0.toFrames() + n, TcRate::Fps25); };
        CHECK(seq.next(t0) == t0 && !seq.jumped());
        CHECK(seq.next(at(2)) == at(1) && !seq.jumped()); // the clock a little ahead: one frame on all the same
        CHECK(seq.next(at(1)) == at(2));                  // a little behind: on too
        CHECK(seq.next(at(100)) == at(100) && seq.jumped());
        CHECK(!seq.next(std::nullopt) && seq.next(t0) == t0 && !seq.jumped()); // after none: a start, not a jump
        CHECK(TcSequence().next(Timecode{23, 59, 59, 24, TcRate::Fps25}) && [] {
            TcSequence w;
            w.next(Timecode{23, 59, 59, 24, TcRate::Fps25});
            return w.next(Timecode{0, 0, 0, 0, TcRate::Fps25}) == (Timecode{0, 0, 0, 0, TcRate::Fps25}) && !w.jumped();
        }()); // midnight: on, not a jump

        // LTC: a clock read per frame, generated in blocks of any size, decoded back in order
        for (TcRate r : {TcRate::Fps25, TcRate::Fps2997Df, TcRate::Fps30}) {
            ltc::Generator gen(48000);
            ltc::Decoder dec(48000);
            dec.setRateHint(r);
            int64_t reads = 0;
            const int64_t start = Timecode{1, 0, 0, 0, r}.toFrames();
            auto clock = [&]() -> std::optional<TcReading> {
                TcReading x;
                x.rate = r;
                x.running = true;
                x.frames = double(start + reads++);
                return x;
            };
            std::vector<float> st(2 * 997);
            int got = 0, wrong = 0;
            int64_t expect = -1;
            for (int b = 0; b < 200; ++b) {
                gen.render(st.data(), 997, 2, clock);
                std::vector<float> mono(997);
                for (int i = 0; i < 997; ++i) mono[size_t(i)] = st[size_t(2 * i + 1)]; // the right channel too
                dec.push(mono.data(), mono.size());
                if (auto t = dec.take()) {
                    if ((expect >= 0 && t->toFrames() != expect) || t->rate != r) ++wrong;
                    expect = t->toFrames() + 1;
                    ++got;
                }
            }
            CHECK(got > 90 && wrong == 0);
        }
        // LTC held (a pause) or backwards: the frame it shows, again; nothing shown: silence
        {
            ltc::Generator gen(48000);
            ltc::Decoder dec(48000);
            TcReading x;
            x.rate = TcRate::Fps25;
            x.frames = Timecode{3, 0, 0, 10, TcRate::Fps25}.toFrames() + 0.5;
            std::vector<float> buf(1920);
            int same = 0, other = 0;
            for (int b = 0; b < 50; ++b) {
                gen.render(buf.data(), 1920, 1, [&]() -> std::optional<TcReading> { return x; });
                dec.push(buf.data(), buf.size());
                if (auto t = dec.take()) (*t == (Timecode{3, 0, 0, 10, TcRate::Fps25}) ? same : other)++;
            }
            CHECK(same > 40 && other == 0);
            float peak = 0;
            gen.render(buf.data(), 1920, 1, [] { return std::optional<TcReading>(); }); // the end of the frame begun
            gen.render(buf.data(), 1920, 1, [] { return std::optional<TcReading>(); });
            for (float f : buf) peak = std::max(peak, std::abs(f));
            CHECK(peak == 0.0f);
        }
    }
    // ATC: the timecode of the layer that sends it goes with the viewports' published pictures
    {
        e.newProject();
        const int li = e.addLayer("TC");
        TimecodeSettings ts;
        ts.output = e.addTimecodeOutput();
        CHECK(e.setLayerTimecode(li, ts));
        quint64 vp = 0;
        for (int i = 0; i < e.layerCount() && !vp; ++i)
            if (e.isViewport(i)) vp = e.layerId(i);
        TimecodeOutput to = e.timecodeOutputs()[0];
        to.routes = {tcroute::viewport(vp), tcroute::viewport(987654)}; // a viewport, and one that is not there
        CHECK(e.setTimecodeOutput(to) && e.timecodeOutputs()[0].routes == QStringList({tcroute::viewport(vp)}));
        CHECK(e.timecodeOutputStatus(ts.output).contains("ATC to"));
        struct Got {
            std::mutex m;
            std::optional<Timecode> tc;
            int frames = 0;
        };
        auto got = std::make_shared<Got>();
        e.setTestTap([got](const CpuFrame &f) {
            std::lock_guard<std::mutex> lk(got->m);
            got->tc = f.timecode;
            ++got->frames;
        });
        for (int k = 0; k < 20; ++k) {
            e.renderFrame();
            QThread::msleep(10);
        }
        {
            std::lock_guard<std::mutex> lk(got->m);
            const Timecode wall = Timecode::fromSeconds(QTime::currentTime().msecsSinceStartOfDay() / 1000.0, TcRate::Fps25);
            CHECK(got->frames > 5 && got->tc && std::llabs(got->tc->toFrames() - wall.toFrames()) < 50);
        }
        to.routes.clear(); // not patched to it any more
        CHECK(e.setTimecodeOutput(to));
        for (int k = 0; k < 6; ++k) {
            e.renderFrame();
            QThread::msleep(10);
        }
        {
            std::lock_guard<std::mutex> lk(got->m);
            CHECK(!got->tc);
        }
        e.setTestTap(nullptr);
    }
    // The transport of a free run: play, pause, seek, in / out points and the play modes, on a test clock
    {
        TimecodeSettings fs;
        fs.internal = TimecodeSettings::Internal::FreeRun;
        TimecodeClock c(fs);
        auto at = [&](qint64 ms) { c.setTestTime(ms); };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
        at(0);
        c.rewind();
        CHECK(c.usesTransport() && c.playing() && near(c.position(), 0));
        at(2000);
        CHECK(near(c.position(), 2) && c.running());
        c.setPlaying(false);
        at(5000);
        CHECK(near(c.position(), 2) && !c.running() && !c.reading()->running);
        c.setPlaying(true);
        at(6000);
        CHECK(near(c.position(), 3));
        c.setInOut(10, 12); // the position is kept within them
        CHECK(near(c.inPoint(), 10) && near(c.outPoint(), 12) && near(c.position(), 10));
        at(8500); // loop: 10 → 12, then 10 → 10.5
        CHECK(near(c.position(), 10.5) && c.reading()->forward);
        c.setPlayMode(TcPlayMode::PingPong);
        at(10500); // to 12 in 1.5 s, then back 0.5 s
        CHECK(near(c.position(), 11.5) && !c.reading()->forward);
        c.seek(11); // a seek keeps its way
        at(11000);
        CHECK(near(c.position(), 10.5) && !c.reading()->forward);
        c.setPlayMode(TcPlayMode::OneShot); // backwards to the in point, and it ends there
        at(13000);
        CHECK(near(c.position(), 10) && !c.running() && c.now());
        c.setPlaying(true); // played to its end: from its in point again, forwards
        at(13500);
        CHECK(near(c.position(), 10.5) && c.running() && c.reading()->forward);
        CHECK(c.now() == (Timecode{0, 0, 10, 12, TcRate::Fps25}));
        c.setPlayMode(TcPlayMode::Stop);
        at(20000);
        CHECK(!c.now() && !c.reading() && c.display().startsWith("--")); // stopped at its end: nothing shown or sent
        c.rewind();
        CHECK(c.now() == (Timecode{0, 0, 10, 0, TcRate::Fps25}));
        c.setInOut(10, 24 * 3600.0); // to the end of the day: no out point
        CHECK(c.outPoint() < 0);
        TimecodeSettings fs2 = fs;
        fs2.rate = TcRate::Fps30;
        TimecodeClock d(fs2); // settings changed: a new clock takes the transport over
        d.setTestTime(0);
        d.takeTransport(c);
        CHECK(near(d.position(), 10) && near(d.inPoint(), 10) && d.playMode() == TcPlayMode::Stop && d.playing());
        CHECK(tcPlayModeFromKey(tcPlayModeKey(TcPlayMode::PingPong)) == TcPlayMode::PingPong);
    }
    // MTC sent from a clock's readings: a full frame at the start, then four quarter frames a frame, each sequence of
    // eight on an even frame; held: a full frame when the frame changes; a jump: a full frame again
    {
        MtcSender s;
        mtc::Decoder dec;
        std::vector<MtcSender::Message> out;
        TcReading r;
        r.rate = TcRate::Fps25;
        r.running = true;
        r.frames = 1001.5;
        s.step(r, out);
        CHECK(out.size() == 1 && out[0].size() == 10 && out[0][0] == 0xF0);
        CHECK(dec.push(out[0].data(), out[0].size()) && dec.current()->toFrames() == 1001);
        out.clear();
        int quarters = 0, sequences = 0, wrong = 0;
        for (int k = 1; k <= 400; ++k) { // 20 frames, a step of 0.05 frame
            r.frames = 1001.5 + k * 0.05;
            s.step(r, out);
            for (const auto &m : out) {
                if (m.size() != 2 || m[0] != 0xF1) {
                    ++wrong;
                    continue;
                }
                ++quarters;
                if (dec.push(m.data(), m.size())) { // piece 7: two frames after its sequence began, an even frame
                    ++sequences;
                    const int64_t got = dec.current()->toFrames();
                    if (got % 2 != 0 || std::abs(double(got) - r.frames) > 1.0) ++wrong;
                }
            }
            out.clear();
        }
        CHECK(wrong == 0 && quarters == 80 && sequences >= 9);
        r.running = false;
        s.step(r, out);
        CHECK(out.size() == 1 && out[0][0] == 0xF0); // stopped: where it is
        out.clear();
        s.step(r, out);
        CHECK(out.empty()); // still there: nothing
        r.frames += 3;
        s.step(r, out);
        CHECK(out.size() == 1 && dec.push(out[0].data(), out[0].size()) && dec.current()->toFrames() == int64_t(r.frames));
        out.clear();
        r.running = true;
        s.step(r, out);
        CHECK(out.size() == 1 && out[0][0] == 0xF0); // runs again
        out.clear();
        r.frames += 50; // a seek
        s.step(r, out);
        CHECK(out.size() == 1 && out[0][0] == 0xF0);
        out.clear();
        r.forward = false; // backwards: full frames
        r.frames -= 1;
        s.step(r, out);
        CHECK(out.size() == 1 && out[0][0] == 0xF0);
        out.clear();
        s.step(std::nullopt, out);
        CHECK(out.empty());
    }
    // Timecode outputs and the MIDI patch: one layer per output, an output to several destinations, the protocol given
    // by the destination; the transport in the layer's values (snapshots); saved and opened again; sent by the router
    {
        e.newProject();
        CHECK(e.timecodeOutputs().empty() && e.midiOutputs().empty());
        const quint64 o1 = e.addTimecodeOutput(), o2 = e.addTimecodeOutput();
        const quint64 m1 = e.addMidiOutput(), m2 = e.addMidiOutput("Desk");
        CHECK(e.timecodeOutputs().size() == 2 && e.timecodeOutputs()[1].name == "Timecode 2");
        CHECK(e.midiOutputs()[0].name == "MIDI 1" && e.midiOutputs()[1].name == "Desk");
        CHECK(e.addMidiOutput("Desk") && e.midiOutputs()[2].name == "Desk 2"); // names stay apart
        e.removeMidiOutput(e.midiOutputs()[2].id);
        TimecodeOutput t = e.timecodeOutputs()[0];
        t.name = "Show";
        t.routes = {tcroute::midi(m1), tcroute::midi(m2), tcroute::midi(m1), "midi:999", "audio:x"};
        CHECK(e.setTimecodeOutput(t));
        t = e.timecodeOutputs()[0];
        CHECK(t.name == "Show" && t.routes == QStringList({tcroute::midi(m1), tcroute::midi(m2)})); // known ones, once
        CHECK(tcroute::protocol(tcroute::kind(t.routes[0])) == "MTC" && tcroute::kind("audio:x") == tcroute::Kind::None);
        CHECK(tcroute::protocol(tcroute::kind(tcroute::audio(3))) == "LTC" && tcroute::protocol(tcroute::kind("viewport:7")) == "ATC");
        // An audio channel (LTC), and a destination takes one timecode output
        const quint64 a1 = e.addAudioChannelOutput();
        CHECK(e.audioChannelOutputs()[0].name == "LTC 1" && e.audioChannelOutputStatus(a1) != "Sending LTC");
        AudioChannelOutput ao = e.audioChannelOutputs()[0];
        ao.channel = 99;
        CHECK(e.setAudioChannelOutput(ao) && e.audioChannelOutputs()[0].channel == 63);
        ao.channel = 2;
        ao.device = "No Such Device";
        CHECK(e.setAudioChannelOutput(ao));
        t.routes << tcroute::audio(a1);
        CHECK(e.setTimecodeOutput(t) && e.timecodeOutputs()[0].routes.contains(tcroute::audio(a1)));
        TimecodeOutput t2 = e.timecodeOutputs()[1];
        t2.routes = {tcroute::audio(a1), tcroute::midi(m2)};
        CHECK(e.setTimecodeOutput(t2) && e.timecodeOutputs()[1].routes.isEmpty()); // both are Show's
        CHECK(e.routeName(tcroute::audio(a1)) == "LTC 1" && e.routeName(tcroute::midi(m2)) == "Desk");
        MidiOutput mo = e.midiOutputs()[0];
        mo.port = MidiOutput::kVirtualPort;
        CHECK(e.setMidiOutput(mo) && e.midiOutputs()[0].port == MidiOutput::kVirtualPort);
        (void)Engine::midiPorts(); // whatever this machine has (none without a MIDI system)

        const quint64 idA = e.layerId(e.addLayer("A")), idB = e.layerId(e.addLayer("B"));
        auto a = [&] { return e.indexOfId(idA); };
        auto b = [&] { return e.indexOfId(idB); };
        TimecodeSettings fs;
        fs.internal = TimecodeSettings::Internal::FreeRun;
        fs.output = o1;
        CHECK(e.setLayerTimecode(a(), fs) && e.timecodeOutputUser(o1) == a());
        CHECK(!e.setLayerTimecode(b(), fs)); // o1 is A's
        fs.output = 12345;
        CHECK(!e.setLayerTimecode(b(), fs)); // not an output of the composition
        fs.output = o2;
        CHECK(e.setLayerTimecode(b(), fs) && e.timecodeOutputUser(o2) == b());
        CHECK(e.timecodeOutputStatus(o1).startsWith("From A") && e.timecodeOutputStatus(o1).contains("MTC to Desk"));
        e.removeTimecodeOutput(o2);
        CHECK(e.timecodeOutputs().size() == 1 && e.layerTimecode(b())->output == 0 && e.timecodeOutputUser(o2) < 0);
        e.removeMidiOutput(m2);
        CHECK(e.timecodeOutputs()[0].routes == QStringList({tcroute::midi(m1), tcroute::audio(a1)}));

        // The transport is in the layer's values: a snapshot holds it
        auto clockA = [&] { return e.layerTimecodeClock(e.indexOfId(idA)); };
        auto param = [&](const char *path) {
            Engine::Lock lk(&e.mutex());
            return e.layer(e.indexOfId(idA))->parameter(path);
        };
        CHECK(param("tc/play") && param("tc/position") && param("tc/in") && param("tc/out") && param("tc/play_mode") &&
              param("tc/rewind"));
        clockA()->setTestTime(0);
        clockA()->setPlaying(false);
        clockA()->seek(100);
        clockA()->setPlayMode(TcPlayMode::PingPong);
        Engine::Snapshot snap;
        snap.fade = 0;
        snap.layers = e.captureLayers();
        e.addSnapshot(snap); // 0: paused at 100
        clockA()->setPlaying(true);
        clockA()->setTestTime(5000);
        snap.layers = e.captureLayers();
        e.addSnapshot(snap); // 1: playing, stored at 105
        clockA()->seek(300);
        clockA()->setPlayMode(TcPlayMode::Loop);
        e.recallSnapshot(0);
        CHECK(!clockA()->playing() && std::abs(clockA()->position() - 100) < 1e-6 && clockA()->playMode() == TcPlayMode::PingPong);
        e.recallSnapshot(1); // it plays there: from where it was stored, playing
        CHECK(clockA()->playing() && std::abs(clockA()->position() - 105) < 1e-6);
        clockA()->seek(200);
        e.recallSnapshot(1); // playing already: it goes on
        CHECK(clockA()->playing() && std::abs(clockA()->position() - 200) < 1e-6);
        CHECK(param("tc/rewind")->setValue(QVariant()) && std::abs(clockA()->position()) < 1e-6);
        // A new clock (its settings changed) keeps the transport
        clockA()->seek(42);
        clockA()->setPlaying(false);
        fs.output = o1;
        fs.rate = TcRate::Fps30;
        CHECK(e.setLayerTimecode(e.indexOfId(idA), fs));
        CHECK(clockA()->settings().rate == TcRate::Fps30 && !clockA()->playing() && std::abs(clockA()->position() - 42) < 1e-6);

        // Saved and opened again: the patch, and which layer drives which output
        CHECK(e.saveProject(tmp + "/tcout.pinol", {}, &err));
        e.newProject();
        CHECK(e.timecodeOutputs().empty());
        CHECK(e.loadProject(tmp + "/tcout.pinol", nullptr, &err));
        CHECK(e.timecodeOutputs().size() == 1 && e.timecodeOutputs()[0].name == "Show" &&
              e.timecodeOutputs()[0].routes == QStringList({tcroute::midi(m1), tcroute::audio(a1)}));
        CHECK(e.audioChannelOutputs().size() == 1 && e.audioChannelOutputs()[0] == ao);
        CHECK(e.midiOutputs().size() == 1 && e.midiOutputs()[0].id == m1 && e.midiOutputs()[0].port == MidiOutput::kVirtualPort);
        const int a2 = e.indexOfId(idA);
        CHECK(a2 >= 0 && e.layerTimecode(a2) && e.layerTimecode(a2)->output == o1 && e.timecodeOutputUser(o1) == a2);
        CHECK(e.addTimecodeOutput() > std::max({o1, m1, a1})); // ids go on after the saved ones
        {
            QElapsedTimer w;
            w.start();
            // Opening the device runs on its own thread (the audio system may take seconds to list its devices)
            while (w.elapsed() < 15000 && e.audioChannelOutputStatus(a1).isEmpty()) QThread::msleep(20);
            const QString st = e.audioChannelOutputStatus(a1);
            std::printf("       LTC 1: %s (%lld ms)\n", qPrintable(st), qint64(w.elapsed()));
            CHECK(st.contains("not found") || st.contains("audio system") || st.startsWith("cannot"));
        }
        // A copy of the layer drives nothing (one layer per output)
        const int copy = e.insertLayerJson(e.layerCount(), e.layerJson(a2));
        CHECK(e.layerTimecode(copy) && e.layerTimecode(copy)->output == 0 && e.timecodeOutputUser(o1) == a2);

        // The router sends the clock's MTC to the MIDI output (to a test sink here, not to a port)
        std::mutex sentMutex;
        std::vector<std::vector<uint8_t>> sent;
        e.timecodeRouter().setTestSink([&](quint64 midi, const std::vector<uint8_t> &msg) {
            std::lock_guard<std::mutex> lk(sentMutex);
            if (midi == m1) sent.push_back(msg);
        });
        e.layerTimecodeClock(a2)->setPlaying(true);
        e.layerTimecodeClock(a2)->seek(3600); // 01:00:00:00
        QElapsedTimer wait;
        wait.start();
        std::optional<Timecode> got;
        while (wait.elapsed() < 3000 && !got) {
            QThread::msleep(20);
            std::lock_guard<std::mutex> lk(sentMutex);
            mtc::Decoder dec;
            int quarters = 0;
            for (const auto &m : sent) quarters += m[0] == 0xF1 && dec.push(m.data(), m.size());
            if (quarters >= 2) got = dec.current();
        }
        CHECK(got && got->rate == TcRate::Fps30 && got->h == 1 && got->m == 0 && got->s < 4);
        CHECK(e.midiOutputStatus(m1) == "Test");
        {
            std::lock_guard<std::mutex> lk(sentMutex);
            CHECK(!sent.empty() && sent.front()[0] == 0xF0); // a full frame first
        }
        e.timecodeRouter().setTestSink(nullptr);
    }
    }

    if (area("interactions")) {
    // The chain of an interaction, on its own: range, toggle, filters, curve, master, pick-up
    {
        auto near = [](double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; };
        Interaction it = Interaction::made(InteractionSource::Midi);
        CHECK(it.takeover == Takeover::PickUp && Interaction::made(InteractionSource::Keyboard).takeover == Takeover::Jump);
        InteractionProcessor pr;
        CHECK(!pr.step(it, 0.016));
        it.srcMin = 0.2;
        it.srcMax = 0.8;
        pr.input(it, 0.5);
        CHECK(near(*pr.step(it, 0.016), 0.5));
        pr.input(it, 0.95);
        CHECK(near(*pr.step(it, 0.016), 1.0)); // beyond the range: held at its end
        it.srcMin = 0;
        it.srcMax = 1;
        it.master = 0.5;
        pr.input(it, 0.8);
        CHECK(near(*pr.step(it, 0.016), 0.4));
        it.master = 1;
        it.curveOn = true; // an inverted curve
        it.curve.keys[0].v = 1;
        it.curve.keys[1].v = 0;
        pr.input(it, 0.25);
        CHECK(near(*pr.step(it, 0.016), 0.75));
        it.curveOn = false;
        // Toggle: up past the middle flips
        it.toggle = true;
        InteractionProcessor tg;
        std::vector<double> outs;
        for (double x : {1.0, 0.0, 1.0, 0.0, 0.3, 1.0}) {
            tg.input(it, x);
            outs.push_back(*tg.step(it, 0.016));
        }
        CHECK(outs == std::vector<double>({1, 1, 0, 0, 0, 1}));
        it.toggle = false;
        // Filters: from 0, a step to 1, sixty frames a second
        auto run = [&](InteractionFilter f, double a, double b, double seconds, double *peak = nullptr) {
            Interaction x = it;
            x.filterOn = true;
            x.filter = f;
            x.filterA = a;
            x.filterB = b;
            InteractionProcessor q;
            q.input(x, 0);
            for (int k = 0; k < 120; ++k) q.step(x, 1.0 / 60); // at rest for two seconds
            q.input(x, 1);
            double y = 0, top = 0;
            for (double t = 0; t < seconds; t += 1.0 / 60) {
                y = *q.step(x, 1.0 / 60);
                top = std::max(top, y);
            }
            if (peak) *peak = top;
            return y;
        };
        CHECK(run(InteractionFilter::Average, 0.5, 0, 0.25) > 0.4 && run(InteractionFilter::Average, 0.5, 0, 0.25) < 0.6);
        CHECK(run(InteractionFilter::Average, 0.5, 0, 1.0) > 0.99);
        CHECK(run(InteractionFilter::WeightedAverage, 0.5, 0, 0.1) < 0.6 && run(InteractionFilter::WeightedAverage, 0.5, 0, 1.0) > 0.98);
        CHECK(run(InteractionFilter::AttackRelease, 0.01, 2, 0.1) > 0.99); // a fast attack
        double peak = 0;
        CHECK(run(InteractionFilter::PulseDetection, 0.2, 0.3, 0.6, &peak) < 1e-9 && peak > 0.9); // a pulse, gone in 0.3 s
        CHECK(std::abs(run(InteractionFilter::Damper, 0.3, 0.7, 3.0) - 1.0) < 0.01); // settles where it is sent
        // Pick-up: the target at 0.5 is left alone until the controller meets it
        Interaction pu = Interaction::made(InteractionSource::Midi);
        InteractionTargetState ts;
        CHECK(!ts.numberToWrite(pu, 0.1, 0.5, 0.01) && !ts.numberToWrite(pu, 0.3, 0.5, 0.01));
        CHECK(ts.numberToWrite(pu, 0.6, 0.5, 0.01) && near(*ts.written, 0.6)); // crossed it
        CHECK(ts.numberToWrite(pu, 0.7, 0.6, 0.01) && near(*ts.written, 0.7));
        CHECK(!ts.numberToWrite(pu, 0.75, 0.2, 0.01)); // moved elsewhere (a snapshot): let go again
        Interaction jp = Interaction::made(InteractionSource::Keyboard);
        InteractionTargetState tj;
        CHECK(tj.numberToWrite(jp, 0.1, 0.5, 0.01) && near(*tj.written, 0.1));
        // A file and back
        it.name = "Fader";
        it.midiPort = "nanoKONTROL";
        it.midiChannel = 3;
        it.midiNumber = 14;
        it.filterOn = true;
        it.filter = InteractionFilter::Damper;
        it.layer = 42;
        it.param = "opacity";
        const Interaction back = Interaction::fromJson(it.toJson());
        CHECK(back.toJson() == it.toJson() && back.midiPort == "nanoKONTROL" && back.filter == InteractionFilter::Damper);
    }
    // In the engine: MIDI, a key, OSC and another value drive the show's values, each frame
    {
        e.newProject();
        const quint64 a = e.layerId(e.addLayer("A")), b = e.layerId(e.addLayer("B"));
        CHECK(e.setLayerImage(e.indexOfId(a), tmp + "/redblue.png", &err));
        auto opacity = [&](quint64 id) { return e.parameterValue(id, "opacity").toDouble(); };
        auto midiCc = [&](int ch, int cc, double v) {
            InteractionEvent ev;
            ev.source = InteractionSource::Midi;
            ev.port = "Test Port";
            ev.channel = ch;
            ev.message = MidiMessageKind::ControlChange;
            ev.number = cc;
            ev.value = v;
            e.feedInteraction(ev);
        };
        Interaction fader = Interaction::made(InteractionSource::Midi);
        fader.midiChannel = 1;
        fader.midiNumber = 7;
        fader.layer = a;
        fader.param = "opacity";
        fader.takeover = Takeover::Jump;
        const quint64 f = e.addInteraction(fader);
        CHECK(e.interactions().size() == 1 && e.interactions()[0].id == f && e.interactions()[0].name == "Interaction 1");
        midiCc(1, 7, 0.25);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.25) < 1e-6);
        midiCc(2, 7, 0.9); // another channel: not this one
        midiCc(1, 8, 0.9); // another controller
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.25) < 1e-6);
        double level = 0, output = 0;
        CHECK(e.interactionLevels(f, &level, &output) && std::abs(level - 0.25) < 1e-6);
        // Pick-up: moved by hand to 0.8, the fader at 0.1 leaves it; brought past it, it takes it
        fader.id = f;
        fader.takeover = Takeover::PickUp;
        CHECK(e.setInteraction(fader));
        CHECK(e.setParameter(a, "opacity", 0.8));
        midiCc(1, 7, 0.1);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.8) < 1e-6);
        midiCc(1, 7, 0.5);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.8) < 1e-6);
        midiCc(1, 7, 0.85);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.85) < 1e-6);
        // Target range: 0..1 lands on 0.2..0.4; disabled: nothing
        fader.takeover = Takeover::Jump;
        fader.dstMin = 0.2;
        fader.dstMax = 0.4;
        CHECK(e.setInteraction(fader));
        midiCc(1, 7, 0.5);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.3) < 1e-6);
        fader.enabled = false;
        CHECK(e.setInteraction(fader));
        midiCc(1, 7, 1.0);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.3) < 1e-6);
        // A locked layer is left alone
        fader.enabled = true;
        CHECK(e.setInteraction(fader));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(a))->locked = true;
        }
        midiCc(1, 7, 0.0);
        e.renderFrame();
        CHECK(std::abs(opacity(a) - 0.3) < 1e-6);
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(a))->locked = false;
        }
        e.removeInteraction(f);
        CHECK(e.interactions().empty());

        // A key toggling a switch (a picture layer's effects)
        Interaction key = Interaction::made(InteractionSource::Keyboard);
        key.key = "Space";
        key.toggle = true;
        key.layer = a;
        key.param = "fx/enable";
        e.addInteraction(key);
        auto press = [&](const QString &k, double v) {
            InteractionEvent ev;
            ev.source = InteractionSource::Keyboard;
            ev.address = k;
            ev.value = v;
            e.feedInteraction(ev);
        };
        CHECK(e.parameterValue(a, "fx/enable").toBool());
        press("Space", 1);
        press("Space", 0); // pressed and let go within a frame: one press, the toggle at 1
        e.renderFrame();
        CHECK(e.parameterValue(a, "fx/enable").toBool());
        press("Space", 1); // the second: 0
        e.renderFrame();
        press("Space", 0);
        e.renderFrame();
        CHECK(!e.parameterValue(a, "fx/enable").toBool());
        press("Space", 1); // the third: 1 again
        press("Space", 0);
        e.renderFrame();
        CHECK(e.parameterValue(a, "fx/enable").toBool());

        // OSC: a message's argument
        OscServer server(&e);
        Interaction osc = Interaction::made(InteractionSource::Osc);
        osc.oscAddress = "/fader/1";
        osc.oscArgument = 1;
        osc.takeover = Takeover::Jump;
        osc.srcMin = 0;
        osc.srcMax = 100;
        osc.layer = b;
        osc.param = "opacity";
        e.addInteraction(osc);
        osc::Message m;
        m.address = "/fader/1";
        m.args = {QVariant(5), QVariant(40.0)};
        server.handleMessage(m);
        e.renderFrame();
        CHECK(std::abs(opacity(b) - 0.4) < 1e-6);

        // OSC -> whole ISF color: never clobber the target's alpha unless
        // the user explicitly enabled the RGBA mapping option.
        const quint64 colorLayer = e.layerId(e.addLayer("Interaction Color"));
        CHECK(e.setLayerIsf(e.indexOfId(colorLayer), root + "/../isf/generators/SolidColor.fs", &err));
        auto component = [&](const char *channel) {
            return e.parameterValue(colorLayer, QStringLiteral("param/color/") + QString::fromLatin1(channel)).toDouble();
        };
        CHECK(e.setParameter(colorLayer, "param/color/a", 0.75));
        Interaction color = Interaction::made(InteractionSource::Osc);
        color.oscAddress = QStringLiteral("/chataigne/color");
        color.layer = colorLayer;
        color.param = QStringLiteral("param/color"); // the natural group address
        color.takeover = Takeover::Jump;
        CHECK(!color.colorMapAlpha);
        color.id = e.addInteraction(color);
        CHECK(!Interaction::fromJson(color.toJson()).colorMapAlpha); // old projects default to preserving alpha

        osc::Message colorMessage;
        colorMessage.address = color.oscAddress;
        colorMessage.types = QStringLiteral("f");
        colorMessage.args = {QVariant(0.25)};
        server.handleMessage(colorMessage);
        e.renderFrame();
        CHECK(std::abs(component("r") - 0.25) < 1e-6 &&
              std::abs(component("g") - 0.25) < 1e-6 &&
              std::abs(component("b") - 0.25) < 1e-6 &&
              std::abs(component("a") - 0.75) < 1e-6);

        colorMessage.types = QStringLiteral("fff");
        colorMessage.args = {QVariant(0.1), QVariant(0.6), QVariant(0.9)};
        server.handleMessage(colorMessage);
        e.renderFrame();
        CHECK(std::abs(component("r") - 0.1) < 1e-6 &&
              std::abs(component("g") - 0.6) < 1e-6 &&
              std::abs(component("b") - 0.9) < 1e-6 &&
              std::abs(component("a") - 0.75) < 1e-6);

        // Even if a sender includes an unused fourth value of ZERO, alpha is
        // kept unless explicitly mapped (common with Chataigne / OSC color).
        colorMessage.types = QStringLiteral("ffff");
        colorMessage.args = {QVariant(0.8), QVariant(0.2), QVariant(0.4), QVariant(0.0)};
        server.handleMessage(colorMessage);
        e.renderFrame();
        CHECK(std::abs(component("r") - 0.8) < 1e-6 &&
              std::abs(component("g") - 0.2) < 1e-6 &&
              std::abs(component("b") - 0.4) < 1e-6 &&
              std::abs(component("a") - 0.75) < 1e-6);

        // Decode from real native OSC bytes (r = 0xRRGGBBAA).
        colorMessage.types = QStringLiteral("r");
        colorMessage.args = {QVariant(qlonglong(0x3366CC00))};
        CHECK(server.handlePacket(osc::encode(colorMessage)) == 0);
        e.renderFrame();
        CHECK(std::abs(component("r") - 0x33 / 255.0) < 1e-6 &&
              std::abs(component("g") - 0x66 / 255.0) < 1e-6 &&
              std::abs(component("b") - 0xCC / 255.0) < 1e-6 &&
              std::abs(component("a") - 0.75) < 1e-6);

        // Opt-in must be stored in projects and snapshot state.
        color.colorMapAlpha = true;
        CHECK(Interaction::fromJson(color.toJson()).colorMapAlpha);
        CHECK(Interaction::fromJson(color.stateJson()).colorMapAlpha);
        CHECK(e.setInteraction(color));
        colorMessage.types = QStringLiteral("ffff");
        colorMessage.args = {QVariant(0.8), QVariant(0.2), QVariant(0.4), QVariant(0.0)};
        server.handleMessage(colorMessage);
        e.renderFrame();
        CHECK(std::abs(component("a")) < 1e-6); // deliberately mapped zero alpha
        colorMessage.types = QStringLiteral("r");
        colorMessage.args = {QVariant(qlonglong(0x3366CC80))};
        CHECK(server.handlePacket(osc::encode(colorMessage)) == 0);
        e.renderFrame();
        CHECK(std::abs(component("a") - 0x80 / 255.0) < 1e-6);

        // Turning it off again immediately preserves alpha on further RGBA packets.
        color.colorMapAlpha = false;
        CHECK(e.setInteraction(color));
        colorMessage.args = {QVariant(qlonglong(0x3366CC00))};
        CHECK(server.handlePacket(osc::encode(colorMessage)) == 0);
        e.renderFrame();
        CHECK(std::abs(component("a") - 0x80 / 255.0) < 1e-6);

        // The native OSCQuery address of a color, and an interaction listening on that same address (audit P0):
        // the interaction owns it, the direct write is not made as well, so its alpha stays as Map alpha says.
        {
            e.setInteractions({});
            const quint64 cl = e.layerId(e.addLayer("Chataigne"));
            CHECK(e.setLayerIsf(e.indexOfId(cl), root + "/../isf/generators/SolidColor.fs", &err));
            auto ch = [&](const char *c) {
                return e.parameterValue(cl, QStringLiteral("param/color/") + QString::fromLatin1(c)).toDouble();
            };
            const QString native = QStringLiteral("/layer/Chataigne/param/color");
            osc::Message direct;
            direct.address = native;
            // No interaction: the address writes RGBA directly (its public API), bytes read as 0..255
            direct.types = QStringLiteral("iiii");
            direct.args = {QVariant(255), QVariant(0), QVariant(51), QVariant(204)};
            CHECK(server.handleMessage(direct));
            CHECK(std::abs(ch("r") - 1.0) < 1e-6 && std::abs(ch("g")) < 1e-6 &&
                  std::abs(ch("b") - 0.2) < 1e-6 && std::abs(ch("a") - 0.8) < 1e-6);
            direct.types = QStringLiteral("ffff");
            direct.args = {QVariant(0.5), QVariant(0.5), QVariant(0.5), QVariant(0.6)};
            CHECK(server.handleMessage(direct) && std::abs(ch("a") - 0.6) < 1e-6);

            Interaction own = Interaction::made(InteractionSource::Osc);
            own.oscAddress = native;
            own.layer = cl;
            own.param = QStringLiteral("param/color");
            own.takeover = Takeover::Jump;
            own.id = e.addInteraction(own);
            CHECK(e.oscAddressClaimed(native) && !e.oscAddressClaimed(native + "/r"));
            // Chataigne's RGBA with an unused alpha of 0: RGB follows, alpha stays (Map alpha off)
            direct.args = {QVariant(0.1), QVariant(0.2), QVariant(0.3), QVariant(0.0)};
            CHECK(!server.handleMessage(direct)); // nothing written directly
            CHECK(std::abs(ch("a") - 0.6) < 1e-6); // not even before the frame
            e.renderFrame();
            CHECK(std::abs(ch("r") - 0.1) < 1e-6 && std::abs(ch("g") - 0.2) < 1e-6 &&
                  std::abs(ch("b") - 0.3) < 1e-6 && std::abs(ch("a") - 0.6) < 1e-6);
            // The same as raw bytes, and as OSC 'r'
            CHECK(server.handlePacket(osc::encode(direct)) == 0);
            direct.types = QStringLiteral("r");
            direct.args = {QVariant(qlonglong(0x3366CC00))};
            CHECK(server.handlePacket(osc::encode(direct)) == 0);
            e.renderFrame();
            CHECK(std::abs(ch("r") - 0x33 / 255.0) < 1e-6 && std::abs(ch("a") - 0.6) < 1e-6);
            // Bytes reach the interaction as they reach the native address: 0..255
            direct.types = QStringLiteral("iiii");
            direct.args = {QVariant(0), QVariant(255), QVariant(0), QVariant(0)};
            CHECK(!server.handleMessage(direct));
            e.renderFrame();
            CHECK(std::abs(ch("g") - 1.0) < 1e-6 && std::abs(ch("r")) < 1e-6 && std::abs(ch("a") - 0.6) < 1e-6);
            // Map alpha on: the zero alpha is meant, it is applied — by the interaction, still the only writer
            own.colorMapAlpha = true;
            CHECK(e.setInteraction(own));
            direct.types = QStringLiteral("ffff");
            direct.args = {QVariant(0.4), QVariant(0.4), QVariant(0.4), QVariant(0.0)};
            CHECK(!server.handleMessage(direct));
            e.renderFrame();
            CHECK(std::abs(ch("a")) < 1e-6);
            // Disabled, it lets the address go: the direct write is back
            own.enabled = false;
            CHECK(e.setInteraction(own));
            CHECK(!e.oscAddressClaimed(native));
            direct.args = {QVariant(0.4), QVariant(0.4), QVariant(0.4), QVariant(0.9)};
            CHECK(server.handleMessage(direct) && std::abs(ch("a") - 0.9) < 1e-6);
            // A scalar of the same layer, not listened to, is still written directly
            CHECK(server.handleMessage({"/layer/Chataigne/opacity", "f", {0.5}}));
            e.setInteractions({});
        }

        // A whole color through the whole chain (audit P1/P2): each channel with its own target range, master,
        // curve and filter; pick-up on the whole color; the alpha without the master
        {
            e.setInteractions({});
            CHECK(e.setParameter(colorLayer, "param/color/a", 0.75));
            Interaction chain = Interaction::made(InteractionSource::Osc);
            chain.oscAddress = QStringLiteral("/chataigne/chain");
            chain.layer = colorLayer;
            chain.param = QStringLiteral("param/color");
            chain.takeover = Takeover::Jump;
            chain.srcMin = 0;
            chain.srcMax = 1;
            chain.dstMin = 0.2;
            chain.dstMax = 0.6;
            chain.id = e.addInteraction(chain);
            osc::Message cm;
            cm.address = chain.oscAddress;
            cm.types = QStringLiteral("fff");
            auto send = [&](double r, double g, double b) {
                cm.args = {QVariant(r), QVariant(g), QVariant(b)};
                server.handleMessage(cm);
                e.renderFrame();
            };
            auto rgbNear = [&](double r, double g, double b, double tol = 1e-6) {
                return std::abs(component("r") - r) < tol && std::abs(component("g") - g) < tol &&
                       std::abs(component("b") - b) < tol;
            };
            send(0.0, 0.5, 1.0); // the target range, per channel
            CHECK(rgbNear(0.2, 0.4, 0.6) && std::abs(component("a") - 0.75) < 1e-6);
            chain.dstMin = 0;
            chain.dstMax = 1;
            chain.master = 0.5;
            chain.colorMapAlpha = true;
            CHECK(e.setInteraction(chain));
            cm.types = QStringLiteral("ffff");
            cm.args = {QVariant(1.0), QVariant(1.0), QVariant(1.0), QVariant(1.0)};
            server.handleMessage(cm);
            e.renderFrame();
            CHECK(rgbNear(0.5, 0.5, 0.5) && std::abs(component("a") - 1.0) < 1e-6); // master on RGB, not on alpha
            chain.master = 1;
            chain.colorMapAlpha = false;
            chain.curveOn = true;
            chain.curve = Interaction::straightCurve();
            chain.curve.keys[0].v = 1; // inverted: 0 → 1, 1 → 0
            chain.curve.keys[1].v = 0;
            CHECK(e.setInteraction(chain));
            cm.types = QStringLiteral("fff");
            send(0.25, 0.5, 1.0); // the curve, per channel
            CHECK(rgbNear(0.75, 0.5, 0.0, 1e-3));
            // A filter: every channel smoothed by its own state, together
            chain.curveOn = false;
            chain.filterOn = true;
            chain.filter = InteractionFilter::WeightedAverage;
            chain.filterA = 1.0; // ~95% of the way in a second
            CHECK(e.setInteraction(chain)); // the filter changed: its state starts again
            send(0.0, 0.0, 0.0);
            CHECK(rgbNear(0.0, 0.0, 0.0, 1e-3));
            send(1.0, 1.0, 1.0);
            const double first = component("r");
            CHECK(first < 0.9 && std::abs(component("g") - first) < 1e-9 && std::abs(component("b") - first) < 1e-9);
            for (int k = 0; k < 10; ++k) { // no message any more: the filter keeps moving the color
                QThread::msleep(200);
                e.renderFrame();
            }
            CHECK(component("r") > 0.97 && component("r") > first && std::abs(component("g") - component("r")) < 1e-9);
            // Pick-up: the color moved elsewhere is held until every channel has met it
            chain.filterOn = false;
            chain.takeover = Takeover::PickUp;
            CHECK(e.setInteraction(chain));
            send(0.9, 0.9, 0.9);
            CHECK(e.setParameter(colorLayer, "param/color/r", 0.3) && e.setParameter(colorLayer, "param/color/g", 0.3) &&
                  e.setParameter(colorLayer, "param/color/b", 0.3)); // moved by something else (a snapshot)
            send(0.8, 0.8, 0.8);
            CHECK(rgbNear(0.3, 0.3, 0.3)); // not met: left alone
            send(0.3, 0.3, 0.8); // red and green met, not blue
            CHECK(rgbNear(0.3, 0.3, 0.3));
            send(0.2, 0.2, 0.1); // blue crossed it: the whole color is taken over
            CHECK(rgbNear(0.2, 0.2, 0.1));
            send(0.6, 0.7, 0.8); // and follows from now on
            CHECK(rgbNear(0.6, 0.7, 0.8));
            e.setInteractions({});
            // The color the checks below start from (the packed 'r' above)
            CHECK(e.setParameter(colorLayer, "param/color/r", 0x33 / 255.0) &&
                  e.setParameter(colorLayer, "param/color/g", 0x66 / 255.0) &&
                  e.setParameter(colorLayer, "param/color/b", 0xCC / 255.0) &&
                  e.setParameter(colorLayer, "param/color/a", 0x80 / 255.0));
        }

        // Existing scalar OSC argument mapping continues to work alongside
        // composite colors; only the selected component is changed.
        e.setInteractions({});
        Interaction green = Interaction::made(InteractionSource::Osc);
        green.oscAddress = QStringLiteral("/chataigne/components");
        green.oscArgument = 1;
        green.layer = colorLayer;
        green.param = QStringLiteral("param/color/g");
        green.takeover = Takeover::Jump;
        e.addInteraction(green);
        colorMessage.address = green.oscAddress;
        colorMessage.types = QStringLiteral("fff");
        colorMessage.args = {QVariant(0.2), QVariant(0.7), QVariant(0.4)};
        server.handleMessage(colorMessage);
        e.renderFrame();
        CHECK(std::abs(component("g") - 0.7) < 1e-6 &&
              std::abs(component("r") - 0x33 / 255.0) < 1e-6);

        // Internal: B's opacity follows A's, inverted
        e.setInteractions({});
        Interaction link = Interaction::made(InteractionSource::Internal);
        link.sourceLayer = a;
        link.sourceParam = "opacity";
        link.layer = b;
        link.param = "opacity";
        link.dstMin = 1;
        link.dstMax = 0;
        e.addInteraction(link);
        CHECK(e.setParameter(a, "opacity", 0.3));
        e.renderFrame();
        CHECK(std::abs(opacity(b) - 0.7) < 1e-6);
        CHECK(e.setParameter(a, "opacity", 0.9));
        e.renderFrame();
        CHECK(std::abs(opacity(b) - 0.1) < 1e-6);

        // Learn: the distinct sources heard, once settled (a key: when pressed); of one kind when asked
        e.setInteractionLearn(true);
        press("F5", 0);
        CHECK(e.takeLearnedInteractions(0).empty());
        midiCc(4, 21, 0.5);
        midiCc(4, 21, 0.6); // the same one again
        midiCc(4, 22, 0.1); // another, at the same time
        CHECK(e.takeLearnedInteractions(10000).empty()); // not settled yet
        std::vector<InteractionEvent> heard = e.takeLearnedInteractions(0);
        CHECK(heard.size() == 2 && heard[0].number == 21 && std::abs(heard[0].value - 0.6) < 1e-9 && heard[1].number == 22);
        CHECK(e.takeLearnedInteractions(0).empty());
        e.setInteractionLearn(true, InteractionSource::Keyboard); // keys only
        midiCc(4, 23, 0.5);
        press("F6", 1);
        heard = e.takeLearnedInteractions(0);
        CHECK(heard.size() == 1 && heard[0].source == InteractionSource::Keyboard && heard[0].address == "F6");
        e.setInteractionLearn(false);
        midiCc(4, 24, 0.5);
        CHECK(e.takeLearnedInteractions(0).empty() && !e.interactionLearning());
        e.renderFrame();

        // Saved with the project
        CHECK(e.saveProject(tmp + "/interactions.pinol", {}, &err));
        e.newProject();
        CHECK(e.interactions().empty());
        CHECK(e.loadProject(tmp + "/interactions.pinol", nullptr, &err));
        CHECK(e.interactions().size() == 1 && e.interactions()[0].source == InteractionSource::Internal &&
              e.interactions()[0].sourceLayer == a && e.interactions()[0].layer == b && e.interactions()[0].dstMin == 1);
        CHECK(e.addInteraction(Interaction::made(InteractionSource::Keyboard)) > e.interactions()[0].id);
    }
    // In the snapshots and the presets: each interaction's values are the composition's ("interaction/<id>/…")
    {
        e.newProject();
        const quint64 a = e.layerId(e.addLayer("A"));
        Interaction fader = Interaction::made(InteractionSource::Midi);
        fader.midiChannel = 1;
        fader.midiNumber = 7;
        fader.layer = a;
        fader.param = "opacity";
        fader.takeover = Takeover::Jump;
        const quint64 f = e.addInteraction(fader);
        const QString master = QStringLiteral("interaction/%1/master").arg(f), on = QStringLiteral("interaction/%1/enable").arg(f);
        bool inComposition = false; // not among the composition's values: reached by their address
        for (const ParamInfo &p : e.parameters(0)) inComposition = inComposition || p.path.startsWith("interaction/");
        ParamInfo mi;
        CHECK(!inComposition && e.parameterInfo(0, master, &mi) && mi.animatable && mi.label == "Interactions › Interaction 1 › Master Level");
        CHECK(e.parameterValue(0, master).toDouble() == 1.0 && e.parameterValue(0, on).toBool());
        auto value = [&](quint64 id) {
            for (const Interaction &x : e.interactions())
                if (x.id == id) return x;
            return Interaction();
        };
        // A preset holding its master level
        CHECK(e.setParameter(0, master, 0.4) && std::abs(value(f).master - 0.4) < 1e-9);
        Engine::Preset pr;
        pr.name = "Half";
        pr.fade = 0;
        pr.values.push_back(e.presetValue(0, master));
        pr.values.push_back(e.presetValue(0, QStringLiteral("interaction/%1/target/max").arg(f)));
        e.addPreset(pr);
        CHECK(e.setParameter(0, master, 1.0));
        e.recallPreset(e.presetCount() - 1);
        CHECK(std::abs(value(f).master - 0.4) < 1e-9);
        // The master level scales what it sends
        InteractionEvent ev;
        ev.source = InteractionSource::Midi;
        ev.channel = 1;
        ev.number = 7;
        ev.value = 1.0;
        e.feedInteraction(ev);
        e.renderFrame();
        CHECK(std::abs(e.parameterValue(a, "opacity").toDouble() - 0.4) < 1e-6);
        // A snapshot holds them all: off, and another master
        CHECK(e.setParameter(0, on, false) && e.setParameter(0, master, 0.7));
        Engine::Snapshot snap;
        snap.fade = 0;
        snap.layers = e.captureLayers();
        snap.composition = e.captureComposition();
        snap.interactions = e.captureInteractions();
        CHECK(!snap.composition.contains("interactions") && snap.interactions.size() == 1); // a part of their own
        CHECK(snap.interactions[0].toObject().value("name").toString() == "Interaction 1");
        e.addSnapshot(snap);
        CHECK(e.setParameter(0, on, true) && e.setParameter(0, master, 0.1));
        Interaction later = Interaction::made(InteractionSource::Keyboard); // made after the snapshot: switched off
        const quint64 k = e.addInteraction(later);
        e.recallSnapshot(e.snapshotCount() - 1);
        CHECK(!value(f).enabled && std::abs(value(f).master - 0.7) < 1e-9 && !value(k).enabled);
        CHECK(value(f).midiNumber == 7 && value(f).param == "opacity"); // its source and target stay
        // Off: the controller moves nothing
        ev.value = 0.2;
        e.feedInteraction(ev);
        e.renderFrame();
        CHECK(std::abs(e.parameterValue(a, "opacity").toDouble() - 0.4) < 1e-6);
        // An interaction can drive another's master level (Internal)
        Interaction link = Interaction::made(InteractionSource::Internal);
        link.sourceLayer = a;
        link.sourceParam = "opacity";
        link.layer = 0;
        link.param = master;
        e.addInteraction(link);
        CHECK(e.setParameter(a, "opacity", 0.25));
        e.renderFrame();
        CHECK(std::abs(value(f).master - 0.25) < 1e-6);
        e.removeInteraction(e.interactions().back().id);

        // Names: their own, unique, editable; their address in OSC
        Interaction renamed = value(f);
        renamed.name = "Fader 1";
        CHECK(e.setInteraction(renamed) && value(f).name == "Fader 1");
        Interaction twin = value(k);
        twin.name = "Fader 1";
        CHECK(e.setInteraction(twin) && value(k).name == "Fader 1 2"); // taken: another
        CHECK(OscNamespace::addressOf(&e, 0, master) == "/interaction/Fader_1/master");
        CHECK(OscNamespace::addressOf(&e, a, "opacity").endsWith("/A/opacity"));
        {
            OscServer server(&e);
            osc::Message m;
            m.address = "/interaction/Fader_1/master";
            m.args = {QVariant(0.6)};
            CHECK(server.handleMessage(m) && std::abs(value(f).master - 0.6) < 1e-9);
        }

        // Faded as any value: a snapshot's master level, then a preset's, in their time
        e.setFadesManual(true);
        CHECK(e.setParameter(0, master, 0.0));
        Engine::Snapshot slow;
        slow.fade = 2;
        slow.interactions = e.captureInteractions();
        QJsonObject state = slow.interactions[0].toObject();
        QJsonObject params = state.value("params").toObject();
        params["master"] = 1.0;
        state["params"] = params;
        slow.interactions[0] = state;
        e.addSnapshot(slow);
        e.recallSnapshot(e.snapshotCount() - 1);
        e.advanceFades(1.0);
        const double half = value(f).master;
        e.advanceFades(1.5);
        CHECK(half > 0.2 && half < 0.8 && std::abs(value(f).master - 1.0) < 1e-9);
        Engine::Preset down;
        down.fade = 1;
        down.values.push_back({0, master, QJsonValue(0.0), -1, QString()});
        e.addPreset(down);
        e.recallPreset(e.presetCount() - 1);
        e.advanceFades(0.5);
        CHECK(value(f).master > 0.1 && value(f).master < 0.9);
        e.advanceFades(1.0);
        CHECK(std::abs(value(f).master) < 1e-9);
        e.setFadesManual(false);

        // Its source, its target, its name and its curve are values too (presets, snapshots, OSC)
        const QString pre = QStringLiteral("interaction/%1/").arg(f);
        CHECK(e.parameterValue(0, pre + "source/kind").toString() == "midi");
        CHECK(e.parameterValue(0, pre + "target").toString() == QStringLiteral("%1:opacity").arg(a));
        CHECK(e.setParameter(0, pre + "source/midi/number", 9) && value(f).midiNumber == 9);
        CHECK(e.setParameter(0, pre + "target", QStringLiteral("%1:scale").arg(a)) && value(f).param == "scale");
        CHECK(e.setParameter(0, pre + "name", "Knob") && value(f).name == "Knob");
        CHECK(e.setParameter(0, pre + "source/kind", "osc") && value(f).source == InteractionSource::Osc);
        CHECK(e.setParameter(0, pre + "source/osc/address", "/knob") && value(f).oscAddress == "/knob");
        CHECK(e.setParameter(0, pre + "source/kind", "midi") && e.setParameter(0, pre + "source/midi/number", 7) &&
              e.setParameter(0, pre + "target", QStringLiteral("%1:opacity").arg(a)) && e.setParameter(0, pre + "name", "Fader 1"));

        // Recalled when it no longer exists: made again, the same, with its source and its target
        Engine::Snapshot whole;
        whole.fade = 0;
        whole.interactions = e.captureInteractions();
        e.addSnapshot(whole);
        e.removeInteraction(f);
        CHECK(value(f).id == 0);
        e.recallSnapshot(e.snapshotCount() - 1);
        CHECK(value(f).id == f && value(f).name == "Fader 1" && value(f).source == InteractionSource::Midi &&
              value(f).midiChannel == 1 && value(f).midiNumber == 7 && value(f).layer == a && value(f).param == "opacity");
        ev.value = 0.5;
        ev.number = 7;
        e.feedInteraction(ev);
        e.renderFrame();
        CHECK(std::abs(e.parameterValue(a, "opacity").toDouble()) < 1.0); // it drives its target again
        // A preset of its source and target makes it again too
        Engine::Preset again;
        again.fade = 0;
        for (const char *key : {"source/kind", "source/midi/channel", "source/midi/number", "target", "name"})
            again.values.push_back(e.presetValue(0, pre + key));
        e.addPreset(again);
        e.removeInteraction(f);
        e.recallPreset(e.presetCount() - 1);
        CHECK(value(f).id == f && value(f).name == "Fader 1" && value(f).source == InteractionSource::Midi &&
              value(f).midiNumber == 7 && value(f).layer == a && value(f).param == "opacity");
        const quint64 next = e.addInteraction(Interaction::made(InteractionSource::Keyboard));
        CHECK(next > f && next != k); // ids go on after the one made again
        // Made after a snapshot: switched off by its recall (one holding none switches them all off)
        CHECK(e.setParameter(0, pre + "enable", true));
        Engine::Snapshot none;
        none.fade = 0;
        e.addSnapshot(none); // holds no interaction
        e.recallSnapshot(e.snapshotCount() - 1);
        CHECK(!value(f).enabled && !value(next).enabled);
        // The same once saved and opened again (a snapshot holding none stores no part for them)
        CHECK(e.setParameter(0, pre + "enable", true));
        CHECK(e.saveProject(tmp + "/interactions-off.pinol", {}, &err));
        CHECK(e.loadProject(tmp + "/interactions-off.pinol", nullptr, &err));
        CHECK(value(f).enabled);
        e.recallSnapshot(e.snapshotCount() - 1);
        CHECK(!value(f).enabled);
    }
    }

    if (area("layersource")) {
    // 12. Copy / paste of layer parameters, and a layer used as the source of another (pre-FX / post-FX)
    {
        e.newProject();
        const QString isfDir = root + "/../isf";
        const quint64 idA = e.layerId(e.addLayer("A"));
        CHECK(e.setLayerIsf(e.indexOfId(idA), isfDir + "/generators/TestPattern.fs", &err));
        // B is above A in the list: the render pass must still render A first (B's source)
        const quint64 idB = e.layerId(e.addLayer("B", V + 0));
        CHECK(e.setLayerIsf(e.indexOfId(idB), isfDir + "/generators/SolidColor.fs", &err));
        auto A = [&] { return e.indexOfId(idA); };
        auto B = [&] { return e.indexOfId(idB); };
        CHECK(A() == V + 1 && B() == V + 0);

        // A: a distinctive value in every part
        {
            Engine::Lock lk(&e.mutex());
            Layer *l = e.layer(A());
            l->roi = QRectF(QPointF(0.25, 0.1), QPointF(0.75, 0.6));
            l->color.temp = 1000;
            l->color.tintOn = false;
            l->opacity = 0.25f;
            l->blend = BlendMode::Multiply;
            l->mapping.setCorner(1, QPointF(0.8, 0.2));
        }
        CHECK(e.addEffect(A(), isfDir + "/effects/Hue.fs", &err) == 0);
        const QJsonObject clip = e.layerJson(A());

        // One part at a time: nothing else moves
        CHECK(e.applyLayerParts(B(), clip, Engine::PartRoi));
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(B());
            CHECK(l->roi == QRectF(QPointF(0.25, 0.1), QPointF(0.75, 0.6)));
            CHECK(l->color.temp == 0.f && l->opacity == 1.f && l->effects.empty());
            CHECK(QLineF(l->mapping.corner(1), QPointF(1, 0)).length() < 1e-9);
            CHECK(l->type == SourceType::Isf && l->sourcePath.endsWith("SolidColor.fs")); // its own media kept
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartColor));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->color.temp == 1000.f && !e.layer(B())->color.tintOn);
            CHECK(e.layer(B())->opacity == 1.f);
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartSpatial | Engine::PartCompositing));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(QLineF(e.layer(B())->mapping.corner(1), QPointF(0.8, 0.2)).length() < 1e-9);
            CHECK(e.layer(B())->opacity == 0.25f && e.layer(B())->blend == BlendMode::Multiply);
            CHECK(e.layer(B())->effects.empty());
        }
        CHECK(e.applyLayerParts(B(), clip, Engine::PartEffects));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->effects.size() == 1 && e.layer(B())->effects[0]->name() == "Hue");
            CHECK(e.layer(B())->type == SourceType::Isf && e.layer(B())->sourcePath.endsWith("SolidColor.fs"));
        }
        // Everything, the source included: B becomes a copy of A, with its own name and place
        CHECK(e.applyLayerParts(B(), clip, Engine::PartAll));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->name == "B" && e.layer(B())->sourcePath.endsWith("TestPattern.fs"));
        }
        CHECK(B() == V + 0 && e.layerId(V + 0) == idB);
        // A locked layer refuses a paste
        {
            Engine::Lock lk(&e.mutex());
            e.layer(B())->locked = true;
        }
        CHECK(!e.applyLayerParts(B(), clip, Engine::PartRoi));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(B())->locked = false;
            e.layer(B())->roi = Layer::fullRoi();
        }

        // The other parts, one at a time: shape masks, routing in the viewports, animations, transport; nothing else
        {
            const quint64 vp = e.mainViewportId();
            e.setLayerMasks(idA, {Mask::ellipse(QRectF(0.2, 0.2, 0.5, 0.5))});
            {
                Engine::Lock lk(&e.mutex());
                e.layer(A())->viewportOpacity[vp] = 0.4f;
            }
            Engine::Animation pulse = e.makeLayerAnim(idA, "opacity", int(Engine::AnimWave::Sine));
            e.setLayerAnims(idA, {pulse});
            const QJsonObject more = e.layerJson(A());
            unsigned rev = 0;
            {
                Engine::Lock lk(&e.mutex());
                rev = e.layer(B())->masksRevision;
            }
            CHECK(e.applyLayerParts(B(), more, Engine::PartMasks));
            {
                Engine::Lock lk(&e.mutex());
                const Layer *l = e.layer(B());
                CHECK(l->masks.size() == 1 && l->masks[0].shape == Mask::Shape::Ellipse && l->masksRevision != rev);
                CHECK(l->viewportOpacity.empty() && l->anims.empty()); // not the other parts
            }
            CHECK(e.applyLayerParts(B(), more, Engine::PartViewports));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(std::abs(e.layer(B())->opacityIn(vp) - 0.4f) < 1e-6 && e.layer(B())->anims.empty());
            }
            CHECK(e.applyLayerParts(B(), more, Engine::PartAnimations));
            {
                Engine::Lock lk(&e.mutex());
                CHECK(e.layer(B())->anims.size() == 1 && e.layer(B())->anims[0].tracks.front().param == "opacity" &&
                      e.layer(B())->anims[0].tracks.front().layer == idB);
            }
            CHECK(Engine::partOf("in") == Engine::PartTransport && Engine::partOf("volume") == Engine::PartTransport &&
                  Engine::partOf("text/content") == Engine::PartSource && (Engine::PartAll & Engine::PartMasks));
            // Cleared again for what follows
            e.setLayerMasks(idA, {});
            e.setLayerMasks(idB, {});
            e.setLayerAnims(idA, {});
            e.setLayerAnims(idB, {});
            Engine::Lock lk(&e.mutex());
            e.layer(A())->viewportOpacity.clear();
            e.layer(B())->viewportOpacity.clear();
        }
        // Transport alone between two videos: the in / out points and the speed, each keeping its own media
        {
            const int v1 = e.addLayer("T1"), v2 = e.addLayer("T2");
            const quint64 t1 = e.layerId(v1), t2 = e.layerId(v2);
            CHECK(e.setLayerVideo(e.indexOfId(t1), root + "/media/h264.mp4", &err) &&
                  e.setLayerVideo(e.indexOfId(t2), root + "/media/av.mp4", &err));
            e.setLayerInOut(e.indexOfId(t1), 0.5, 1.5);
            e.setLayerSpeed(e.indexOfId(t1), 0.5);
            CHECK(e.applyLayerParts(e.indexOfId(t2), e.layerJson(e.indexOfId(t1)), Engine::PartTransport));
            {
                Engine::Lock lk(&e.mutex());
                const Layer *l = e.layer(e.indexOfId(t2));
                CHECK(std::abs(l->inPoint - 0.5) < 1e-6 && std::abs(l->outPoint - 1.5) < 1e-6 && std::abs(l->speed - 0.5) < 1e-6);
                CHECK(l->sourcePath.endsWith("av.mp4"));
            }
            e.removeLayer(e.indexOfId(t1));
            e.removeLayer(e.indexOfId(t2));
        }

        // B's picture is A's, after A's effect chain
        CHECK(e.setLayerSourceLayer(B(), idA, LayerTap::PostFx, &err));
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->type == SourceType::Layer && e.layer(B())->rawTex != 0);
            CHECK(e.layer(B())->rawTex == e.layer(A())->finalTex);
            CHECK(e.layer(A())->preFxTex != e.layer(A())->finalTex); // A has a ROI, a color and an effect
        }
        // ... or before it
        CHECK(e.setLayerTap(B(), LayerTap::PreFx));
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(B())->rawTex == e.layer(A())->preFxTex);
        }
        // Refused: itself, and anything that would feed the picture back on itself
        CHECK(!e.setLayerSourceLayer(B(), idB, LayerTap::PostFx, &err) && !err.isEmpty());
        CHECK(!e.setLayerSourceLayer(A(), idB, LayerTap::PostFx, &err) && !err.isEmpty());
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layerDependsOn(idB, idA) && !e.layerDependsOn(idA, idB));
        }
        // A group has no source of its own
        const int g = e.addGroup("G", V + 0);
        CHECK(!e.setLayerSourceLayer(g, idA, LayerTap::PostFx, &err));
        // A hidden group used as a source is rendered all the same
        const quint64 idG = e.layerId(g);
        e.setStructure(tree::intoGroup(e.structure(), {idA}, idG));
        CHECK(e.setLayerSourceLayer(B(), idG, LayerTap::PostFx, &err));
        {
            Engine::Lock lk(&e.mutex());
            e.layer(e.indexOfId(idG))->enabled = false;
        }
        e.renderFrame();
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(e.indexOfId(idG))->finalTex != 0 && e.layer(e.indexOfId(idG))->referenced);
            CHECK(e.layer(B())->rawTex == e.layer(e.indexOfId(idG))->finalTex);
        }
        // A member of a group cannot use its own group: the group is made of it
        CHECK(!e.setLayerSourceLayer(e.indexOfId(idA), idG, LayerTap::PostFx, &err));

        // Saved and read back
        CHECK(e.saveProject(tmp + "/layersrc.pinol", {}, &err));
        const QJsonObject saved = readJson(tmp + "/layersrc.pinol");
        err.clear(); // the refusals above left their message there
        CHECK(e.loadProject(tmp + "/layersrc.pinol", nullptr, &err) && err.isEmpty());
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(e.indexOfId(idB));
            CHECK(l && l->type == SourceType::Layer && l->sourceLayer == idG && l->sourceTap == LayerTap::PostFx);
        }
        CHECK(e.saveProject(tmp + "/layersrc2.pinol", {}, &err));
        CHECK(saved.value("layers") == readJson(tmp + "/layersrc2.pinol").value("layers"));

        // The layer it used is gone, or a hand-edited project loops: the source is dropped, with a warning
        e.removeLayer(e.indexOfId(idG)); // removes the group and its member
        QStringList warnings;
        e.fixLayerReferences(&warnings);
        CHECK(warnings.size() == 1 && warnings[0].contains("gone"));
        {
            Engine::Lock lk(&e.mutex());
            CHECK(e.layer(e.indexOfId(idB))->type == SourceType::None);
        }
        e.newProject();
    }
    } // layersource

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL OK", failures);
    e.shutdown();
    return failures ? 1 : 0;
}
