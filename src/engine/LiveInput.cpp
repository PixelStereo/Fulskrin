// Live inputs (LiveInput.h): cameras through Qt Multimedia, NDI and OMT through their libraries loaded at run time,
// Syphon in LiveInputSyphon.mm.
#include "LiveInput.h"
#include "NetVideo.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#ifdef FULSKRIN_HAS_CAMERA
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoFrame>
#include <QVideoSink>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif
#endif

#ifdef FULSKRIN_HAS_SYPHON
std::vector<LiveInputInfo> listSyphonInputs();
std::unique_ptr<LiveInput> createSyphonInput(const QString &name, QString *err);
#endif

namespace {

QString part(const QString &uri, int k) { return uri.section(QLatin1Char(':'), k, k); }
QString rest(const QString &uri) { return uri.section(QLatin1Char(':'), 2); } // after "input:<kind>:"

// The newest frame, from a thread of its own, taken by the render thread
class LatestFrame
{
public:
    // `rows` of `stride` bytes, from the top row down; bgra: blue first (swapped to RGBA here). Stored bottom row first,
    // as the still images and the other textures are (Texture2D expects it), so a live picture is not upside down.
    void put(const uint8_t *data, int w, int h, int stride, bool bgra)
    {
        if (!data || w <= 0 || h <= 0) return;
        QMutexLocker lk(&m_mutex);
        m_rgba.resize(qsizetype(w) * h * 4);
        uchar *dst = reinterpret_cast<uchar *>(m_rgba.data());
        for (int y = 0; y < h; ++y) {
            const uint8_t *s = data + qsizetype(y) * stride;
            uchar *d = dst + qsizetype(h - 1 - y) * w * 4;
            if (!bgra) {
                std::memcpy(d, s, size_t(w) * 4);
            } else {
                for (int x = 0; x < w; ++x, s += 4, d += 4) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = s[3];
                }
            }
        }
        m_w = w;
        m_h = h;
        m_fresh = true;
    }
    bool take(Texture2D &tex)
    {
        QMutexLocker lk(&m_mutex);
        if (!m_fresh) return false;
        m_fresh = false;
        tex.upload(m_rgba.constData(), m_w, m_h);
        return true;
    }

private:
    QMutex m_mutex;
    QByteArray m_rgba;
    int m_w = 0, m_h = 0;
    bool m_fresh = false;
};

// --- A solid color (tests)
class TestInput : public LiveInput
{
public:
    explicit TestInput(const QColor &c)
    {
        QImage img(64, 36, QImage::Format_RGBA8888);
        img.fill(c);
        m_frame.put(img.constBits(), img.width(), img.height(), int(img.bytesPerLine()), false);
    }
    bool update(Texture2D &tex) override { return m_frame.take(tex); }

private:
    LatestFrame m_frame;
};

// --- NDI
class NdiInput : public LiveInput
{
public:
    NdiInput(ndi::Lib *lib, const QString &name) : m_lib(lib), m_name(name.toUtf8())
    {
        m_thread = std::thread([this] { run(); });
    }
    ~NdiInput() override
    {
        m_quit = true;
        if (m_thread.joinable()) m_thread.join();
    }
    bool update(Texture2D &tex) override { return m_frame.take(tex); }
    QString status() const override { return m_connected ? QString() : QStringLiteral("Waiting for the NDI source"); }

private:
    void run()
    {
        ndi::RecvCreateV3 c{};
        c.source_to_connect_to.p_ndi_name = m_name.constData();
        c.color_format = ndi::kRecvRGBX_RGBA;
        c.bandwidth = ndi::kRecvBandwidthHighest;
        c.allow_video_fields = false;
        c.p_ndi_recv_name = "Fulskrin";
        void *recv = m_lib->recv_create_v3(&c);
        if (!recv) return;
        while (!m_quit) {
            ndi::VideoFrameV2 v{};
            if (m_lib->recv_capture_v2(recv, &v, nullptr, nullptr, 100) != ndi::kFrameVideo) continue;
            m_connected = true;
            const bool bgra = v.FourCC == ndi::kBGRA || v.FourCC == ndi::fourcc('B', 'G', 'R', 'X');
            if (v.FourCC == ndi::kRGBA || v.FourCC == ndi::kRGBX || bgra)
                m_frame.put(v.p_data, v.xres, v.yres, v.line_stride_in_bytes, bgra);
            m_lib->recv_free_video_v2(recv, &v);
        }
        m_lib->recv_destroy(recv);
    }
    ndi::Lib *m_lib;
    QByteArray m_name;
    LatestFrame m_frame;
    std::atomic<bool> m_quit{false}, m_connected{false};
    std::thread m_thread;
};

