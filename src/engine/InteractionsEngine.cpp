// Engine: the interactions (Interactions.h) — their list, their sources' events, and the chains run each frame.
#include "EngineInternal.h"
#include "Midi.h"
#include "EventLog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <cmath>

// Each one has a name of its own (its address in OSC, its name in the presets and the snapshots): an empty one is
// given "Interaction N", one taken gets a number after it
static void nameInteractions(std::vector<Interaction> &list)
{
    QSet<QString> taken;
    int next = 1;
    for (Interaction &it : list) {
        QString base = it.name.trimmed();
        if (base.isEmpty()) {
            while (taken.contains(QStringLiteral("Interaction %1").arg(next))) ++next;
            base = QStringLiteral("Interaction %1").arg(next);
        }
        QString n = base;
        for (int k = 2; taken.contains(n); ++k) n = QStringLiteral("%1 %2").arg(base).arg(k);
        it.name = n;
        taken.insert(n);
    }
}

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
        nameInteractions(m_interactions);
        for (const Interaction &it : m_interactions) m_nextInteractionId = std::max(m_nextInteractionId, it.id + 1);
        for (Interaction &it : m_interactions)
            if (!it.id) it.id = m_nextInteractionId++;
        m_interactionParamsDirty = true;
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
        nameInteractions(m_interactions);
        m_interactionParamsDirty = true;
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
        nameInteractions(m_interactions);
        m_interactionParamsDirty = true;
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
        m_interactionParamsDirty = true;
    }
    updateMidiInputs();
    emit interactionsChanged();
}

bool Engine::interactionLevels(quint64 id, double *level, double *output, double *curveIn) const
{
    Lock lk(&m_mutex);
    const auto r = m_interactionRun.find(id);
    if (r == m_interactionRun.end() || !r->second.proc.level()) return false;
    if (level) *level = *r->second.proc.level();
    if (output) *output = r->second.proc.output().value_or(*r->second.proc.level());
    if (curveIn) *curveIn = r->second.proc.curveInput().value_or(*r->second.proc.level());
    return true;
}

bool Engine::oscAddressClaimed(const QString &address) const
{
    Lock lk(&m_mutex);
    return std::any_of(m_interactions.cbegin(), m_interactions.cend(), [&](const Interaction &it) {
        return it.enabled && it.source == InteractionSource::Osc && !it.oscAddress.isEmpty() &&
               it.oscAddress == address;
    });
}

void Engine::feedInteraction(const InteractionEvent &e)
{
    if (e.source == InteractionSource::Keyboard || e.source == InteractionSource::Midi) {
        const int channel = e.source == InteractionSource::Keyboard ? EventLog::Keyboard : EventLog::MidiIn;
        if (EventLog::instance().wants(channel)) {
            const QString line = e.describe() + QStringLiteral(" = ") + QString::number(e.value, 'g', 5);
            EventLog::instance().append(channel, line);
            emit eventObserved(EventLog::channelName(channel), line);
        }
    }
    std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
    if (m_interactionEvents.size() < 8192) m_interactionEvents.push_back(e); // a flood is cut, not kept forever
    else m_interactionEventsDropped.fetch_add(1, std::memory_order_relaxed);
    if (!m_interactionLearn || (m_learnKind && e.source != *m_learnKind)) return;
    if (e.source == InteractionSource::Keyboard && e.value < 0.5) return; // a key: when pressed
    for (InteractionEvent &x : m_learned)
        if (x.source == e.source && x.address == e.address && x.port == e.port && x.channel == e.channel &&
            x.message == e.message && x.number == e.number && x.argument == e.argument) {
            x.value = e.value;
            x.color = e.color;
            x.colorComponents = e.colorComponents; // update tuple after scalar arg 0
            return;
        }
    if (m_learned.empty()) m_learnClock.start();
    if (m_learned.size() < 32) m_learned.push_back(e);
}

