#pragma once
// MIDI ports (RtMidi: CoreMIDI, Windows MM, ALSA): listed by name, opened by name. Used by the timecode (MTC).
// Built without a MIDI backend (Linux without ALSA): no port.

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace midi {

bool available();          // built with a MIDI backend
bool virtualPorts();       // ports of its own can be made (macOS, Linux)
QStringList outputNames(); // the ports to send to (not Fulskrin's own virtual ones)
QStringList inputNames();  // the ports to receive from

class Output
{
public:
    Output();
    ~Output();
    bool open(const QString &name, QString *err); // empty: the first port
    // A port of its own other software reads from, named so (macOS, Linux; false elsewhere)
    bool openVirtual(const QString &name, QString *err);
    void send(const std::vector<uint8_t> &msg);
    QString name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

class Input
{
public:
    using Callback = std::function<void(const uint8_t *msg, size_t len)>; // its thread
    Input();
    ~Input();
    bool open(const QString &name, Callback cb, QString *err); // empty: the first port
    QString name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace midi