// The NDI sources on the network: one finder for the program's life (it keeps listening)
std::vector<LiveInputInfo> ndiSources(const QString &folder)
{
    std::vector<LiveInputInfo> out;
    ndi::Lib &lib = ndi::lib(folder);
    if (!lib.canReceive()) return out;
    static void *finder = nullptr;
    static QMutex mutex;
    QMutexLocker lk(&mutex);
    if (!finder) {
        const ndi::FindCreate f{true, nullptr, nullptr};
        finder = lib.find_create_v2(&f);
        if (!finder) return out;
    }
    uint32_t n = 0;
    const ndi::Source *s = lib.find_get_current_sources(finder, &n);
    for (uint32_t k = 0; s && k < n; ++k) {
        const QString name = QString::fromUtf8(s[k].p_ndi_name);
        if (!name.isEmpty()) out.push_back({QStringLiteral("input:ndi:") + name, name, QStringLiteral("NDI")});
    }
    return out;
}

// --- OMT
class OmtInput : public LiveInput
{
public:
    OmtInput(omt::Lib *lib, const QString &address) : m_lib(lib), m_address(address.toUtf8())
    {
        m_thread = std::thread([this] { run(); });
    }
    ~OmtInput() override
    {
        m_quit = true;
        if (m_thread.joinable()) m_thread.join();
    }
    bool update(Texture2D &tex) override { return m_frame.take(tex); }
    QString status() const override { return m_connected ? QString() : QStringLiteral("Waiting for the OMT source"); }

private:
    void run()
    {
        void *recv = m_lib->receive_create(m_address.constData(), omt::kVideo, omt::kPreferBGRA, omt::kReceiveDefault);
        if (!recv) return;
        while (!m_quit) {
            const omt::MediaFrame *f = m_lib->receive(recv, omt::kVideo, 100);
            if (!f || f->Type != omt::kVideo || f->Codec != omt::kBGRA) continue;
            m_connected = true;
            m_frame.put(static_cast<const uint8_t *>(f->Data), f->Width, f->Height, f->Stride, true);
        }
        m_lib->receive_destroy(recv);
    }
    omt::Lib *m_lib;
    QByteArray m_address;
    LatestFrame m_frame;
    std::atomic<bool> m_quit{false}, m_connected{false};
    std::thread m_thread;
};

std::vector<LiveInputInfo> omtSources(const QString &folder)
{
    std::vector<LiveInputInfo> out;
#if defined(Q_OS_LINUX) // libomt's discovery needs Avahi, and aborts without it
    if (!QFile::exists(QStringLiteral("/run/avahi-daemon/socket")) && !QFile::exists(QStringLiteral("/var/run/avahi-daemon/socket")))
        return out;
#endif
    omt::Lib &lib = omt::lib(folder);
    if (!lib.canReceive()) return out;
    int n = 0;
    char **a = lib.discovery_getaddresses(&n);
    for (int k = 0; a && k < n; ++k) {
        if (!a[k]) continue;
        const QString address = QString::fromUtf8(a[k]);
        out.push_back({QStringLiteral("input:omt:") + address, address, QStringLiteral("OMT")});
    }
    return out;
}

