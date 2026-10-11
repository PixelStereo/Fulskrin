#include "Interactions.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
static const char *const kSources[] = {"keyboard", "midi", "osc", "internal"};
static const char *const kFilters[] = {"average", "weighted_average", "pulse_detection", "attack_release", "damper"};
static const char *const kMidi[] = {"cc", "note", "pitch_bend", "program"};

QString interactionSourceName(InteractionSource s)
{
    switch (s) {
    case InteractionSource::Keyboard: return QStringLiteral("Keyboard");
    case InteractionSource::Midi: return QStringLiteral("MIDI");
    case InteractionSource::Osc: return QStringLiteral("OSC");
    case InteractionSource::Internal: return QStringLiteral("Internal");
    }
    return QString();
}

QString interactionFilterName(InteractionFilter f)
{
    switch (f) {
    case InteractionFilter::Average: return QStringLiteral("Average");
    case InteractionFilter::WeightedAverage: return QStringLiteral("Weighted Average");
    case InteractionFilter::PulseDetection: return QStringLiteral("Pulse Detection");
    case InteractionFilter::AttackRelease: return QStringLiteral("Attack Release");
    case InteractionFilter::Damper: return QStringLiteral("Damper");
    }
    return QString();
}

QString midiMessageName(MidiMessageKind k)
{
    switch (k) {
    case MidiMessageKind::ControlChange: return QStringLiteral("CC");
    case MidiMessageKind::Note: return QStringLiteral("Note");
    case MidiMessageKind::PitchBend: return QStringLiteral("Pitch Bend");
    case MidiMessageKind::ProgramChange: return QStringLiteral("Program");
    }
    return QString();
}

FilterSetting interactionFilterSetting(InteractionFilter f, int which)
{
    const QString s = QStringLiteral(" s");
    switch (f) {
    case InteractionFilter::Average: return which == 0 ? FilterSetting{"Window", s, 0.01, 5, 0.2} : FilterSetting{};
    case InteractionFilter::WeightedAverage: return which == 0 ? FilterSetting{"Smoothing", s, 0.01, 5, 0.2} : FilterSetting{};
    case InteractionFilter::PulseDetection:
        return which == 0 ? FilterSetting{"Threshold", "", 0.01, 1, 0.2} : FilterSetting{"Decay", s, 0.01, 5, 0.3};
    case InteractionFilter::AttackRelease:
        return which == 0 ? FilterSetting{"Attack", s, 0.001, 5, 0.05} : FilterSetting{"Release", s, 0.001, 5, 0.5};
    case InteractionFilter::Damper:
        return which == 0 ? FilterSetting{"Response", s, 0.02, 5, 0.4} : FilterSetting{"Damping", "", 0.05, 2, 0.4};
    }
    return FilterSetting{};
}

