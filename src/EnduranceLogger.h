#pragma once
// Optional endurance metrics recorder. All file I/O happens on a dedicated worker,
// never on the render thread. CSV rows are sampled every second; deadline events
// are appended to a separate JSONL file.
#include "Engine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStringList>
#include <QJsonObject>
#include <QSysInfo>
#include <QDebug>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if __has_include("pinol_build_sha.h")
#include "pinol_build_sha.h" // generated at every build (cmake/BuildSha.cmake)
#endif
#ifndef PINOL_BUILD_SHA
#define PINOL_BUILD_SHA "unknown"
#endif

class EnduranceLogger
{
public:
    ~EnduranceLogger() { stop(); }
    EnduranceLogger(const EnduranceLogger &) = delete;
    EnduranceLogger &operator=(const EnduranceLogger &) = delete;
    EnduranceLogger() = default;

    bool start(Engine &engine, const QString &csvPath, QString *error = nullptr)
    {
        QFile probe(csvPath);
        if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (error) *error = probe.errorString();
            return false;
        }
        probe.close();
        m_engine = &engine;
        m_path = csvPath;
        m_version = QCoreApplication::applicationVersion();
        // Discard pre-session timing; the next window is a true reporting interval.
        engine.takeFrameTimingWindow();
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    static QString fingerprint(const QString &path)
    {
        if (path.isEmpty()) return QStringLiteral("unsaved");
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("unreadable");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&f)) return QStringLiteral("unreadable");
        return QString::fromLatin1(hash.result().toHex());
    }

    static QByteArray csvValue(QString value)
    {
        value.replace('"', "\"\"");
        return (QStringLiteral("\"") + value + QStringLiteral("\"")).toUtf8();
    }

    static void jsonLine(QFile &f, const QJsonObject &obj)
    {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.write("\n");
    }

    void run()
    {
        QFile csv(m_path), events(m_path + QStringLiteral(".events.jsonl"));
        if (!csv.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text) ||
            !events.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            qWarning("Endurance telemetry: could not open CSV/events output files");
            return;
        }
        QString project = m_engine->projectPath();
        const QJsonObject meta{{"schema", 1}, {"version", m_version}, {"git_sha", QStringLiteral(PINOL_BUILD_SHA)},
                               {"utc_start", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                               {"host_os", QSysInfo::prettyProductName()},
                               {"gpu_vendor", m_engine->glVendor()}, {"gpu_renderer", m_engine->glRenderer()},
                               {"opengl_version", m_engine->glVersion()},
                               {"driver_override", qEnvironmentVariable("PINOL_GPU_DRIVER", "unspecified")},
                               {"project", project}, {"project_sha256", fingerprint(project)}};
        csv.write("# PINOL endurance metrics v1\n# ");
        csv.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
        csv.write("\n");
        csv.write("utc,frames,missed,interval_p99_ms,interval_max_ms,compute_p95_ms,compute_p99_ms,compute_max_ms,present_p99_ms,present_max_ms,osc_to_frame_p99_ms,cue_to_frame_p99_ms,osc_inputs,cue_inputs,outputs_dropped_total,shares_skipped_total,process_resident_mb,decoded_frames_mb,upload_buffers_mb,graphics_memory_free_mb,graphics_memory_total_mb,software_decode_threads,ffmpeg_errors_total,empty_video_loops_stopped_total,resource_warnings,late_events_lost,journal_lost\n");
        jsonLine(events, QJsonObject{{"kind", "session"}, {"meta", meta}});
        csv.flush();
        events.flush();

        while (true) {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_cv.wait_for(lock, std::chrono::seconds(1), [this] { return m_stop; })) break;
            lock.unlock();

            const auto wall = QDateTime::currentDateTimeUtc();
            const qint64 nowNs = FrameTiming::nowNs();
            const auto window = m_engine->takeFrameTimingWindow();
            const QJsonObject w = window.stats.json();
            const QJsonObject total = m_engine->frameTimingJson();
            const QJsonObject budget = m_engine->resourcesJson();
            auto val = [](const QJsonObject &o, const char *key) { return QString::number(o.value(QLatin1String(key)).toDouble(), 'f', 3); };
            auto hist = [&](const char *key, const char *stat) { return val(w.value(QLatin1String(key)).toObject(), stat); };

            QStringList row{
                csvValue(wall.toString(Qt::ISODateWithMs)),
                val(w, "frames"), val(w, "missed_deadlines"),
                hist("interval", "p99_ms"), hist("interval", "max_ms"),
                hist("compute", "p95_ms"), hist("compute", "p99_ms"), hist("compute", "max_ms"),
                hist("present", "p99_ms"), hist("present", "max_ms"),
                hist("osc_to_frame", "p99_ms"), hist("cue_to_frame", "p99_ms"),
                QString::number(window.inputs[FrameTiming::Osc]), QString::number(window.inputs[FrameTiming::Cue]),
                val(total, "outputs_dropped"), val(total, "shares_skipped"),
                val(budget, "process_resident_mb"),
                val(budget, "decoded_frames_mb"), val(budget, "upload_buffers_mb"),
                val(budget, "graphics_memory_free_mb"), val(budget, "graphics_memory_total_mb"),
                val(budget, "software_decode_threads"),
                val(budget, "ffmpeg_errors_total"), val(budget, "empty_video_loops_stopped_total"),
                csvValue(QString::fromUtf8(QJsonDocument(budget.value("warnings").toArray()).toJson(QJsonDocument::Compact))),
                QString::number(window.eventsLost), QString::number(window.journalLost)};
            csv.write(row.join(',').toUtf8());
            csv.write("\n");

            for (size_t i = 0; i < window.eventCount; ++i) {
                const auto &ev = window.events[i];
                const qint64 deltaMs = (nowNs - ev.atNs) / 1000000;
                jsonLine(events, QJsonObject{{"kind", "frame_over_2x_budget"},
                    {"utc", wall.addMSecs(-deltaMs).toString(Qt::ISODateWithMs)},
                    {"at_monotonic_ns", ev.atNs}, {"interval_ms", ev.intervalMs}, {"budget_ms", ev.budgetMs}});
            }
            // What the show did in that second (snapshots recalled, timelines driven, layers sought), in order
            for (const auto &ev : window.journal) {
                const qint64 deltaMs = (nowNs - ev.atNs) / 1000000;
                jsonLine(events, QJsonObject{{"kind", ev.kind}, {"detail", ev.detail},
                    {"utc", wall.addMSecs(-deltaMs).toString(Qt::ISODateWithMs)}, {"at_monotonic_ns", ev.atNs}});
            }
            const QString next = m_engine->projectPath();
            if (next != project) {
                project = next;
                jsonLine(events, QJsonObject{{"kind", "project_changed"},
                    {"utc", wall.toString(Qt::ISODateWithMs)},
                    {"project", next}, {"project_sha256", fingerprint(next)}});
            }
            csv.flush();
            events.flush();
        }
    }

    Engine *m_engine = nullptr;
    QString m_path, m_version;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
};
