// Engine: the interactions (Interactions.h) — their list, their sources' events, and the chains run each frame.
#include "EngineInternal.h"
#include "Midi.h"

#include <QTimer>
#include <cmath>

std::vector<Interaction> Engine::interactions() const
{
    Lock lk(&m_mutex);
    return m_interactions;
}

void Engine::setInteractions(const std::vector<Interaction> &list)
{
    {
        Lock lk(&m_mutex);
        m_interactions = list;
        for (const Interaction &it : m_interactions) m_nextInteractionId = std::max(m_nextInteractionId, it.id + 1);
        for (Interaction &it : m_interactions)
            if (!it.id) it.id = m_nextInteractionId++;
        // Those gone: their state goes; those that stay keep theirs (a fader held is not let go by an edit)
        for (auto r = m_interactionRun.begin(); r != m_interactionRun.end();) {
            const bool kept = std::any_of(m_interactions.begin(), m_interactions.end(), [&](const Interaction &it) { return it.id == r->first; });
            r = kept ? std::next(r) : m_interactionRun.erase(r);
        }
    }
    updateMidiInputs();
    emit interactionsChanged();
}

quint64 Engine::addInteraction(const Interaction &it)
{
    quint64 id;
    {
        Lock lk(&m_mutex);
        Interaction n = it;
        n.id = id = m_nextInteractionId++;
        m_interactions.push_back(n);
    }
    updateMidiInputs();
    emit interactionsChanged();
    return id;
}

bool Engine::setInteraction(const Interaction &it)
{
    {
        Lock lk(&m_mutex);
        auto f = std::find_if(m_interactions.begin(), m_interactions.end(), [&](const Interaction &x) { return x.id == it.id; });
        if (f == m_interactions.end()) return false;
        const bool sourceChanged = f->source != it.source || f->toggle != it.toggle || f->filterOn != it.filterOn ||
                                   f->filter != it.filter || f->layer != it.layer || f->param != it.param ||
                                   f->sourceLayer != it.sourceLayer || f->sourceParam != it.sourceParam;
        *f = it;
        if (sourceChanged) m_interactionRun.erase(it.id); // starts again from what comes next
    }
    updateMidiInputs();
    emit interactionsChanged();
    return true;
}

void Engine::removeInteraction(quint64 id)
{
    {
        Lock lk(&m_mutex);
        const auto n = m_interactions.size();
        m_interactions.erase(std::remove_if(m_interactions.begin(), m_interactions.end(), [id](const Interaction &x) { return x.id == id; }),
                             m_interactions.end());
        if (n == m_interactions.size()) return;
        m_interactionRun.erase(id);
    }
    updateMidiInputs();
    emit interactionsChanged();
}

bool Engine::interactionLevels(quint64 id, double *level, double *output) const
{
    Lock lk(&m_mutex);
    const auto r = m_interactionRun.find(id);
    if (r == m_interactionRun.end() || !r->second.proc.level()) return false;
    if (level) *level = *r->second.proc.level();
    if (output) *output = r->second.proc.output().value_or(*r->second.proc.level());
    return true;
}

void Engine::feedInteraction(const InteractionEvent &e)
{
    std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
    if (m_interactionEvents.size() < 8192) m_interactionEvents.push_back(e); // a flood is cut, not kept forever
    if (m_interactionLearn && (e.source != InteractionSource::Keyboard || e.value > 0.5)) m_learned = e; // a key: pressed
}

void Engine::setInteractionLearn(bool on)
{
    {
        std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
        m_interactionLearn = on;
        m_learned.reset();
    }
    updateMidiInputs(); // a controller is listened to while learning
}

std::optional<InteractionEvent> Engine::takeLearnedInteraction()
{
    std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
    std::optional<InteractionEvent> e = m_learned;
    m_learned.reset();
    return e;
}

// ---------------------------------------------------------------------------
// MIDI: every input of the machine, open while a MIDI interaction is there (or while learning); looked for again every
// few seconds (a controller plugged in)
// ---------------------------------------------------------------------------
static bool parseMidi(const uint8_t *m, size_t n, InteractionEvent &e)
{
    if (n < 2) return false;
    const int status = m[0] & 0xF0;
    e.source = InteractionSource::Midi;
    e.channel = (m[0] & 0x0F) + 1;
    switch (status) {
    case 0xB0:
        if (n < 3) return false;
        e.message = MidiMessageKind::ControlChange;
        e.number = m[1] & 0x7F;
        e.value = (m[2] & 0x7F) / 127.0;
        return true;
    case 0x90:
    case 0x80:
        if (n < 3) return false;
        e.message = MidiMessageKind::Note;
        e.number = m[1] & 0x7F;
        e.value = status == 0x90 ? (m[2] & 0x7F) / 127.0 : 0.0; // note on at velocity 0: off
        return true;
    case 0xE0:
        if (n < 3) return false;
        e.message = MidiMessageKind::PitchBend;
        e.value = (((m[2] & 0x7F) << 7) | (m[1] & 0x7F)) / 16383.0;
        return true;
    case 0xC0:
        e.message = MidiMessageKind::ProgramChange;
        e.number = m[1] & 0x7F;
        e.value = 1.0; // a program chosen: up (then down at once, below)
        return true;
    default: return false;
    }
}

