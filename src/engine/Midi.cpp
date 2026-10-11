#include "Midi.h"

#ifdef FULSKRIN_HAS_MIDI
#include <RtMidi.h>

template <class Port> static QStringList portNames()
{
    QStringList out;
    try {
        Port p;
        const unsigned n = p.getPortCount();
        for (unsigned i = 0; i < n; ++i) out << QString::fromStdString(p.getPortName(i));
    } catch (const RtMidiError &) {
    }
    return out;
}

// The port of that name (empty: the first one), -1 when there is none
template <class Port> static int portIndex(Port &p, const QString &name)
{
    const unsigned n = p.getPortCount();
    if (n == 0) return -1;
    if (name.isEmpty()) return 0;
    for (unsigned i = 0; i < n; ++i)
        if (QString::fromStdString(p.getPortName(i)) == name) return int(i);
    return -1;
}

bool midi::available() { return true; }
QStringList midi::outputNames() { return portNames<RtMidiOut>(); }
QStringList midi::inputNames() { return portNames<RtMidiIn>(); }

struct midi::Output::Impl {
    std::unique_ptr<RtMidiOut> port;
    QString name;
};

midi::Output::Output() : d(std::make_unique<Impl>()) {}
midi::Output::~Output() = default;

bool midi::Output::open(const QString &name, QString *err)
{
    try {
        d->port = std::make_unique<RtMidiOut>();
        const int i = portIndex(*d->port, name);
        if (i < 0) {
            if (err) *err = name.isEmpty() ? QStringLiteral("no MIDI output") : QStringLiteral("MIDI output \"%1\" not found").arg(name);
            d->port.reset();
            return false;
        }
        d->port->openPort(unsigned(i), "Fulskrin");
        d->name = QString::fromStdString(d->port->getPortName(unsigned(i)));
        return true;
    } catch (const RtMidiError &e) {
        if (err) *err = QString::fromStdString(e.getMessage());
        d->port.reset();
        return false;
    }
}

void midi::Output::send(const std::vector<uint8_t> &msg)
{
    if (!d->port) return;
    try {
        d->port->sendMessage(msg.data(), msg.size());
    } catch (const RtMidiError &) {
    }
}

QString midi::Output::name() const { return d->name; }

struct midi::Input::Impl {
    std::unique_ptr<RtMidiIn> port;
    Callback cb;
    QString name;
    static void received(double, std::vector<unsigned char> *msg, void *self)
    {
        auto *impl = static_cast<Impl *>(self);
        if (msg && !msg->empty() && impl->cb) impl->cb(msg->data(), msg->size());
    }
};

midi::Input::Input() : d(std::make_unique<Impl>()) {}
midi::Input::~Input()
{
    if (d->port) {
        try {
            d->port->cancelCallback();
            d->port->closePort();
        } catch (const RtMidiError &) {
        }
    }
}

bool midi::Input::open(const QString &name, Callback cb, QString *err)
{
    try {
        d->port = std::make_unique<RtMidiIn>();
        const int i = portIndex(*d->port, name);
        if (i < 0) {
            if (err) *err = name.isEmpty() ? QStringLiteral("no MIDI input") : QStringLiteral("MIDI input \"%1\" not found").arg(name);
            d->port.reset();
            return false;
        }
        d->cb = std::move(cb);
        d->port->openPort(unsigned(i), "Fulskrin");
        d->port->ignoreTypes(false, false, true); // SysEx (MTC full frames) and time code kept, active sensing not
        d->port->setCallback(&Impl::received, d.get());
        d->name = QString::fromStdString(d->port->getPortName(unsigned(i)));
        return true;
    } catch (const RtMidiError &e) {
        if (err) *err = QString::fromStdString(e.getMessage());
        d->port.reset();
        return false;
    }
}

QString midi::Input::name() const { return d->name; }

#else // no MIDI backend

bool midi::available() { return false; }
QStringList midi::outputNames() { return {}; }
QStringList midi::inputNames() { return {}; }
struct midi::Output::Impl {};
midi::Output::Output() : d(std::make_unique<Impl>()) {}
midi::Output::~Output() = default;
bool midi::Output::open(const QString &, QString *err)
{
    if (err) *err = QStringLiteral("built without MIDI");
    return false;
}
void midi::Output::send(const std::vector<uint8_t> &) {}
QString midi::Output::name() const { return {}; }
struct midi::Input::Impl {};
midi::Input::Input() : d(std::make_unique<Impl>()) {}
midi::Input::~Input() = default;
bool midi::Input::open(const QString &, Callback, QString *err)
{
    if (err) *err = QStringLiteral("built without MIDI");
    return false;
}
QString midi::Input::name() const { return {}; }

#endif