#ifdef FULSKRIN_HAS_CAMERA
// The format a camera is opened in. Left to itself, the system may pick a square one (the cameras with Center
// Stage, the iPhone's Continuity Camera offer them): the whole picture is wanted, so a wide format (not square),
// at least 24 fps, as large as possible up to 4K, then the highest rate up to 60.
static QCameraFormat bestCameraFormat(const QCameraDevice &device)
{
    QCameraFormat best;
    double bestScore = -1;
    for (const QCameraFormat &f : device.videoFormats()) {
        const QSize s = f.resolution();
        if (s.isEmpty()) continue;
        const bool wide = double(s.width()) / double(s.height()) > 1.2;
        const double fps = std::min(double(f.maxFrameRate()), 60.0);
        const double area = std::min(double(s.width()) * double(s.height()), 3840.0 * 2160.0);
        const double score = (wide ? 1e15 : 0) + (fps >= 24 ? 1e14 : 0) + area * 100 + fps;
        if (score > bestScore) {
            bestScore = score;
            best = f;
        }
    }
    return best;
}

// --- A camera: Qt Multimedia, in the application's thread; each frame converted to RGBA there
class CameraInput : public LiveInput
{
public:
    explicit CameraInput(const QCameraDevice &device)
    {
        m_holder = new QObject;
        QObject *holder = m_holder;
        auto frame = m_frame;
        auto status = m_status;
        auto start = [holder, device, frame, status] {
            auto *camera = new QCamera(device, holder);
            const QCameraFormat format = bestCameraFormat(device);
            if (!format.isNull()) {
                camera->setCameraFormat(format);
                qInfo("Camera %s: %dx%d, %.0f fps", qPrintable(device.description()), format.resolution().width(),
                      format.resolution().height(), double(format.maxFrameRate()));
            }
            auto *session = new QMediaCaptureSession(holder);
            auto *sink = new QVideoSink(holder);
            session->setCamera(camera);
            session->setVideoSink(sink);
            QObject::connect(sink, &QVideoSink::videoFrameChanged, holder, [frame](const QVideoFrame &f) {
                const QImage img = f.toImage().convertToFormat(QImage::Format_RGBA8888);
                if (!img.isNull()) frame->put(img.constBits(), img.width(), img.height(), int(img.bytesPerLine()), false);
            });
            QObject::connect(camera, &QCamera::errorOccurred, holder, [status](QCamera::Error, const QString &e) {
                QMutexLocker lk(&status->mutex);
                status->text = e;
            });
            camera->start();
        };
        // Its objects live in the application's thread (the one with an event loop)
        QObject *app = QCoreApplication::instance();
        if (app) holder->moveToThread(app->thread());
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (app) {
            QMetaObject::invokeMethod(app, [holder, start, status] {
                QCameraPermission permission;
                switch (qApp->checkPermission(permission)) {
                case Qt::PermissionStatus::Granted: start(); break;
                case Qt::PermissionStatus::Undetermined:
                    qApp->requestPermission(permission, holder, [start, status](const QPermission &p) {
                        if (p.status() == Qt::PermissionStatus::Granted) start();
                        else {
                            QMutexLocker lk(&status->mutex);
                            status->text = QStringLiteral("Camera access refused (System Settings › Privacy)");
                        }
                    });
                    break;
                case Qt::PermissionStatus::Denied: {
                    QMutexLocker lk(&status->mutex);
                    status->text = QStringLiteral("Camera access refused (System Settings › Privacy)");
                    break;
                }
                }
            });
        }
#else
        if (app) QMetaObject::invokeMethod(app, start);
#endif
    }
    ~CameraInput() override { m_holder->deleteLater(); } // stopped and released in its own thread
    bool update(Texture2D &tex) override { return m_frame->take(tex); }
    QString status() const override
    {
        QMutexLocker lk(&m_status->mutex);
        return m_status->text;
    }

private:
    struct Status {
        QMutex mutex;
        QString text;
    };
    QObject *m_holder;
    std::shared_ptr<LatestFrame> m_frame = std::make_shared<LatestFrame>();
    std::shared_ptr<Status> m_status = std::make_shared<Status>();
};
#endif

} // namespace

static const QString kInputsFolder = QStringLiteral("/Fulskrin/Inputs/");

QString liveInputPath(const QString &uri) { return kInputsFolder + QString::fromLatin1(QUrl::toPercentEncoding(uri)); }