void Engine::setInteractionLearn(bool on, std::optional<InteractionSource> kind)
{
    {
        std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
        m_interactionLearn = on;
        m_learnKind = kind;
        m_learned.clear();
    }
    updateMidiInputs(); // a controller is listened to while learning
}

bool Engine::interactionLearning() const
{
    std::lock_guard<std::mutex> lk(const_cast<Engine *>(this)->m_interactionEventsMutex);
    return m_interactionLearn;
}

std::vector<InteractionEvent> Engine::takeLearnedInteractions(int settleMs)
{
    std::lock_guard<std::mutex> lk(m_interactionEventsMutex);
    if (m_learned.empty() || m_learnClock.elapsed() < settleMs) return {};
    std::vector<InteractionEvent> out;
    out.swap(m_learned);
    return out;
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
    const bool needsSpectrum=std::any_of(m_interactions.begin(),m_interactions.end(),[](const Interaction &it){return it.enabled && it.source==InteractionSource::Audio && it.audioFeature==Interaction::AudioFeature::Band;});
    const auto spectrum=needsSpectrum?m_audio->spectrum():AudioAnalysis::Spectrum{};
    const auto audio=m_audio->analysis();
    for (const Interaction &it : m_interactions) {
        if (!it.enabled || it.param.isEmpty()) continue;
        InteractionRun &run = m_interactionRun[it.id];
        bool fresh = false;
        if (it.source == InteractionSource::Audio) {
            const double x=it.audioFeature==Interaction::AudioFeature::Band?spectrum.band(float(it.audioLow),float(it.audioHigh)):
                it.audioFeature==Interaction::AudioFeature::Level?audio.level:audio.onset;
            run.proc.input(it,x);
            fresh=true;
        } else if (it.source == InteractionSource::Internal) {
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
                    run.color = e.color;
                    run.colorComponents = e.colorComponents;
                    if (e.colorComponents >= 3) { // a whole color: each channel through its own chain (no toggle)
                        Interaction chain = it;
                        chain.toggle = false;
                        for (int c = 0; c < 4; ++c) run.channels[size_t(c)].input(chain, e.color[size_t(c)]);
                    }
                    fresh = true;
                }
        }
        const std::optional<double> out = run.proc.step(it, dt);
        if (!out) continue;
        // A whole color: its channels move on too (a filter keeps moving them after the last message); the alpha
        // without the master level
        bool colorMoved = false;
        if (run.colorComponents >= 3) {
            Interaction chain = it;
            chain.toggle = false;
            for (int c = 0; c < 4; ++c) {
                if (c == 3) chain.master = 1;
                const std::optional<double> o = run.channels[size_t(c)].step(chain, dt);
                if (o && (!run.channelOut[size_t(c)] || std::abs(*run.channelOut[size_t(c)] - *o) > 1e-9)) colorMoved = true;
                run.channelOut[size_t(c)] = o;
            }
        }
        const bool changed = fresh || colorMoved || !run.lastOut || std::abs(*run.lastOut - *out) > 1e-9;
        run.lastOut = out;
        if (!changed) continue; // nothing new: the target is left to whatever else moves it
        if (it.layer) {
            const int li = indexOfId(it.layer);
            if (li < 0 || m_layers[size_t(li)]->locked) continue; // a locked layer is left alone
        }
        Parameter *p = findParameter(it.layer, it.param);
        if (!p) {
            // An ISF color has four scalar Parameter entries but OSC and the
            // ColorPicker expose the natural group address (param/color).
            // Resolve that group here without changing scalar parameter storage
            // or disrupting existing animations, snapshots and presets.
            const QString prefix = it.param + QLatin1Char('/');
            std::array<Parameter *, 4> channels{};
            for (int c = 0; c < 4; ++c)
                channels[size_t(c)] = findParameter(it.layer, prefix + QChar("rgba"[c]));
            bool valid = true;
            for (int c = 0; c < 3; ++c)
                valid &= channels[size_t(c)] && (channels[size_t(c)]->info().access & ParamInfo::Write) &&
                         channels[size_t(c)]->info().isNumber();
            if (!valid) continue;
            // A whole color (OSC 'r', three / four numbers): each channel through its own chain, then the target
            // range, with a pick-up on the whole color. Alpha only with Map alpha and a fourth channel (some senders
            // fill an unused alpha with zero). A single number: a gray, through the usual chain.
            const bool whole = run.colorComponents >= 3;
            const int n = whole && it.colorMapAlpha && run.colorComponents >= 4 && channels[3] &&
                                  (channels[3]->info().access & ParamInfo::Write)
                              ? 4
                              : 3;
            std::array<double, 4> want{{0, 0, 0, 1}}, current{{0, 0, 0, 1}};
            bool ready = true;
            for (int c = 0; c < n; ++c) {
                const std::optional<double> o = whole ? run.channelOut[size_t(c)] : out;
                if (!o) ready = false;
                want[size_t(c)] = std::clamp(it.dstMin + o.value_or(0) * (it.dstMax - it.dstMin), 0.0, 1.0);
                current[size_t(c)] = channels[size_t(c)]->number();
            }
            if (!ready) continue;
            const double tolerance = std::max(0.004, 0.01 * std::abs(it.dstMax - it.dstMin));
            const std::optional<std::array<double, 4>> w = run.colorTarget.toWrite(it, want, current, n, tolerance);
            if (!w) continue;
            for (int c = 0; c < n; ++c) {
                Parameter *ch = channels[size_t(c)];
                if (std::abs(ch->number() - (*w)[size_t(c)]) > 1e-6 && ch->setNumber((*w)[size_t(c)]) && it.layer)
                    releaseFromFades(it.layer, prefix + QChar("rgba"[c]));
            }
            continue;
        }
        if (!(p->info().access & ParamInfo::Write)) continue;
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

// ---------------------------------------------------------------------------
// Their values as the composition's parameters, and in the snapshots
// ---------------------------------------------------------------------------
void Engine::interactionValuesTouched()
{
    if (m_interactionValuesPending.exchange(true)) return; // one is on its way
    QMetaObject::invokeMethod(this, [this] {
        m_interactionValuesPending = false;
        emit interactionValuesChanged();
    }, Qt::QueuedConnection);
}

void Engine::makeInteractionParams()
{
    m_interactionParams.clear();
    using T = Parameter::Type;
    for (const Interaction &it : m_interactions) {
        const quint64 id = it.id;
        const QString prefix = QStringLiteral("interaction/%1/").arg(id);
        const QString cat = QStringLiteral("Interactions › %1 › ").arg(it.name);
        // Read and written in the interaction of that id (the lock held by the caller); a write tells the interface
        auto find = [this, id]() -> Interaction * {
            for (Interaction &x : m_interactions)
                if (x.id == id) return &x;
            return nullptr;
        };
        auto add = [&](const QString &path, const QString &name, T type) -> Parameter & {
            m_interactionParams.push_back(std::make_unique<Parameter>(prefix + path, cat + name, type));
            return *m_interactionParams.back();
        };
        auto number = [&](const QString &path, const QString &name, double lo, double hi, double def,
                          std::function<double &(Interaction &)> field) -> Parameter & {
            return add(path, name, T::Float)
                .range(lo, hi)
                .limits(-1e9, 1e9)
                .step((hi - lo) / 100.0)
                .byDefault(def)
                .bind([find, field] {
                          Interaction *x = find();
                          return x ? QVariant(field(*x)) : QVariant();
                      },
                      [this, find, field](const QVariant &v) {
                          if (Interaction *x = find()) {
                              field(*x) = v.toDouble();
                              interactionValuesTouched();
                          }
                      });
        };
        auto flag = [&](const QString &path, const QString &name, bool def, std::function<bool &(Interaction &)> field) {
            add(path, name, T::Bool).byDefault(def).ramp(Parameter::Ramp::Cut).bind(
                [find, field] {
                    Interaction *x = find();
                    return x ? QVariant(field(*x)) : QVariant();
                },
                [this, find, field](const QVariant &v) {
                    if (Interaction *x = find()) {
                        field(*x) = v.toBool();
                        interactionValuesTouched();
                    }
                });
        };
        // The source's values and the target's, as the interface shows them. The composition's own values are read
        // in its own list (the whole one is being made)
        auto lookup = [this](quint64 layer, const QString &path) -> const Parameter * {
            if (layer) return findParameter(layer, path);
            for (const auto &p : m_compParams)
                if (p->path() == path) return p.get();
            return nullptr;
        };
        double sLo = 0, sHi = 1, tLo = 0, tHi = 1;
        if (it.source == InteractionSource::Osc) {
            sLo = std::min(0.0, it.srcMin);
            sHi = std::max(1.0, it.srcMax);
        } else if (it.source == InteractionSource::Internal) {
            if (const Parameter *p = lookup(it.sourceLayer, it.sourceParam); p && p->isNumber()) {
                sLo = p->info().min;
                sHi = p->info().max;
            }
        }
        if (const Parameter *p = lookup(it.layer, it.param); p && p->isNumber()) {
                tLo = p->info().min;
                tHi = p->info().max;
            }

        flag(QStringLiteral("enable"), QStringLiteral("On"), true, [](Interaction &x) -> bool & { return x.enabled; });
        number(QStringLiteral("master"), QStringLiteral("Master Level"), 0, 1, 1, [](Interaction &x) -> double & { return x.master; })
            .limits(0, 1)
            .animatable();
        flag(QStringLiteral("toggle"), QStringLiteral("Toggle"), false, [](Interaction &x) -> bool & { return x.toggle; });
        flag(QStringLiteral("color/map_alpha"), QStringLiteral("Map Color Alpha"), false,
             [](Interaction &x) -> bool & { return x.colorMapAlpha; });
        number(QStringLiteral("source/min"), QStringLiteral("Source Min"), sLo, sHi, sLo, [](Interaction &x) -> double & { return x.srcMin; });
        number(QStringLiteral("source/max"), QStringLiteral("Source Max"), sLo, sHi, sHi, [](Interaction &x) -> double & { return x.srcMax; });
        number(QStringLiteral("target/min"), QStringLiteral("Target Min"), tLo, tHi, tLo, [](Interaction &x) -> double & { return x.dstMin; })
            .animatable();
        number(QStringLiteral("target/max"), QStringLiteral("Target Max"), tLo, tHi, tHi, [](Interaction &x) -> double & { return x.dstMax; })
            .animatable();
        add(QStringLiteral("takeover"), QStringLiteral("Takeover"), T::Choice)
            .choices({QStringLiteral("pickup"), QStringLiteral("jump")})
            .byDefault(QStringLiteral("jump"))
            .bind([find] {
                      Interaction *x = find();
                      return x ? QVariant(x->takeover == Takeover::PickUp ? QStringLiteral("pickup") : QStringLiteral("jump")) : QVariant();
                  },
                  [this, find](const QVariant &v) {
                      if (Interaction *x = find()) {
                          x->takeover = v.toString() == QLatin1String("pickup") ? Takeover::PickUp : Takeover::Jump;
                          interactionValuesTouched();
                      }
                  });
        flag(QStringLiteral("filter/enable"), QStringLiteral("Filter"), false, [](Interaction &x) -> bool & { return x.filterOn; });
        {
            QStringList keys;
            for (int k = 0; k < kInteractionFilterCount; ++k)
                keys << interactionFilterName(InteractionFilter(k)).toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
            add(QStringLiteral("filter/type"), QStringLiteral("Filter Type"), T::Choice)
                .choices(keys)
                .byDefault(keys.front())
                .bind([find, keys] {
                          Interaction *x = find();
                          return x ? QVariant(keys[int(x->filter)]) : QVariant();
                      },
                      [this, find, keys](const QVariant &v) {
                          const int k = int(keys.indexOf(v.toString()));
                          if (Interaction *x = find(); x && k >= 0) {
                              x->filter = InteractionFilter(k);
                              interactionValuesTouched();
                          }
                      });
        }
        const FilterSetting fa = interactionFilterSetting(it.filter, 0), fb = interactionFilterSetting(it.filter, 1);
        if (!fa.name.isEmpty())
            number(QStringLiteral("filter/a"), QStringLiteral("Filter ") + fa.name, fa.min, fa.max, fa.def,
                   [](Interaction &x) -> double & { return x.filterA; });
        if (!fb.name.isEmpty())
            number(QStringLiteral("filter/b"), QStringLiteral("Filter ") + fb.name, fb.min, fb.max, fb.def,
                   [](Interaction &x) -> double & { return x.filterB; });
        flag(QStringLiteral("curve/enable"), QStringLiteral("Curve"), false, [](Interaction &x) -> bool & { return x.curveOn; });

        // What it is: its name, its source, its target, its curve — set at once (presets and snapshots hold them; a
        // snapshot holds them all, so that one removed since is made again as it was)
        auto changed = [this] { // its name, its source or its target: the list, the MIDI inputs, the labels
            m_interactionParamsDirty = true;
            interactionValuesTouched();
            QMetaObject::invokeMethod(this, [this] { updateMidiInputs(); }, Qt::QueuedConnection);
        };
        auto text = [&](const QString &path, const QString &name, std::function<QString(const Interaction &)> get,
                        std::function<void(Interaction &, const QString &)> set) {
            add(path, name, T::Text).ramp(Parameter::Ramp::Cut).bind(
                [find, get] {
                    Interaction *x = find();
                    return x ? QVariant(get(*x)) : QVariant();
                },
                [this, find, set, changed](const QVariant &v) {
                    if (Interaction *x = find()) {
                        set(*x, v.toString());
                        nameInteractions(m_interactions);
                        changed();
                    }
                });
        };
        auto whole = [&](const QString &path, const QString &name, int lo, int hi, int def, std::function<int &(Interaction &)> field) {
            add(path, name, T::Int).range(lo, hi).limits(lo, hi).step(1).byDefault(def).ramp(Parameter::Ramp::Cut).bind(
                [find, field] {
                    Interaction *x = find();
                    return x ? QVariant(field(*x)) : QVariant();
                },
                [find, field, changed](const QVariant &v) {
                    if (Interaction *x = find()) {
                        field(*x) = v.toInt();
                        changed();
                    }
                });
        };
        auto choice = [&](const QString &path, const QString &name, const QStringList &keys, std::function<int(const Interaction &)> get,
                          std::function<void(Interaction &, int)> set) {
            add(path, name, T::Choice).choices(keys).byDefault(keys.front()).ramp(Parameter::Ramp::Cut).bind(
                [find, get, keys] {
                    Interaction *x = find();
                    return x ? QVariant(keys.value(get(*x))) : QVariant();
                },
                [find, set, keys, changed](const QVariant &v) {
                    const int k = int(keys.indexOf(v.toString()));
                    if (Interaction *x = find(); x && k >= 0) {
                        set(*x, k);
                        changed();
                    }
                });
        };
        // A value of the show, as text: "<layer id>:<address>" (0: the composition)
        auto valueRef = [](quint64 layer, const QString &path) { return path.isEmpty() ? QString() : QStringLiteral("%1:%2").arg(layer).arg(path); };
        auto parseRef = [](const QString &s, quint64 *layer, QString *path) {
            const int colon = s.indexOf(QLatin1Char(':'));
            *layer = colon < 0 ? 0 : s.left(colon).toULongLong();
            *path = colon < 0 ? QString() : s.mid(colon + 1);
        };
        text(QStringLiteral("name"), QStringLiteral("Name"), [](const Interaction &x) { return x.name; },
             [](Interaction &x, const QString &v) {
                 if (!v.trimmed().isEmpty()) x.name = v.trimmed();
             });
        choice(QStringLiteral("source/kind"), QStringLiteral("Source"),
               {QStringLiteral("keyboard"), QStringLiteral("midi"), QStringLiteral("osc"), QStringLiteral("internal"), QStringLiteral("audio")},
               [](const Interaction &x) { return int(x.source); },
               [](Interaction &x, int k) { x.source = InteractionSource(k); });
        text(QStringLiteral("source/key"), QStringLiteral("Key"), [](const Interaction &x) { return x.key; },
             [](Interaction &x, const QString &v) { x.key = v; });
        text(QStringLiteral("source/midi/input"), QStringLiteral("MIDI Input"), [](const Interaction &x) { return x.midiPort; },
             [](Interaction &x, const QString &v) { x.midiPort = v; });
        whole(QStringLiteral("source/midi/channel"), QStringLiteral("MIDI Channel"), 0, 16, 0, [](Interaction &x) -> int & { return x.midiChannel; });
        choice(QStringLiteral("source/midi/message"), QStringLiteral("MIDI Message"),
               {QStringLiteral("cc"), QStringLiteral("note"), QStringLiteral("pitch_bend"), QStringLiteral("program")},
               [](const Interaction &x) { return int(x.midiMessage); },
               [](Interaction &x, int k) { x.midiMessage = MidiMessageKind(k); });
        whole(QStringLiteral("source/midi/number"), QStringLiteral("MIDI Number"), 0, 127, 0, [](Interaction &x) -> int & { return x.midiNumber; });
        text(QStringLiteral("source/osc/address"), QStringLiteral("OSC Address"), [](const Interaction &x) { return x.oscAddress; },
             [](Interaction &x, const QString &v) { x.oscAddress = v; });
        whole(QStringLiteral("source/osc/argument"), QStringLiteral("OSC Argument"), 0, 63, 0, [](Interaction &x) -> int & { return x.oscArgument; });
        text(QStringLiteral("source/internal"), QStringLiteral("Follows"),
             [valueRef](const Interaction &x) { return valueRef(x.sourceLayer, x.sourceParam); },
             [parseRef](Interaction &x, const QString &v) { parseRef(v, &x.sourceLayer, &x.sourceParam); });
        number(QStringLiteral("source/audio/low"),QStringLiteral("Audio Low Hz"),20,20000,20,[](Interaction &x)->double &{return x.audioLow;}).limits(20,20000);
        number(QStringLiteral("source/audio/high"),QStringLiteral("Audio High Hz"),20,20000,20000,[](Interaction &x)->double &{return x.audioHigh;}).limits(20,20000);
        choice(QStringLiteral("source/audio/feature"),QStringLiteral("Audio Measure"),{QStringLiteral("band"),QStringLiteral("level"),QStringLiteral("onset")},
            [](const Interaction &x){return int(x.audioFeature);},[](Interaction &x,int k){x.audioFeature=Interaction::AudioFeature(k);});
        text(QStringLiteral("target"), QStringLiteral("Target"), [valueRef](const Interaction &x) { return valueRef(x.layer, x.param); },
             [parseRef](Interaction &x, const QString &v) { parseRef(v, &x.layer, &x.param); });
        text(QStringLiteral("curve"), QStringLiteral("Curve Keys"),
             [](const Interaction &x) { return QString::fromUtf8(QJsonDocument(x.toJson().value("curve").toArray()).toJson(QJsonDocument::Compact)); },
             [](Interaction &x, const QString &v) {
                 const QJsonDocument d = QJsonDocument::fromJson(v.toUtf8());
                 if (!d.isArray()) return;
                 QJsonObject o = x.toJson();
                 o["curve"] = d.array();
                 x.curve = Interaction::fromJson(o).curve;
             });
    }
}
