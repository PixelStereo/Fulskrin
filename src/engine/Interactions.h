#pragma once
// Interactions: a value of the show driven from outside (a key, a MIDI controller, an OSC message) or by another value
// of the show (Internal), without a protocol in between.
//
// An interaction takes its source's value through a chain, in this order:
//  1. the source range: the part of the source's values used, made 0..1 (beyond it: held at its ends);
//  2. toggle: each time it goes up past the middle, the result flips between 0 and 1 (a key, a pad: on, then off);
//  3. a filter, over time: Average (of the last moments), Weighted Average (smoothed), Pulse Detection (a pulse on
//     each sudden rise, decaying), Attack Release (rises and falls at speeds of their own), Damper (a spring that
//     follows, overshooting more or less);
//  4. a curve (0..1 to 0..1, drawn as an animation's keys);
//  5. the master level (scales the whole);
//  6. the target range: where in the target's own values 0..1 lands.
// A number receives it; a switch is on above the middle; a choice takes the one at that place in its list; an action
// (trigger) fires each time it goes up past the middle.
//
// Pick-up (faders, knobs): once the target was moved by something else (a snapshot, the interface), the interaction
// leaves it alone until its own value meets the target's (it is not made to jump). Jump: it sets it at once.
//
// Sources feed events into the engine from any thread (Engine::feedInteraction); the engine runs the chains each frame
// (its lock held) and sets the targets, as an animation does (the snapshots' fades let them go).

#include "Anim.h"

#include <QJsonObject>
#include <QString>
#include <array>
#include <deque>
#include <optional>

enum class InteractionSource { Keyboard = 0, Midi, Osc, Internal };
inline constexpr int kInteractionSourceCount = 4;
enum class InteractionFilter { Average = 0, WeightedAverage, PulseDetection, AttackRelease, Damper };
inline constexpr int kInteractionFilterCount = 5;
enum class MidiMessageKind { ControlChange = 0, Note, PitchBend, ProgramChange };
enum class Takeover { PickUp = 0, Jump };

QString interactionSourceName(InteractionSource s); // "Keyboard", "MIDI", "OSC", "Internal"
QString interactionFilterName(InteractionFilter f); // "Average"…
QString midiMessageName(MidiMessageKind k);         // "CC", "Note", "Pitch Bend", "Program"
// A filter's two settings: their names (empty: not used), their ranges and defaults
struct FilterSetting {
    QString name, suffix;
    double min = 0, max = 1, def = 0;
};
FilterSetting interactionFilterSetting(InteractionFilter f, int which); // which: 0 (A) or 1 (B)

// What came in from a source
struct InteractionEvent {
    InteractionSource source = InteractionSource::Keyboard;
    QString address;   // Keyboard: the key ("Space", "Shift+A"); OSC: the address
    QString port;      // MIDI: the input it came from
    int channel = 0;   // MIDI: 1..16
    MidiMessageKind message = MidiMessageKind::ControlChange;
    int number = 0;    // MIDI: the controller, the note, the program
    int argument = 0;  // OSC: which argument of the message
    double value = 0;  // Keyboard: 1 pressed, 0 released; MIDI: 0..1; OSC: the value as sent
    QString describe() const; // "MIDI · ch 1 · CC 7", "Key Space", "OSC /fader/1"
};

struct Interaction {
    quint64 id = 0;
    QString name; // its own (empty: described by its source and its target)
    bool enabled = true;
    InteractionSource source = InteractionSource::Keyboard;

    // The source
    QString key;          // Keyboard
    QString midiPort;     // MIDI: an input by name ("": any)
    int midiChannel = 0;  // MIDI: 1..16 (0: any)
    MidiMessageKind midiMessage = MidiMessageKind::ControlChange;
    int midiNumber = 0;
    QString oscAddress;   // OSC
    int oscArgument = 0;
    quint64 sourceLayer = 0; // Internal: a value of the show (a layer's id, 0: the composition) and its address
    QString sourceParam;

    // The target: a value of the show (a layer's id, 0: the composition) and its address
    quint64 layer = 0;
    QString param;

    // The chain
    double master = 1;    // 0..1
    bool toggle = false;
    double srcMin = 0, srcMax = 1;
    double dstMin = 0, dstMax = 1;
    bool filterOn = false;
    InteractionFilter filter = InteractionFilter::Average;
    double filterA = 0.2, filterB = 0.3;
    bool curveOn = false;
    AnimTrack curve; // keys over 0..1 (time) to 0..1 (value)
    Takeover takeover = Takeover::Jump;

    bool matches(const InteractionEvent &e) const; // the event is from its source
    QString label() const; // its name, else its source ("Key Space", "MIDI ch 1 CC 7"…)
    // What a snapshot keeps of it: on / off and its chain (not its source, not its target, not its name)
    QJsonObject stateJson() const;
    void applyState(const QJsonObject &state);
    QJsonObject toJson() const;
    static Interaction fromJson(const QJsonObject &o);
    static AnimTrack straightCurve(); // 0 → 0, 1 → 1
    // A new one, set for its kind of source (MIDI and OSC: pick-up; a source range that suits it)
    static Interaction made(InteractionSource s);
};

// The chain of one interaction, from its source's values to the value of its target. Pure: no engine, no clock.
class InteractionProcessor
{
public:
    // A value from the source (in the source's units). Returns its place 0..1 after the range and the toggle.
    void input(const Interaction &it, double raw);
    // One frame of `dt` seconds: the filter moves on. The result 0..1 after the filter, the curve and the master
    // (none: nothing came in yet)
    std::optional<double> step(const Interaction &it, double dt);
    std::optional<double> level() const { return m_level; } // after range and toggle (shown)
    std::optional<double> output() const { return m_out; }  // the last result of step
    std::optional<double> curveInput() const { return m_curveIn; } // what entered the curve (after the filter)
    void reset() { *this = InteractionProcessor(); }

private:
    std::optional<double> m_level;   // after range and toggle
    bool m_toggled = false, m_above = false;
    bool m_fresh = false;            // a value came in since the last step
    double m_y = 0, m_v = 0;         // filter: its value, its speed (Damper)
    bool m_started = false;
    std::deque<std::pair<double, double>> m_window; // Average: (time, value)
    double m_time = 0;
    double m_slow = 0;               // Pulse: the slow average it is compared with
    std::optional<double> m_out, m_curveIn;
};

// Where a target stands for an interaction: what it wrote last, and whether it holds it (pick-up)
struct InteractionTargetState {
    bool caught = false;
    std::optional<double> written, prevOut;
    bool above = false; // trigger: the last output was above the middle
    // A result (0..1) and the target's value now: the value to write, if any (number targets)
    std::optional<double> numberToWrite(const Interaction &it, double out, double current, double tolerance);
};