QString liveInputUriOfPath(const QString &path)
{
    const QString p = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int at = p.indexOf(kInputsFolder);
    if (at < 0) return {};
    const QString uri = QUrl::fromPercentEncoding(p.mid(at + kInputsFolder.size()).toLatin1());
    return isLiveInputUri(uri) ? uri : QString();
}

QString liveInputKind(const QString &uri)
{
    const QString k = part(uri, 1);
    if (k == QLatin1String("camera")) return QStringLiteral("Camera");
    if (k == QLatin1String("ndi")) return QStringLiteral("NDI");
    if (k == QLatin1String("omt")) return QStringLiteral("OMT");
    if (k == QLatin1String("syphon")) return QStringLiteral("Syphon");
    if (k == QLatin1String("test")) return QStringLiteral("Test");
    return {};
}

QString liveInputName(const QString &uri)
{
#ifdef FULSKRIN_HAS_CAMERA
    if (part(uri, 1) == QLatin1String("camera"))
        for (const QCameraDevice &d : QMediaDevices::videoInputs())
            if (QString::fromUtf8(d.id()) == rest(uri)) return d.description();
#endif
    return rest(uri);
}

std::vector<LiveInputInfo> listLiveInputs(const QString &folder)
{
    std::vector<LiveInputInfo> out;
#ifdef FULSKRIN_HAS_CAMERA
    for (const QCameraDevice &d : QMediaDevices::videoInputs())
        out.push_back({QStringLiteral("input:camera:") + QString::fromUtf8(d.id()), d.description(), QStringLiteral("Camera")});
#endif
    for (const LiveInputInfo &i : ndiSources(folder)) out.push_back(i);
    for (const LiveInputInfo &i : omtSources(folder)) out.push_back(i);
#ifdef FULSKRIN_HAS_SYPHON
    for (const LiveInputInfo &i : listSyphonInputs()) out.push_back(i);
#endif
    return out;
}

std::unique_ptr<LiveInput> createLiveInput(const QString &uri, const QString &folder, QString *err)
{
    auto fail = [err](const QString &e) -> std::unique_ptr<LiveInput> {
        if (err) *err = e;
        return nullptr;
    };
    const QString kind = part(uri, 1), what = rest(uri);
    if (!isLiveInputUri(uri) || what.isEmpty()) return fail(QStringLiteral("Not an input: ") + uri);
    if (kind == QLatin1String("test")) {
        const QStringList c = what.split(QLatin1Char(','));
        return std::make_unique<TestInput>(QColor(c.value(0).toInt(), c.value(1).toInt(), c.value(2).toInt()));
    }
    if (kind == QLatin1String("ndi")) {
        ndi::Lib &lib = ndi::lib(folder);
        if (!lib.ok) return fail(lib.error);
        if (!lib.canReceive()) return fail(QStringLiteral("This NDI library cannot receive: ") + lib.path);
        return std::make_unique<NdiInput>(&lib, what);
    }
    if (kind == QLatin1String("omt")) {
        omt::Lib &lib = omt::lib(folder);
        if (!lib.ok) return fail(lib.error);
        if (!lib.canReceive()) return fail(QStringLiteral("This OMT library cannot receive: ") + lib.path);
        return std::make_unique<OmtInput>(&lib, what);
    }
    if (kind == QLatin1String("camera")) {
#ifdef FULSKRIN_HAS_CAMERA
        for (const QCameraDevice &d : QMediaDevices::videoInputs())
            if (QString::fromUtf8(d.id()) == what) return std::make_unique<CameraInput>(d);
        return fail(QStringLiteral("Camera not found: ") + what);
#else
        return fail(QStringLiteral("Cameras: this build has no Qt Multimedia"));
#endif
    }
    if (kind == QLatin1String("syphon")) {
#ifdef FULSKRIN_HAS_SYPHON
        return createSyphonInput(what, err);
#else
        return fail(QStringLiteral("Syphon is only available on macOS"));
#endif
    }
    return fail(QStringLiteral("Unknown input: ") + uri);
}
