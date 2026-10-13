// Playback benchmark: several videos at once, as the show plays them.
//
//   pinol_bench [--seconds S] [--fps F] [--comp WxH] [--decode] [--copy] [--software] file...
//
// Default: every file in a layer of one composition (a grid), played by the render thread for S seconds;
// for each layer, the frames it showed per second against the file's own rate, and the rate of the render.
// --decode: the decoders alone (no GPU), each file's frames fetched as fast as they come.
// --copy: hardware frames copied to upload buffers, as before zero copy (macOS), to compare.
// --software: no hardware decoding.
#include "Engine.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSurfaceFormat>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static int decodeOnly(const QStringList &files, double seconds)
{
    std::vector<std::unique_ptr<VideoDecoder>> dec;
    for (const QString &f : files) {
        auto d = std::make_unique<VideoDecoder>();
        QString err;
        if (!d->open(f, &err)) {
            std::printf("%s: %s\n", qPrintable(f), qPrintable(err));
            return 1;
        }
        Timeline t;
        t.mode = Timeline::Loop;
        d->setTimeline(t);
        d->seek(0);
        dec.push_back(std::move(d));
    }
    std::vector<VideoFrame> frames(dec.size());
    std::vector<int> got(dec.size(), 0);
    QElapsedTimer tm;
    tm.start();
    while (tm.elapsed() < seconds * 1000) {
        bool any = false;
        for (size_t i = 0; i < dec.size(); ++i)
            if (dec[i]->fetch((got[i] + 0.5) / dec[i]->fps(), frames[i])) {
                ++got[i];
                any = true;
            }
        if (!any) QThread::usleep(200);
    }
    const double s = tm.elapsed() / 1000.0;
    std::printf("Decoding only, %.1f s, %d CPU threads\n", s, QThread::idealThreadCount());
    for (size_t i = 0; i < dec.size(); ++i) {
        std::printf("  %-34s %-26s %5dx%-5d %6.1f frames/s (file: %.2f)\n", qPrintable(QFileInfo(files[int(i)]).fileName()),
                    qPrintable(dec[i]->codecName()), dec[i]->width(), dec[i]->height(), got[i] / s, dec[i]->fps());
        const VideoDecoder::Metrics m = dec[i]->metrics();
        std::printf("      decode p95/max %.2f/%.2f ms; decoded %llu; budget holds %llu; threads %d\n",
                    m.decodeTimeP95Ms, m.decodeTimeMaxMs,
                    static_cast<unsigned long long>(m.decodedFrames),
                    static_cast<unsigned long long>(m.budgetHolds), dec[i]->decodeThreads());
    }
    return 0;
}

