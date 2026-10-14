#pragma once
// The publishers of the viewports (NDI, OMT, Syphon, Spout; the tests' tap): one set per viewport, following its
// settings. Owned by the Engine; driven from the render thread with the engine's OpenGL context current, its states
// read from any thread. It knows nothing of the layers: the engine hands it each viewport's id, settings and picture.

#include "Gl.h"
#include "Publish.h"

#include <QElapsedTimer>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class Publishing
{
public:
    // drawQuad: draws the engine's full-screen quad (the flip for the readback goes through it)
    explicit Publishing(std::function<void()> drawQuad);
    ~Publishing();

    // --- Render thread, context current
    bool initGl(QString *log);
    // Every viewport's publishers follow its settings (in the list's order: the first one carries the test tap);
    // those of a viewport that is gone are stopped. Cheap when nothing changed
    void follow(const std::vector<std::pair<quint64, PublishSettings>> &viewports);
    // NDI, OMT, the tap: the picture read back without waiting, sent by a thread of their own (before the frame's end)
    // tc: the timecode the picture carries (ATC), if any
    void readback(quint64 viewport, const RenderTarget &picture, double fps, quint64 frame,
                  const std::optional<Timecode> &tc = std::nullopt);
    // Syphon, Spout: their contexts copy the finished picture (after the frame's end)
    void share(quint64 viewport, const RenderTarget &picture);
    bool sharing() const; // a Syphon or Spout publisher is on: the frame must be finished before share()
    quint64 readbacksSkipped() const { return m_readbacksSkipped.load(); } // NDI / OMT frames not sent: GPU late
    void release();       // every publisher stopped, the OpenGL objects deleted

    // --- Any thread
    PublishState state(quint64 viewport, PublishKind k) const;
    void setTap(std::function<void(const CpuFrame &)> fn); // tests: frames of the first viewport
    // Tests: Syphon / Spout publishers made by this (any platform, without the SDKs) instead of the real ones; set
    // before the publish settings that use them. nullptr: the real ones again
    using GpuPublisherFactory = std::function<std::unique_ptr<GpuPublisher>(PublishKind)>;
    void setGpuPublisherFactoryForTests(GpuPublisherFactory f);

private:
    struct Publication;
    void apply(Publication &pub, const PublishSettings &s);
    void releasePublication(Publication &pub);
    void setState(Publication &pub, PublishKind k, PublishState st);

    std::function<void()> m_drawQuad;
    GLuint m_flipProgram = 0;
    GLint m_flipTexLoc = -1;
    std::map<quint64, std::unique_ptr<Publication>> m_pubs; // by viewport id (render thread; states: m_stateMutex)
    mutable std::mutex m_stateMutex;
    std::mutex m_tapMutex;
    std::function<void(const CpuFrame &)> m_tap;
    bool m_tapDirty = false;
    std::mutex m_gpuFactoryMutex;
    GpuPublisherFactory m_gpuFactory; // tests (under m_gpuFactoryMutex)
    QElapsedTimer m_clock;
    std::atomic<quint64> m_readbacksSkipped{0};
};