void Engine::updateMidiInputs()
{
    bool wanted;
    {
        Lock lk(&m_mutex);
        wanted = std::any_of(m_interactions.begin(), m_interactions.end(),
                             [](const Interaction &it) { return it.enabled && it.source == InteractionSource::Midi; });
    }
    {
        std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
        wanted = wanted || m_interactionLearn;
    }
    if (!wanted) {
        m_midiIns.clear();
        m_midiInNames.clear();
        if (m_midiScan) m_midiScan->stop();
        return;
    }
    if (!m_midiScan) {
        m_midiScan = new QTimer(this);
        m_midiScan->setInterval(2000);
        connect(m_midiScan, &QTimer::timeout, this, &Engine::updateMidiInputs);
    }
    if (!m_midiScan->isActive()) m_midiScan->start();
    const QStringList names = midi::inputNames();
    if (names == m_midiInNames && m_midiIns.size() == size_t(names.size())) return;
    m_midiIns.clear();
    m_midiInNames.clear();
    for (const QString &name : names) {
        auto in = std::make_unique<midi::Input>();
        QString err;
        const bool ok = in->open(name, [this, name](const uint8_t *m, size_t n) {
            InteractionEvent e;
            e.port = name;
            if (!parseMidi(m, n, e)) return;
            feedInteraction(e);
            if (e.message == MidiMessageKind::ProgramChange) { // an action: up, then down
                e.value = 0.0;
                feedInteraction(e);
            }
        }, &err);
        if (!ok) continue;
        m_midiIns.push_back(std::move(in));
        m_midiInNames << name;
    }
    m_midiInNames = names; // the ones that could not be opened are not tried again until the list changes
}

// ---------------------------------------------------------------------------
// The chains, each frame
// ---------------------------------------------------------------------------
void Engine::stepInteractions(double dt)
{
    std::vector<InteractionEvent> events;
    {
        std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
        events.swap(m_interactionEvents);
    }
    if (m_interactions.empty()) return;
    for (const Interaction &it : m_interactions) {
        if (!it.enabled || it.param.isEmpty()) continue;
        InteractionRun &run = m_interactionRun[it.id];
        bool fresh = false;
        if (it.source == InteractionSource::Internal) {
            if (const Parameter *src = findParameter(it.sourceLayer, it.sourceParam)) {
                const QVariant v = src->value();
                const double x = v.typeId() == QMetaType::Bool ? (v.toBool() ? 1.0 : 0.0) : v.toDouble();
                if (std::isfinite(x) && !(x == run.lastInternal)) {
                    run.lastInternal = x;
                    run.proc.input(it, x);
                    fresh = true;
                }
            }
        } else {
            for (const InteractionEvent &e : events)
                if (it.matches(e)) {
                    run.proc.input(it, e.value);
                    fresh = true;
                }
        }
        const std::optional<double> out = run.proc.step(it, dt);
        if (!out) continue;
        const bool changed = fresh || !run.lastOut || std::abs(*run.lastOut - *out) > 1e-9;
        run.lastOut = out;
        if (!changed) continue; // nothing new: the target is left to whatever else moves it
        if (it.layer) {
            const int li = indexOfId(it.layer);
            if (li < 0 || m_layers[size_t(li)]->locked) continue; // a locked layer is left alone
        }
        Parameter *p = findParameter(it.layer, it.param);
        if (!p || !(p->info().access & ParamInfo::Write)) continue;
        const ParamInfo info = p->info();
        bool wrote = false;
        switch (info.type) {
        case ParamInfo::Type::Float:
        case ParamInfo::Type::Int: {
            const double tolerance = std::max(info.step, 0.01 * std::abs(it.dstMax - it.dstMin));
            if (const std::optional<double> v = run.target.numberToWrite(it, *out, p->number(), tolerance))
                wrote = p->setNumber(*v);
            break;
        }
        case ParamInfo::Type::Bool: {
            const bool on = it.dstMin + *out * (it.dstMax - it.dstMin) >= 0.5;
            if (on != p->value().toBool()) wrote = p->setValue(on);
            break;
        }
        case ParamInfo::Type::Choice: {
            if (info.choices.isEmpty()) break;
            const int n = int(info.choices.size());
            const int k = std::clamp(int(std::lround(*out * (n - 1))), 0, n - 1);
            if (!run.choiceSet || k != run.lastChoice) {
                run.choiceSet = true;
                run.lastChoice = k;
                wrote = p->setValue(info.choices[k]);
            }
            break;
        }
        case ParamInfo::Type::Trigger: {
            const bool above = *out > 0.5;
            if (above && !run.target.above) wrote = p->setValue(QVariant()); // fires each time it goes up
            run.target.above = above;
            break;
        }
        case ParamInfo::Type::Text: break;
        }
        if (wrote && it.layer) releaseFromFades(it.layer, it.param); // a fade running on it lets go
    }
}