int main(int argc, char **argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);

    double seconds = 10, fps = 60;
    QSize comp(3840, 2160);
    bool decode = false, copy = false, software = false;
    QStringList files;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == "--seconds" && i + 1 < args.size()) seconds = args[++i].toDouble();
        else if (a == "--fps" && i + 1 < args.size()) fps = args[++i].toDouble();
        else if (a == "--comp" && i + 1 < args.size()) {
            const QStringList wh = args[++i].split('x');
            if (wh.size() == 2) comp = QSize(wh[0].toInt(), wh[1].toInt());
        } else if (a == "--decode") decode = true;
        else if (a == "--copy") copy = true;
        else if (a == "--software") software = true;
        else files << a;
    }
    if (files.isEmpty()) {
        std::printf("usage: pinol_bench [--seconds S] [--fps F] [--comp WxH] [--decode] [--copy] [--software] file...\n");
        return 1;
    }
    if (software) VideoDecoder::setHardwareDecoding(false);
    if (decode) return decodeOnly(files, seconds);

    Engine e;
    QString err;
    if (!e.initialize(&err)) {
        std::printf("Engine: %s\n", qPrintable(err));
        return 1;
    }
    if (copy) VideoDecoder::setZeroCopy(false); // after the engine set it
    std::printf("Hardware decoding: %s, zero copy: %s\n", VideoDecoder::hardwareDecoding() ? "on" : "off",
                VideoDecoder::zeroCopy() ? "on" : "off");
    e.setCompositionSize(comp);
    Engine::RenderSettings rs;
    rs.frameRate = fps;
    rs.samples = 0;
    rs.mipmaps = 0;
    e.setRenderSettings(rs);
    const int n = int(files.size()), cols = int(std::ceil(std::sqrt(double(n)))), rows = (n + cols - 1) / cols;
    std::vector<quint64> ids;
    for (int k = 0; k < n; ++k) {
        const int i = e.addLayer(QFileInfo(files[k]).fileName(), e.layerCount());
        if (!e.setLayerVideo(i, files[k], &err)) {
            std::printf("%s: %s\n", qPrintable(files[k]), qPrintable(err));
            return 1;
        }
        Engine::Lock lk(&e.mutex());
        Layer *l = e.layer(i);
        const double x = double(k % cols) / cols, y = double(k / cols) / rows, w = 1.0 / cols, h = 1.0 / rows;
        l->mapping.setCorner(0, QPointF(x, y));
        l->mapping.setCorner(1, QPointF(x + w, y));
        l->mapping.setCorner(2, QPointF(x + w, y + h));
        l->mapping.setCorner(3, QPointF(x, y + h));
        ids.push_back(l->id);
    }
    if (!e.start()) {
        std::printf("No render thread on this platform\n");
        return 1;
    }
    QThread::msleep(1500); // decoders filled, buffers handed over
    std::vector<quint64> start(static_cast<size_t>(n), 0);
    for (int k = 0; k < n; ++k) start[size_t(k)] = e.videoFramesShown(e.indexOfId(ids[size_t(k)]));
    const quint64 frames0 = e.frameCount();
    QElapsedTimer tm;
    tm.start();
    double fpsSum = 0;
    int fpsCount = 0;
    std::vector<double> waits; // how long the interface waits for the engine's lock (ms), sampled as it would
    while (tm.elapsed() < seconds * 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        e.acknowledgeFrame();
        QElapsedTimer w;
        w.start();
        { Engine::Lock lk(&e.mutex()); }
        waits.push_back(w.nsecsElapsed() / 1e6);
        QThread::msleep(7);
        fpsSum += e.fps();
        ++fpsCount;
    }
    const double s = tm.elapsed() / 1000.0;
    const double renderFps = (e.frameCount() - frames0) / s;
    std::printf("Playing together: %d layers, composition %dx%d, %.1f s, render asked %.0f fps, got %.1f fps\n", n,
                comp.width(), comp.height(), s, fps, renderFps);
    if (!waits.empty()) {
        std::sort(waits.begin(), waits.end());
        double mean = 0;
        for (double x : waits) mean += x;
        mean /= double(waits.size());
        std::printf("Interface waiting for the lock: mean %.2f ms, p99 %.2f ms, max %.2f ms (%zu samples)\n", mean,
                    waits[std::min(waits.size() - 1, size_t(double(waits.size()) * 0.99))], waits.back(), waits.size());
    }
    bool allGood = true;
    for (int k = 0; k < n; ++k) {
        const int i = e.indexOfId(ids[size_t(k)]);
        const quint64 shown = e.videoFramesShown(i) - start[size_t(k)];
        QString codec, format;
        double fileFps = 0;
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(i);
            codec = l->video->codecName();
            fileFps = l->video->fps();
            format = l->frame.layout ? l->frame.layout->description : QString();
        }
        const double rate = shown / s, want = std::min(fileFps, renderFps);
        const bool good = rate >= want * 0.95;
        allGood = allGood && good;
        VideoDecoder::Metrics m;
        uint64_t uploads = 0, fast = 0;
        {
            Engine::Lock lk(&e.mutex());
            const Layer *l = e.layer(i);
            if (l && l->video) m = l->video->metrics();
            if (l && l->videoTex) {
                uploads = l->videoTex->uploads();
                fast = l->videoTex->stagedUploads() + l->videoTex->surfaceUploads();
            }
        }
        std::printf("  %-34s %-26s %6.1f frames/s of %.2f  %s  [%s]\n", qPrintable(QFileInfo(files[k]).fileName()),
                    qPrintable(codec), rate, fileFps, good ? "ok" : "DROPS", qPrintable(format));
        std::printf("      decode p95/max %.2f/%.2f ms; late %llu; skipped %llu; fetch misses %llu; queue high %llu\n",
                    m.decodeTimeP95Ms, m.decodeTimeMaxMs,
                    static_cast<unsigned long long>(m.lateFrames),
                    static_cast<unsigned long long>(m.skippedFrames),
                    static_cast<unsigned long long>(m.fetchMisses),
                    static_cast<unsigned long long>(m.queueHighWatermark));
        std::printf("      decoded %llu; budget holds %llu; decode total %.1f ms; slow uploads %llu of %llu\n",
                    static_cast<unsigned long long>(m.decodedFrames),
                    static_cast<unsigned long long>(m.budgetHolds), m.decodeTimeTotalMs,
                    static_cast<unsigned long long>(uploads - std::min(uploads, fast)),
                    static_cast<unsigned long long>(uploads));
    }
    e.stop();
    std::printf(allGood ? "Every layer kept its rate.\n" : "Some layers dropped frames.\n");
    (void)fpsSum;
    return allGood ? 0 : 2;
}