QString InteractionEvent::describe() const
{
    switch (source) {
    case InteractionSource::Keyboard: return QStringLiteral("Key %1").arg(address);
    case InteractionSource::Midi: {
        QString s = QStringLiteral("MIDI · ch %1 · %2").arg(channel).arg(midiMessageName(message));
        if (message != MidiMessageKind::PitchBend) s += QStringLiteral(" %1").arg(number);
        return s;
    }
    case InteractionSource::Osc: return QStringLiteral("OSC %1").arg(address) + (argument ? QStringLiteral(" [%1]").arg(argument) : QString());
    case InteractionSource::Internal: return QStringLiteral("Internal");
    }
    return QString();
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
bool Interaction::matches(const InteractionEvent &e) const
{
    if (e.source != source) return false;
    switch (source) {
    case InteractionSource::Keyboard: return !key.isEmpty() && e.address.compare(key, Qt::CaseInsensitive) == 0;
    case InteractionSource::Midi:
        if (!midiPort.isEmpty() && e.port != midiPort) return false;
        if (midiChannel && e.channel != midiChannel) return false;
        if (e.message != midiMessage) return false;
        return midiMessage == MidiMessageKind::PitchBend || e.number == midiNumber;
    case InteractionSource::Osc: return !oscAddress.isEmpty() && e.address == oscAddress && e.argument == oscArgument;
    case InteractionSource::Internal: return false; // read from the show each frame, not sent
    }
    return false;
}

AnimTrack Interaction::straightCurve()
{
    AnimTrack t;
    AnimKey a, b;
    a.t = 0;
    a.v = 0;
    b.t = 1;
    b.v = 1;
    t.keys = {a, b};
    return t;
}

Interaction Interaction::made(InteractionSource s)
{
    Interaction it;
    it.source = s;
    it.curve = straightCurve();
    // A fader or a knob: it waits to meet the value (pick-up); a key, a link: it sets it
    it.takeover = s == InteractionSource::Midi || s == InteractionSource::Osc ? Takeover::PickUp : Takeover::Jump;
    return it;
}

QJsonObject Interaction::toJson() const
{
    QJsonObject o;
    o["id"] = QString::number(id); // ids are strings in files
    if (!name.isEmpty()) o["name"] = name;
    o["enabled"] = enabled;
    o["source"] = kSources[int(source)];
    switch (source) {
    case InteractionSource::Keyboard: o["key"] = key; break;
    case InteractionSource::Midi:
        if (!midiPort.isEmpty()) o["port"] = midiPort;
        o["channel"] = midiChannel;
        o["message"] = kMidi[int(midiMessage)];
        o["number"] = midiNumber;
        break;
    case InteractionSource::Osc:
        o["address"] = oscAddress;
        o["argument"] = oscArgument;
        break;
    case InteractionSource::Internal:
        o["source_layer"] = QString::number(sourceLayer);
        o["source_param"] = sourceParam;
        break;
    }
    o["layer"] = QString::number(layer);
    o["param"] = param;
    o["master"] = master;
    o["toggle"] = toggle;
    o["source_range"] = QJsonArray{srcMin, srcMax};
    o["target_range"] = QJsonArray{dstMin, dstMax};
    o["filter_on"] = filterOn;
    o["filter"] = kFilters[int(filter)];
    o["filter_settings"] = QJsonArray{filterA, filterB};
    o["curve_on"] = curveOn;
    QJsonArray keys;
    for (const AnimKey &k : curve.keys) {
        QJsonObject kj{{"t", k.t}, {"v", k.v}, {"curve", k.curve}};
        if (k.hasIn()) kj["in"] = QJsonArray{k.inDt, k.inDv};
        if (k.hasOut()) kj["out"] = QJsonArray{k.outDt, k.outDv};
        keys.append(kj);
    }
    o["curve"] = keys;
    o["takeover"] = takeover == Takeover::PickUp ? "pickup" : "jump";
    return o;
}

template <size_t N> static int keyIndex(const char *const (&keys)[N], const QString &k, int def)
{
    for (size_t i = 0; i < N; ++i)
        if (k == QLatin1String(keys[i])) return int(i);
    return def;
}

Interaction Interaction::fromJson(const QJsonObject &o)
{
    Interaction it;
    it.id = o.value("id").toString().toULongLong();
    it.name = o.value("name").toString();
    it.enabled = o.value("enabled").toBool(true);
    it.source = InteractionSource(keyIndex(kSources, o.value("source").toString(), 0));
    it.key = o.value("key").toString();
    it.midiPort = o.value("port").toString();
    it.midiChannel = std::clamp(o.value("channel").toInt(0), 0, 16);
    it.midiMessage = MidiMessageKind(keyIndex(kMidi, o.value("message").toString(), 0));
    it.midiNumber = std::clamp(o.value("number").toInt(0), 0, 127);
    it.oscAddress = o.value("address").toString();
    it.oscArgument = std::max(0, o.value("argument").toInt(0));
    it.sourceLayer = o.value("source_layer").toString().toULongLong();
    it.sourceParam = o.value("source_param").toString();
    it.layer = o.value("layer").toString().toULongLong();
    it.param = o.value("param").toString();
    it.master = std::clamp(o.value("master").toDouble(1), 0.0, 1.0);
    it.toggle = o.value("toggle").toBool(false);
    const QJsonArray sr = o.value("source_range").toArray(), tr = o.value("target_range").toArray();
    it.srcMin = sr.at(0).toDouble(0);
    it.srcMax = sr.at(1).toDouble(1);
    it.dstMin = tr.at(0).toDouble(0);
    it.dstMax = tr.at(1).toDouble(1);
    it.filterOn = o.value("filter_on").toBool(false);
    it.filter = InteractionFilter(keyIndex(kFilters, o.value("filter").toString(), 0));
    const QJsonArray fs = o.value("filter_settings").toArray();
    it.filterA = fs.at(0).toDouble(interactionFilterSetting(it.filter, 0).def);
    it.filterB = fs.at(1).toDouble(interactionFilterSetting(it.filter, 1).def);
    it.curveOn = o.value("curve_on").toBool(false);
    for (const QJsonValue &v : o.value("curve").toArray()) {
        const QJsonObject kj = v.toObject();
        AnimKey k;
        k.t = std::clamp(kj.value("t").toDouble(), 0.0, 1.0);
        k.v = kj.value("v").toDouble();
        k.curve = kj.value("curve").toInt(0);
        const QJsonArray in = kj.value("in").toArray(), out = kj.value("out").toArray();
        k.inDt = in.at(0).toDouble(0);
        k.inDv = in.at(1).toDouble(0);
        k.outDt = out.at(0).toDouble(0);
        k.outDv = out.at(1).toDouble(0);
        it.curve.keys.push_back(k);
    }
    std::sort(it.curve.keys.begin(), it.curve.keys.end(), [](const AnimKey &a, const AnimKey &b) { return a.t < b.t; });
    if (it.curve.keys.empty()) it.curve = straightCurve();
    it.takeover = o.value("takeover").toString() == QLatin1String("pickup") ? Takeover::PickUp : Takeover::Jump;
    return it;
}

// ---------------------------------------------------------------------------
// The chain
// ---------------------------------------------------------------------------
void InteractionProcessor::input(const Interaction &it, double raw)
{
    if (!std::isfinite(raw)) return;
    const double span = it.srcMax - it.srcMin;
    double n = std::abs(span) < 1e-12 ? (raw >= it.srcMax ? 1.0 : 0.0) : (raw - it.srcMin) / span;
    n = std::clamp(n, 0.0, 1.0);
    if (it.toggle) {
        const bool above = n > 0.5;
        if (above && !m_above) m_toggled = !m_toggled; // up past the middle: flips
        m_above = above;
        n = m_toggled ? 1.0 : 0.0;
    }
    m_level = n;
    m_fresh = true;
}

std::optional<double> InteractionProcessor::step(const Interaction &it, double dt)
{
    if (!m_level) return std::nullopt;
    dt = std::clamp(dt, 0.0, 0.25); // a long stall is not a long smoothing
    m_time += dt;
    const double x = *m_level;
    double y = x;
    if (it.filterOn) {
        if (!m_started) { // starts where the source is (not from 0)
            m_y = x;
            m_slow = x;
            m_v = 0;
            m_started = true;
            if (it.filter == InteractionFilter::PulseDetection) m_y = 0;
        }
        const double a = std::max(1e-3, it.filterA), b = std::max(1e-3, it.filterB);
        auto toward = [dt](double from, double to, double tau) { return from + (to - from) * (1.0 - std::exp(-dt / tau)); };
        switch (it.filter) {
        case InteractionFilter::Average: {
            m_window.push_back({m_time, x});
            while (!m_window.empty() && m_window.front().first < m_time - a) m_window.pop_front();
            double sum = 0;
            for (const auto &[t, v] : m_window) sum += v;
            m_y = m_window.empty() ? x : sum / double(m_window.size());
            break;
        }
        case InteractionFilter::WeightedAverage: m_y = toward(m_y, x, a / 3.0); break; // ~95% of the way in `a`
        case InteractionFilter::PulseDetection: {
            // A rise above the slow average by more than the threshold: a pulse, decaying in `b`
            if (m_fresh && x - m_slow > a) m_y = 1.0;
            else m_y = std::max(0.0, m_y - dt / b);
            m_slow = toward(m_slow, x, 0.25);
            break;
        }
        case InteractionFilter::AttackRelease: m_y = toward(m_y, x, (x > m_y ? a : b) / 3.0); break;
        case InteractionFilter::Damper: {
            // A spring of period `a`, damped by `b` (1: no overshoot), in small steps for stability
            const double w = 2.0 * M_PI / a;
            const int n = std::max(1, int(std::ceil(dt / 0.002)));
            const double h = dt / n;
            for (int i = 0; i < n; ++i) {
                m_v += (w * w * (x - m_y) - 2.0 * b * w * m_v) * h;
                m_y += m_v * h;
            }
            break;
        }
        }
        y = std::clamp(m_y, 0.0, 1.0);
    }
    m_fresh = false;
    m_curveIn = y;
    if (it.curveOn && !it.curve.keys.empty()) {
        const double c = it.curve.valueAt(y, y);
        if (std::isfinite(c)) y = std::clamp(c, 0.0, 1.0);
    }
    y *= std::clamp(it.master, 0.0, 1.0);
    m_out = y;
    return y;
}

std::optional<double> InteractionTargetState::numberToWrite(const Interaction &it, double out, double current, double tolerance)
{
    const double v = it.dstMin + out * (it.dstMax - it.dstMin);
    if (it.takeover == Takeover::Jump) {
        caught = true;
    } else {
        // Moved by something else since it was written: let go until met again
        if (caught && written && std::abs(current - *written) > tolerance) caught = false;
        if (!caught) {
            const bool met = std::abs(v - current) <= tolerance || (prevOut && (*prevOut - current) * (v - current) <= 0);
            prevOut = v;
            if (!met) return std::nullopt;
            caught = true;
        }
    }
    prevOut = v;
    if (written && std::abs(*written - v) < 1e-12 && std::abs(current - v) < 1e-12) return std::nullopt; // already there
    written = v;
    return v;
}
