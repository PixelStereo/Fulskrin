#pragma once
// Render engine: depends only on QtCore/QtGui/OpenGL, no dependency on widgets.
//
// Two modes:
//  - dedicated thread (start()): a QThread owns the OpenGL context, renders the composition and presents
//    it itself in the output window, locked to vertical sync. The UI may freeze
//    (dialog, loading) without the output stopping;
//  - manual (no start()): renderFrame() is called by the thread that owns the engine (tests, command-line rendering).
//
// Concurrency:
//  - composition data (layers, parameters, mapping, composition) is protected by mutex();
//    the UI takes Engine::Lock before reading or writing a Layer directly;
//  - anything touching OpenGL (shader compilation, texture release…) goes through runGl(),
//    which runs it on the render thread. These tasks never take the lock: it is safe to wait
//    on a task while holding the lock, with no risk of deadlock.

#include "TestCard.h"
#include "PresentMetal.h"
#include "Sequencer.h"
#include "FrameTiming.h"
#include "AudioOutput.h"
#include "IsfLibrary.h"
#include "Layer.h"
#include "LayerTree.h"
#include "Publish.h"
#include "Publishing.h"
#include "TimecodeOutputs.h"
#include "Interactions.h"

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QRecursiveMutex>
#include "AnnotatedMutex.h"
#include <limits>
#include <QSet>
#include <QSize>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <mutex>

class QOpenGLContext;
class QOffscreenSurface;
class QSurface;
class QThread;
class QWindow;
class QTimer;

namespace midi {
class Input;
}

class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    using Mutex = Annotated<QRecursiveMutex>; // ThreadSanitizer sees what it orders (AnnotatedMutex.h)
    using Lock = QMutexLocker<Mutex>;
    Mutex &mutex() { return m_mutex; }

    bool initialize(QString *err);
    bool start();          // starts the render thread (returns false if the platform does not support it)
    void stop();
    // Emergency stop: cancel every sequence/WAIT, fade, running animation and media transport;
    // retain the currently visible parameter values (no snap back to an earlier cue).
    void stopShowPlayback();
    bool isThreaded() const { return m_threaded; }
    // The render thread could not make its OpenGL context current: nothing is rendered (start() again to retry)
    bool renderFailed() const
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        return m_renderFailed;
    }
    static void failNextRenderStartForTests() { s_failRenderStart = true; }
    // One-shot simulation of a missing Difference-blend copy target; only
    // used by the rendering regression tests, no GPU resources destroyed.
    static void failNextDifferenceCopyForTests() { s_failDifferenceCopyForTests = true; }
    void shutdown();

    // Runs fn on the render thread with the OpenGL context current (or immediately in manual mode).
    void runGl(std::function<void()> fn, bool wait = true);

    // --- Composition (take Lock to access Layer objects)
    QSize compositionSize() const;
    void setCompositionSize(QSize s);

    int layerCount() const;
    Layer *layer(int i);
    int indexOf(const Layer *l) const;

    int addLayer(const QString &name = {}, int at = 0); // index 0 = top layer
    int addGroup(const QString &name = {}, int at = 0);  // empty group (members are moved in with setStructure)
    // Removes a layer; a removed group's members go back to the top level.
    void removeLayer(int i);
    void moveLayer(int from, int to);
    // Duplicates a layer before itself (a group with its members); returns the index of the copy (the group's).
    int duplicateLayer(int i);

    // --- Structure: order and groups (see LayerTree.h). Members immediately follow their group.
    quint64 layerId(int i) const;
    int indexOfId(quint64 id) const;
    LayerTree structure() const;
    void setStructure(const LayerTree &t); // reorders and regroups (no layer is created or destroyed)
    int groupIndexOf(int i) const;         // index of the group containing layer i, -1 at the top level
    QList<int> groupMembers(int group) const;
    bool isLocked(int i) const;            // locked itself, or member of a locked group
    // Solo: by index, the layers another layer's solo leaves out (none when nothing is soloed). Lock held.
    std::vector<bool> soloLeftOut() const;
    // A layer's masks (Masks.h), by its id. Setting them redraws them.
    std::vector<Mask> layerMasks(quint64 id) const;
    void setLayerMasks(quint64 id, const std::vector<Mask> &m);
    // The frame a layer's surface masks are drawn in, in pixels (its picture; a group: the composition; a viewport:
    // its size)
    QSize maskFrame(quint64 id) const;

    // --- Viewports: windows onto the composition, each with its size in pixels, its screen and its
    // publishing. Placed in the composition by their Spatial (mapping bounds); never inside a group.
    int addViewport(const QString &name = {}, QSize size = {}); // index of the new viewport
    bool isViewport(int i) const;
    QList<int> viewports() const;          // indices, in list order (they come first)
    quint64 mainViewportId() const;        // the first one
    void setViewportSize(int i, QSize size);
    void setViewportOutput(int i, const QString &screen, int mode); // mode: 0 hidden, 1 windowed, 2 fullscreen
    // An item at the top of the list can be left out of some viewports (shown everywhere by default)
    void setOpacityIn(int i, quint64 viewport, float opacity); // how much of the item a viewport shows (0..1)
    void ensureViewport(); // there is always at least one

    // JSON snapshot of a layer, and re-creation (undo / redo, duplicate)
    QJsonObject layerJson(int i) const;
    int insertLayerJson(int at, const QJsonObject &o);
    void replaceLayerJson(int i, const QJsonObject &o);
    QJsonArray effectsJson(int i) const;
    void setEffectsJson(int i, const QJsonArray &a);

    // Parts of a layer state (layerJson) that can be copied onto another layer
    enum LayerParts {
        PartSource = 1 << 0,      // media or generator, transport, sound (the media is opened again)
        PartRoi = 1 << 1,         // part of the source picture used
        PartColor = 1 << 2,       // balance, added / removed colors and their switches
        PartSpatial = 1 << 3,     // mapping: corners and mesh
        PartEffects = 1 << 4,     // ISF chain and its enable switch
        PartCompositing = 1 << 5, // opacity, blend mode
        PartMasks = 1 << 6,       // its shape masks (drawn in the preview)
        PartViewports = 1 << 7,   // how much of it each viewport shows
        PartAnimations = 1 << 8,  // the animations of its values
        PartTransport = 1 << 9,   // speed, play mode, in / out, played or paused, volume, mute (the media stays)
        PartAll = PartSource | PartRoi | PartColor | PartSpatial | PartEffects | PartCompositing | PartMasks |
                  PartViewports | PartAnimations | PartTransport,
    };
    // Copies the chosen parts of `o` (a layerJson, from any layer or project) onto layer i, leaving its
    // identity (id, name, group, place in the list) alone. False if the layer is missing or locked.
    static int partOf(const QString &path); // the part (Part…) a parameter of a layer belongs to, by its address
    bool applyLayerParts(int i, const QJsonObject &o, int parts);

    bool setLayerVideo(int i, const QString &path, QString *err = nullptr);
    bool setLayerImage(int i, const QString &path, QString *err = nullptr);
    // What a shader that does not load (missing, does not compile) does to the layer:
    //  - Refuse (an operator replacing a source, live): nothing changes, the source shown stays; false and err;
    //  - KeepWithError (a project opened, an undo, a snapshot): it takes its place all the same, with its path, its
    //    values and its error (black, or the image passed through for an effect), so that nothing of the show is lost
    //    on the next save and it works again once the shader is there
    enum class IsfFailure { Refuse, KeepWithError };
    bool setLayerIsf(int i, const QString &path, QString *err = nullptr, IsfFailure onFailure = IsfFailure::Refuse);
    bool setLayerAudio(int i, const QString &path, QString *err = nullptr);
    // Video, image or audio according to the file: a video file without a picture becomes an audio layer.
    bool setLayerFile(int i, const QString &path, QString *err = nullptr);
    // Another layer of the composition as the source, tapped before or after its effect chain.
    // Refused (with err) for a group, for the layer itself, and whenever the picture would feed back on itself.
    bool setLayerSourceLayer(int i, quint64 sourceId, LayerTap tap, QString *err = nullptr);
    bool setLayerTap(int i, LayerTap tap);
    // True when the picture of layer `id` depends on layer `onId` (source references, members of a group).
    // The lock must be held.
    bool layerDependsOn(quint64 id, quint64 onId) const;
    // Drops the layer sources that point nowhere or would feed back (after a project is read)
    void fixLayerReferences(QStringList *warnings = nullptr);
    void clearLayerSource(int i);
    void setGeneratorSize(int i, int w, int h);

    void setLayerPlaying(int i, bool playing);
    void setLayerPlayMode(int i, PlayMode mode);
    void setLayerSpeed(int i, double speed); // negative: backwards (changing direction keeps the position)
    // In / out points (seconds; out < 0: end of the media). Playback, loops and ping-pong stay within them.
    void setLayerInOut(int i, double in, double out);
    // Color models shown by the Color tab of a new layer (preference); each layer then keeps its own
    void setDefaultColorModels(int m) { m_defaultColorModels = m; }
    // Mode given to a video or a sound when it is loaded into a layer (preference)
    void setDefaultPlayMode(PlayMode m) { m_defaultPlayMode = m; }

    // --- Rendering: frame rate and antialiasing. A project chooses them (Composition tab), or takes the machine's
    // defaults (Settings): -1 in a project means "the default".
    struct RenderSettings {
        double frameRate = -1; // frames per second; 0: the refresh rate of the screen
        int samples = -1;      // multisampling of the mapped edges: 0 / 1 off, 2, 4, 8
        int mipmaps = -1;      // smooth pictures drawn smaller than they are (mipmaps): 0 off, 1 on
        int depth = -1;        // bits per channel of the render: 10 (RGBA16, the default) or 8 (lighter)
    };
    void setRenderSettings(const RenderSettings &project); // saved with the project
    RenderSettings renderSettings() const;
    void setRenderDefaults(const RenderSettings &machine); // no -1 there
    RenderSettings renderDefaults() const;
    RenderSettings effectiveRender() const; // what is used: the project's, or the defaults
    void setScreenRefreshRate(double hz);   // of the main screen (the pace without a window shown)
    double screenRefreshRate() const;
    // Transition used when a snapshot gives a layer another source, for the layers that do not choose one
    // (ISF transition; empty: a crossfade)
    void setDefaultTransition(const QString &path);
    QString defaultTransition() const;
    bool isTransitioning(quint64 layer) const; // a source transition is running on it
    // The ISF shader the running transition of `layer` mixes with (empty: a crossfade, always for a sound)
    QString transitionShader(quint64 layer) const;
    // Mask of an effect: the picture of another layer (0: none) says where the effect applies. Refused for a
    // viewport, the layer itself, or a layer that already depends on this one (err says why).
    bool setEffectMask(int layer, int effect, quint64 maskLayer, bool invert, QString *err = nullptr);
    bool setEffectMaskTap(int layer, int effect, bool preFx); // the mask's picture: before or after its own effects
    // Where the color section applies: another layer's picture (0: everywhere). Same rules as an effect's mask.
    bool setColorMask(int layer, quint64 maskLayer, bool invert, QString *err = nullptr);
    PlayMode defaultPlayMode() const { return m_defaultPlayMode; }
    void seekLayer(int i, double t);
    void setLayerVolume(int i, float volume);
    void setLayerMuted(int i, bool muted);

    // Text layer properties
    bool setLayerText(int i); // the layer's source becomes the Text generator
    // The Timecode generator: a text source that shows its clock (TimecodeClock.h). Setting it again with other
    // settings keeps its text style; its clock (and its devices) start again.
    bool setLayerTimecode(int i, const TimecodeSettings &s);
    std::optional<TimecodeSettings> layerTimecode(int i) const; // none: not a Timecode generator
    QString layerTimecodeStatus(int i) const;
    void restartLayerTimecode(int i); // free run: from its in point, playing
    std::shared_ptr<TimecodeClock> layerTimecodeClock(int i) const; // its transport
    int timecodeOutputUser(quint64 output) const; // the layer driving a timecode output (-1: none)

    // --- Timecode outputs (TimecodeOutputs.h): named outputs the Timecode layers drive, patched to destinations that
    // give the protocol (a MIDI output: MTC; an audio channel: LTC; a viewport: ATC in its NDI and DeckLink pictures). The
    // patch: named MIDI outputs given a port each, named audio channels given a device and a channel each
    std::vector<TimecodeOutput> timecodeOutputs() const;
    quint64 addTimecodeOutput(const QString &name = QString()); // "Timecode 1", "Timecode 2"… by default
    // Its name and its routes, by its id (false: no such). Routes to nothing, or to a destination another output
    // has, are left out
    bool setTimecodeOutput(const TimecodeOutput &o);
    void removeTimecodeOutput(quint64 id);                     // the layer driving it drives none
    QString timecodeOutputStatus(quint64 id) const;            // who drives it and where it goes
    QString routeName(const QString &route) const;             // the destination's name ("" : none)
    std::vector<MidiOutput> midiOutputs() const;
    quint64 addMidiOutput(const QString &name = QString());     // "MIDI 1"…, no port yet
    bool setMidiOutput(const MidiOutput &o);
    void removeMidiOutput(quint64 id);                         // and the routes to it
    QString midiOutputStatus(quint64 id) const;                // "Sending MTC", "Port not found"…
    std::vector<AudioChannelOutput> audioChannelOutputs() const;
    quint64 addAudioChannelOutput(const QString &name = QString()); // "LTC 1"…, no device yet
    bool setAudioChannelOutput(const AudioChannelOutput &o);
    void removeAudioChannelOutput(quint64 id);                 // and the routes to it
    QString audioChannelOutputStatus(quint64 id) const;        // "Sending LTC", "Channel 3: … has 2"…
    // --- Interactions (Interactions.h): values of the show driven by a key, a MIDI controller, an OSC message, or
    // another value of the show. Run each frame after the animations (they win over the snapshots' fades)
    std::vector<Interaction> interactions() const;
    void setInteractions(const std::vector<Interaction> &list); // the whole list (undo); ids 0 are given one
    quint64 addInteraction(const Interaction &it);              // its id
    bool setInteraction(const Interaction &it);                 // by its id
    void removeInteraction(quint64 id);
    // What it does now (shown): its source after the range and the toggle, and its result (0..1); false: nothing yet
    // curveIn: what enters its curve (after the filter)
    bool interactionLevels(quint64 id, double *level, double *output, double *curveIn = nullptr) const;
    void feedInteraction(const InteractionEvent &e); // a source's value (any thread)
    // An enabled OSC interaction listens on this exact address: it owns it. OSC then sends the message to the
    // interaction only, not to the native value of that address as well (one writer per value: an interaction that
    // preserves alpha is not undone by the direct write landing first). Any thread.
    bool oscAddressClaimed(const QString &address) const;
    // Learn: the sources that come in are kept (of one kind, when given), for the interface to take once they have
    // settled — several moved together (a controller sending two messages): it asks which one. The interactions go on
    void setInteractionLearn(bool on, std::optional<InteractionSource> kind = std::nullopt);
    bool interactionLearning() const;
    // The distinct sources heard, once `settleMs` have passed since the first (none yet: empty); taken, they are cleared
    std::vector<InteractionEvent> takeLearnedInteractions(int settleMs = 300);

    // The hardware of this machine to patch to (looked up on each call)
    static QStringList midiPorts();        // MIDI outputs
    static QStringList midiInputPorts();   // MIDI inputs (MTC in)
    static bool midiVirtualPorts();        // virtual MIDI ports can be made (macOS, Linux)
    static QStringList audioOutputDevices();
    static QStringList audioInputDevices(); // (LTC in)
    TimecodeRouter &timecodeRouter() { return *m_tcRouter; }   // tests
    // A live input (LiveInput.h) as the layer's source: a camera, an NDI or OMT source, a Syphon server
    bool setLayerInput(int i, const QString &uri, QString *err = nullptr);
    static std::vector<LiveInputInfo> liveInputs(const QString &libraryFolder = QString()) { return listLiveInputs(libraryFolder); }
    void setLayerTextContent(int i, const QString &text);
    void editLayerText(int i, const std::function<void(Layer &)> &edit); // style (bold, outline, shadow…), under the lock

    // -1 when refused (see IsfFailure); with KeepWithError, a broken effect joins the chain and passes the image through
    int addEffect(int layerIndex, const QString &path, QString *err = nullptr, IsfFailure onFailure = IsfFailure::Refuse);
    void removeEffect(int layerIndex, int fx);
    void moveEffect(int layerIndex, int from, int to);
    bool setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err = nullptr);
    bool reloadIsf(IsfInstance *inst); // reloads from disk (live editing)

    // --- Snapshots (cues, as in MadMapper): snapshots of the layers, recalled with a fade
    struct Snapshot {
        quint64 id = 0;     // stable (sequences refer to it); given by addSnapshot
        QString name;
        double fade = 1.0;  // seconds
        QImage thumbnail;   // output at the time it was stored
        QJsonArray layers;  // layerJson of the layers, with "included" (false: left alone by the recall)
        // The composition: its opacity and the sound volume, with "included" and "timing" as a layer's
        // (empty: a snapshot stored before it was kept, left alone by the recall)
        QJsonObject composition;
        // The interactions: each one's values (on / off, master level, ranges, filter, curve), by its id, with its
        // name, "included", "timing" and "easing" as a layer's: [{"id", "name", "included", "params", "timing",
        // "easing"}]
        // (one it does not hold, made since, is switched off by the recall)
        QJsonArray interactions;
    };
    QJsonObject captureComposition() const;
    void applyComposition(const QJsonObject &c, double fade); // the recall's part for the composition
    QJsonArray captureInteractions() const; // every interaction's values, all included
    // The recall's part for the interactions: the numbers (master level, ranges, a filter's settings) fade in their
    // time, the rest is set at once; one removed since is made again; one left out is left alone; one made since
    // (not among them) is switched off
    void applyInteractions(const QJsonArray &states, double fade);
    // Values of one interaction (keys: "master", "enable"… — or their whole address "interaction/<id>/master"), faded
    // as a snapshot's (lock held)
    void applyInteractionValues(quint64 id, const QJsonObject &params, const QJsonObject &timing, const QJsonObject &easing,
                                double fade);
    static QStringList interactionKeys(); // the keys of an interaction's values, in their order
    int snapshotCount() const;
    Snapshot snapshot(int i) const;
    void setSnapshot(int i, const Snapshot &m);
    int addSnapshot(const Snapshot &m, int at = -1);
    void removeSnapshot(int i);
    // State of every layer, all included; for a snapshot, without the values their animations drive (the
    // animations, held with their state, give them); withAnimated: every value (the undo of a recall)
    QJsonArray captureLayers(bool withAnimated = false) const;
    // Undo snapshot recalls from the cue clock: serialize only existing layers
    // the recall can actually modify, rather than every layer in the show.
    QJsonArray captureRecallLayers(int snapshotIndex) const;
    // Applies layer states (those not excluded): opacity, volume, roi, color, mapping and ISF numbers fade in
    // `fade` seconds, or in the time a state gives them ("timing": key → seconds, 0 a cut); sources and effect chains change at once; a layer that became visible fades in from 0,
    // one that becomes hidden fades out; layers removed since are recreated. Other layers are left alone, or with
    // hideOthers (a snapshot's recall: the picture as it was stored) faded out and hidden — not the viewports, not
    // the layers the state holds but leaves out ("included": false), not the locked ones.
    void applyLayers(const QJsonArray &layers, double fade, bool hideOthers = false);
    int indexOfSnapshot(quint64 id) const; // -1: none
    // How the snapshots are arranged: groups (nested) and snapshots, in order. The snapshots' own order (their indexes,
    // their numbers) is this tree's, depth first. A snapshot node has `snapshot` (its id); a group, `group` (its id)
    struct SnapshotNode {
        quint64 snapshot = 0;
        quint64 group = 0;
        QString name; // a group's
        bool open = true;
        std::vector<SnapshotNode> children;
        bool operator==(const SnapshotNode &o) const;
    };
    std::vector<SnapshotNode> snapshotTree() const;
    // Rearranged (dragged, grouped): a group without an id gets one; a snapshot missing from it goes at the end, one
    // it does not have is dropped; the snapshots' order follows
    void setSnapshotTree(const std::vector<SnapshotNode> &tree);
    quint64 newSnapshotGroupId(); // for a group about to be put in the tree

    // --- Presets: a few values of the layers (and of the composition), chosen one by one, recalled with a fade as a
    // snapshot's values are. By layer id (0: the composition) and address; each with its own time (< 0: the preset's
    // fade, 0: a cut) and easing (empty: the default)
    struct PresetValue {
        quint64 layer = 0;
        QString path;
        QJsonValue value;
        double time = -1;
        QString easing;
    };
    struct Preset {
        quint64 id = 0;
        QString name;
        double fade = 1.0;
        std::vector<PresetValue> values;
        // Definitions captured with the selected values, not merely their current output.
        std::vector<Animation> animations;
        std::vector<Interaction> interactions;
        int indexOf(quint64 layer, const QString &path) const; // -1: not in it
    };
    int presetCount() const;
    std::vector<Preset> presets() const;
    void setPresets(const std::vector<Preset> &all); // as they are (an undo)
    Preset preset(int i) const;
    void setPreset(int i, const Preset &p);
    int addPreset(const Preset &p, int at = -1); // returns where it went
    void removePreset(int i);
    int indexOfPreset(quint64 id) const;
    void recallPreset(int i);
    // A value as it is now, to put in a preset (an undefined value: it has none at that address)
    PresetValue presetValue(quint64 layer, const QString &path) const;

    // --- Timelines: values of the layers (and of the composition) drawn over time, played on their own clock,
    // outside the sequences — the sequences only drive their transport (play, pause, stop, rewind, seek, loop).
    // Called animations here (a Timeline is a media's transport); the interface names them timelines.
    //
    // A timeline has a duration and tracks, each driving one number: a curve (keys over the duration, each
    // eased towards the next one — the pattern repeats when the timeline loops) or an oscillator (a wave of its
    // own period, on the time played so that it goes on without a jump across the loops). Values are absolute.
    // While a timeline plays, its values are set every frame after the snapshots' fades: they win over them.
    // Paused or stopped, it leaves its values where they are. Several timelines play side by side.
    using AnimWave = ::AnimWave; // the types are in Anim.h: a layer holds animations of its own numbers too
    using AnimLoop = ::AnimLoop;
    using AnimState = ::AnimState;
    using AnimAction = ::AnimAction;
    static constexpr int kAnimHold = ::kAnimHold;
    static constexpr int kAnimBezier = ::kAnimBezier;
    using AnimKey = ::AnimKey;
    using AnimTrack = ::AnimTrack;
    using Animation = ::Animation;
    using AnimParam = ParamInfo; // a number a timeline can drive (Parameter.h)
    int animationCount() const;
    Animation animation(int i) const;
    int indexOfAnimation(quint64 id) const; // -1: none
    int addAnimation(const Animation &a, int at = -1);
    void setAnimation(int i, const Animation &a); // its contents; where it is stays
    void removeAnimation(int i);
    // The transport: Play (from where it is; from the start once stopped), Pause, Stop (back to the start,
    // its values left as they are), Rewind (to the start), Seek (to `time`, values set at once even when not
    // playing), LoopMode (`loop` and `repeat`), Speed (`time` is the speed, 0 to 10: it plays on from where it is)
    void controlAnimation(quint64 id, AnimAction action, double time = 0, AnimLoop loop = AnimLoop::Loop, int repeat = 0);
    // The numbers of a layer (0: the composition) a timeline can drive, with their ranges
    std::vector<AnimParam> animatableParams(quint64 layer) const;
    // The parameters of a layer (0: the composition), as they declare themselves (Parameter.h)
    std::vector<ParamInfo> parameters(quint64 layer) const;
    bool parameterInfo(quint64 layer, const QString &path, ParamInfo *p) const; // false: it has none at that address
    bool animParamValue(quint64 layer, const QString &path, double *value) const;
    // One value, by its layer's id (0: the composition) and its address: every edit by hand (the interface and its
    // undo) goes through here. Set, a fade that drives it lets go. Undefined / false: it has none at that address
    QJsonValue parameterValue(quint64 layer, const QString &path) const;
    bool setParameter(quint64 layer, const QString &path, const QJsonValue &v);
    // Where a layer's shader keeps its values: "" its generator (slot -1), "fx/<fx>/" an effect
    QString shaderPrefix(quint64 layer, int slot) const;
    void stepAnimations(double dt); // the layers' animations, then the timelines, move on and set their values (lock held)

    // --- Animations of a layer's numbers (Layer::anims, the Anim tab of the inspector): each one drives one number of
    // its layer (its single track, on that layer) with its own duration, loop, speed and transport. One that is on
    // plays from the moment it is made (and when the project is opened); off, it leaves its number where it is. They
    // are set every frame after the snapshots' fades and before the timelines (a timeline playing the same number
    // wins), on a locked layer too (they are its content). They belong to the layer (saved, duplicated, undone with
    // it), not to the snapshots: a recall leaves them as they are.
    std::vector<Animation> layerAnims(quint64 layer) const;
    bool layerAnim(quint64 layer, const QString &param, Animation *a) const; // false: that number is not animated
    // The layer's whole list: an animation already there (the same number) goes on from where it is; one that is new,
    // or turned on, starts from the beginning; one turned off stops
    void setLayerAnims(quint64 layer, const std::vector<Animation> &anims);
    // One of them (by its number): replaced, added at `at` (-1: at the end), or removed (a: null); the others are left
    // as they are
    void setLayerAnim(quint64 layer, const QString &param, const Animation *a, int at = -1);
    void setLayerAnimOn(quint64 layer, const QString &param, bool on); // on (from the start) or off
    int layerAnimIndex(quint64 layer, const QString &param) const;      // its place in the list (-1: none)
    // How its card is shown (pinned at the top of the Anim tab, folded): saved, not an edit (setLayerAnims keeps them)
    void setLayerAnimView(quint64 layer, const QString &param, bool pinned, bool folded);
    // Its transport (as controlAnimation; LoopMode takes `loop` and `repeat`)
    void controlLayerAnim(quint64 layer, const QString &param, AnimAction action, double time = 0,
                          AnimLoop loop = AnimLoop::Loop, int repeat = 0);
    // A new animation of a number, as the Animate menu makes it: a wave (`wave`: an AnimWave) around the number's
    // value, or (`wave` < 0) keys starting from it. Not added: see setLayerAnims.
    Animation makeLayerAnim(quint64 layer, const QString &param, int wave) const;
    // How a wave goes around a number by default (layer 0: the composition): its center and how far each way — around
    // its value, as far as its range allows, or the whole range from a bound; a turn for a rotation, a tenth of the
    // composition for a position
    void waveAround(quint64 layer, const QString &param, double *center, double *amplitude) const;

    // --- Sequences (the cue lists): Sequencer.h
    Sequencer &sequencer() { return *m_sequencer; }
    const Sequencer &sequencer() const { return *m_sequencer; }
    // A snapshot's action: its longest time (the fade, and the times of its own values)
    double snapshotDuration(quint64 id) const;
    // A step's action: its snapshot's, or the time its timeline will play (Play on a timeline that ends; else 0;
    double animationRemaining(quint64 id) const; // real seconds a timeline playing has left (infinity: not moving)
    // Real seconds a timeline told to play now will play (0: endless or gone; infinity: frozen, speed 0)
    double animationPlayTime(quint64 id) const;
    void recallSnapshot(int i); // with its fade
    bool isFading() const;
    // The snapshot recalled last, and where its fade is (its longest time: values with times of their own included)
    struct RecallProgress {
        quint64 snapshot = 0;          // 0: none recalled since the project was opened
        double elapsed = 0, total = 0;
        bool running() const { return snapshot && total > 0 && elapsed < total; }
        double fraction() const { return total > 0 ? std::min(1.0, elapsed / total) : 1.0; }
    };
    struct FadeJob; // a layer on its way to a snapshot's state (Snapshots.cpp)
    RecallProgress recallProgress() const;
    void stepCompositionFade(double dt); // a snapshot's fade of the level and the volume (lock held)
    void stepTypewriters(double dt); // the texts snapshots gave are typed on (lock held)
    void advanceFades(double dt); // tests: moves the fades on by dt seconds, as a rendered frame does
    void setFadesManual(bool on) { m_fadesManual = on; }
    bool fadesManual() const { return m_fadesManual; } // tests: only advanceFades moves them, not the frames
    // A snapshot gives a value of a layer its time ("timing": key → seconds, 0 at once) and its easing ("easing": key →
    // easing key) under its address or the address of a group it is in ("position" for "position/x", "fx/blur" for
    // that effect's); the other keys: "file" (another source's transition), "viewports" (its routing), "mesh" (its
    // points). The key that gives `path` its time in `timing` (or its easing in `easing`), empty when none does: it
    // follows the snapshot's fade
    static QString timingKeyOf(const QJsonObject &timing, const QString &path);
    // The easings of the fades — and of a timeline's keys, the same (0..5) — by key ("ease_in_out") and by name
    static QStringList easingKeys();
    static QStringList easingNames();
    // The easing of a value a snapshot gives no easing (by its timing key): in-out, the typing of a text even
    static QString defaultEasing(const QString &timingKey);

    // --- External media (media bin)
    struct MediaRef {
        QString path;
        bool video = false;      // video file
        bool audio = false;      // audio file (neither: image)
        bool missing = false;
        bool imported = false;   // added to the media bin by the user
        QStringList users;       // "Layer" or "Layer › FX"
    };
    std::vector<MediaRef> mediaUsage() const;
    QStringList binItems() const;
    static bool isIsfFile(const QString &path); // .fs / .frag
    void addBinItems(const QStringList &paths);
    void removeBinItem(const QString &path);
    QList<int> layersUsingMedia(const QString &path) const;
    // Replaces file `from` with `to` in a layer (source and ISF image inputs). Returns true if changed.
    bool relinkLayerMedia(int layer, const QString &from, const QString &to, QString *err = nullptr);
    void relinkBinItem(const QString &from, const QString &to);
    static bool isVideoFile(const QString &path);
    static bool isImageFile(const QString &path);
    static bool isAudioFile(const QString &path);
    static QStringList videoExtensions();
    static QStringList imageExtensions();
    static QStringList audioExtensions();

    // --- Sound: one output device, mixing the audio of every layer
    bool startAudio(const QString &device, QString *err, bool nullDevice = false); // empty = system default
    void stopAudio();
    AudioOutput &audioOutput() { return *m_audio; }
    float audioVolume() const { return m_audio->volume(); }
    void setAudioVolume(float v); // stops a snapshot's fade of the volume

    // --- Output publishing (NDI, OMT, Syphon, Spout): each viewport publishes its own picture
    void setPublishSettings(quint64 viewport, const PublishSettings &s);
    PublishSettings publishSettings(quint64 viewport) const;
    PublishState publishState(quint64 viewport, PublishKind k) const;
    // Tests: receives the read-back frames (send thread)
    void setTestTap(std::function<void(const CpuFrame &)> fn);
    // Tests: Syphon / Spout publishers made by `f` on any platform (Publishing::setGpuPublisherFactoryForTests)
    void setGpuPublisherFactoryForTests(std::function<std::unique_ptr<GpuPublisher>(PublishKind)> f);

    // --- Composition opacity: 0..1 reached in `seconds` seconds (picture only)
    void fadeCompositionOpacity(double target, double seconds);
    // Speed of the whole composition (0..10): a coefficient on every pace — media, shaders' TIME, timelines, fades,
    // sequences. Pause holds it at 0 and gives the speed back when released.
    double compositionSpeed() const { return m_compositionSpeed.load(); }
    void setCompositionSpeed(double speed);
    bool paused() const { return m_paused.load(); }
    void setPaused(bool on);
    double compositionOpacity() const { return m_compositionOpacity.load(); }
    double compositionOpacityTarget() const;
    // Blackout: fades the picture and the sound out (and back in) in `seconds` seconds
    void setBlackout(bool on, double seconds);
    void setBlackout(bool on) { setBlackout(on, blackoutFade()); }
    bool blackout() const;
    double blackoutLevel() const { return m_blackLevel.load(); }
    void setBlackoutFade(double seconds);  // default fade duration
    double blackoutFade() const;
    double outputLevel() const { return compositionOpacity() * blackoutLevel(); }

    // --- Source preview (roi editor): a downscaled copy of a layer's source picture (before roi),
    // read back by the render thread every few frames while requested.
    void requestSourcePreview(quint64 layerId, int maxSide = 360); // 0: stop
    QImage sourcePreview(quint64 *layerId = nullptr) const;

    // --- Output: window in which the render thread presents the composition
    // One window per viewport; the render thread presents the viewport's picture into it
    // macOS: the output windows are shown through Metal (their surface type must then be Metal), the engine rendering
    // on its own thread; elsewhere (or without Metal) through OpenGL
    static bool metalOutput() { return MetalOutput::available(); }
    QString glRenderer() const { return m_glRenderer; } // the OpenGL renderer's name
    QString glVersion() const { return m_glVersion; }
    QString glVendor() const { return m_glVendor; }
    quint64 windowFrames(quint64 viewport); // pictures the viewport's window was given (tests)
    void setViewportWindow(quint64 viewport, QWindow *w);
    // refreshHz: its screen's (0: not known). The render keeps that pace while the window is shown, and counts its
    // missed deadlines against it, not against the main screen's
    void setViewportExposed(quint64 viewport, bool exposed, QSize pixelSize, double refreshHz = 0);

    // --- Rendering
    void renderFrame();                  // manual mode only
    GLuint outputTexture() const;        // last published frame (readable from a shared context)
    // Another context reading the published pictures calls this first, with itself current: its GPU waits for the
    // frame to be drawn (the CPU does not)
    void waitForFrame(QOpenGLExtraFunctions *f) const;
    double fps() const { return m_fps.load(); }
    // Frame timing (FrameTiming.h): intervals, missed deadlines, compute and present times, OSC / cue to frame
    FrameTiming::Stats frameTiming() const { return m_frameTiming.stats(); }
    FrameTiming::Window takeFrameTimingWindow() { return m_frameTiming.takeWindow(); }
    QJsonObject frameTimingJson() const; // the same, with the outputs' dropped frames (OSC /composition/timing)
    void resetFrameTiming();
    void noteInput(FrameTiming::Input kind) { m_frameTiming.noteInput(kind); } // any thread
    // Any thread: what the show did (snapshot recalled, timeline driven, layer sought), for the endurance journal
    void journal(const QString &kind, const QString &detail) { m_frameTiming.noteEvent(kind, detail); }
    // What the show holds against its budgets (any thread): decoded frames, upload buffers, decoders' threads,
    // graphics memory free (when the driver says: NVIDIA, AMD), and which budget is exceeded now ("warnings").
    // OSC /composition/budget. Sampled by the render thread once a second; a budget newly exceeded is logged.
    QJsonObject resourcesJson() const;
    // Syphon/Spout: true (default) publishes the prior complete viewport picture using
    // a protected third buffer; false waits for GPU completion with glFinish (legacy mode).
    void setGpuShareOneFrameLate(bool on) { m_shareOneFrameLate = on; }
    bool gpuShareOneFrameLate() const { return m_shareOneFrameLate.load(); }
    quint64 mipmapsMade() const { return m_mipmapsMade.load(); } // layers' mipmaps generated since the start
    QImage grabOutput();                 // the whole composition (interface preview)
    QImage grabViewport(quint64 viewport);
    QImage grabLayerSource(int index); // a layer's source picture, full size, before its ROI (tests, tools)
    quint64 frameCount() const { return m_frameCount.load(); }
    // Video frames a layer has shown so far (uploaded to the GPU): its playback rate, measured
    quint64 videoFramesShown(int index) const;
    // Compressed texture formats the GPU samples (HAP): DXT, and BC7 / BC6 (not on macOS)
    bool gpuSamplesDxt() const { return m_videoConv.s3tc(); }
    bool gpuSamplesBptc() const { return m_videoConv.bptc(); }

    // --- Project
    void newProject();   // empty, with one viewport
    bool saveProject(const QString &path, const QJsonObject &uiState, QString *err);
    QJsonObject projectToJson(const QString &projectDir, const QJsonObject &uiState) const; // what saveProject writes

    // --- Collect Files (ProjectCollect.cpp): the project and every file it uses gathered in one folder,
    // <folder>/<name>.pinol and <folder>/MEDIA/{MOVIES, PICTS, AUDIO, ISF, OTHER}, the paths of the project (layers,
    // effects, ISF images, transitions, snapshots, media bin) pointing there. ISF shaders come with their vertex
    // shader and the images they import (a shader that imports some: in a folder of its own, ISF/<name>/)
    struct CollectFile {
        QString from, to; // absolute paths
        qint64 bytes = 0;
    };
    struct CollectPlan {
        QString folder, projectFile;
        std::vector<CollectFile> files;
        QStringList missing; // referenced, not found: their paths are kept as they are
        QStringList skipped; // found, not collected (an ISF image outside its shader's folder)
        qint64 bytes = 0;
        QJsonObject project; // the project as it will be written there
    };
    // What would be done (nothing is written): `parent` the folder that receives `name` (a new folder)
    CollectPlan planCollect(const QString &parent, const QString &name, const QJsonObject &uiState) const;
    // Copies the files and writes the project. Fails (and removes what it wrote) if the folder exists already or a
    // copy fails. The originals are left: removeCollectedOriginals deletes them (Move), once the collected project
    // is the one open
    bool collect(const CollectPlan &plan, QString *err);
    static int removeCollectedOriginals(const CollectPlan &plan, QStringList *failed = nullptr);
    bool loadProject(const QString &path, QJsonObject *uiState, QString *err);
    // A project used as a template (the startup project of Settings ▸ Interface): its content loaded, but untitled —
    // "Save" asks where, the file it came from is never written over
    bool loadProjectAsTemplate(const QString &path, QJsonObject *uiState, QString *err);
    QString projectPath() const;
    void setProjectPath(const QString &p);

    IsfLibrary &library() { return m_library; }

signals:
    void layersChanged();
    void compositionSizeChanged(QSize size);
    void snapshotsChanged();
    void presetsChanged();
    void animationsChanged();       // the timelines' contents (not their transport)
    void layerAnimsChanged(quint64 layer); // the animations of a layer's numbers (their contents, not their transport)
    void snapshotRecalled(int index);
    void timecodeRoutingChanged(); // timecode / MIDI outputs, their patch, or which layer drives which output
    void interactionsChanged();    // the list of interactions (not their values)
    void eventObserved(const QString &channel, const QString &message);
    // Their values changed by a preset, a snapshot, a timeline, OSC (sent once for many, from the main thread)
    void interactionValuesChanged();
    void frameRendered(); // emitted from the render thread, at most once per frame displayed by the UI

public:
    void acknowledgeFrame() { m_framePending = false; } // the UI has handled frameRendered

private:
    void takeOverFades(quint64 id, const QSet<QString> &paths, bool all = false);
    void applyParams(Layer &l, const QJsonObject &params, const QJsonObject &timing, const QJsonObject &easing, double fade,
                     FadeJob &job, bool showing, bool hiding, bool chain);
    friend class RenderThread;
    friend struct ScopedCurrent;

    struct Garbage; // detached resources, released in the render thread
    void makeCurrent();
    void doneCurrent();
    void renderLoop();
    void runPendingTasks();
    void frame(double dt);
    bool presentViewports(); // every viewport whose window is on screen (false: none)
    void releaseLayer(Layer &l);
    void releaseAll();
    std::shared_ptr<Garbage> detachSource(Layer &l);
    void releaseGarbage(const std::shared_ptr<Garbage> &g);
    void updateSource(Layer &l, double dt);
    void markNeeded(); // which layers are drawn this frame (shown, or used by another one)
    void renderPass(const IsfRenderContext &rc); // every layer, in dependency order
    void renderLayer(Layer &l, const IsfRenderContext &rc);
    void processLayer(Layer &l, GLuint tex, int w, int h, bool premultiplied, const IsfRenderContext &rc);
    void renderGroup(Layer &g, const std::vector<Layer *> &members, const IsfRenderContext &rc);
    // `view`: the part of the composition the target shows (normalized, origin top left)
    // The target is cleared to `clear` first (through a multisampled buffer when antialiasing is on)
    void compositeLayers(const RenderTarget &target, const std::vector<Layer *> &topToBottom,
                         const QRectF &view = QRectF(0, 0, 1, 1), QColor clear = QColor(0, 0, 0, 0),
                         quint64 viewport = 0, double angle = 0); // angle (degrees): the view turned about its center; viewport: the one drawn for (its share of each layer's opacity)
    std::map<std::pair<int, int>, MsaaBuffer> m_msaa; // by size (render thread)
    int m_maxSamples = 0;
    void renderViewport(Layer &v, const std::vector<Layer *> &shown, const IsfRenderContext &rc);
    void composite();
    void readSourcePreview();
    void stepFade(double dt); // snapshot fades (render thread, lock held)
    QJsonObject snapshotToJson(const Snapshot &m, const QString &projectDir) const;
    Snapshot snapshotFromJson(const QJsonObject &o, const QString &projectDir) const;
    void normalizeLocked(); // restores the structure invariants (lock held)
    quint64 newIdLocked();
    void clearProject();
    int viewportCountLocked() const;
    void drawQuad();
    void blit(GLuint tex, const RenderTarget &target);
    void bindMesh(Layer &l);         // its mapped mesh (render thread, mesh vertex array bound)
    void bindMeshBuffer(GLuint vbo); // any mesh vertex buffer
    QString resolvePath(const QJsonObject &o, const QString &projectDir) const;
    QJsonObject layerToJson(Layer &l, const QString &projectDir) const;
    void layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings);
    double nextDt();

    mutable Mutex m_mutex;

    QOpenGLContext *m_context = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QSurface *m_currentSurface = nullptr;
    QThread *m_thread = nullptr;
    QThread *m_ownerThread = nullptr;
    std::atomic<bool> m_threaded{false}, m_quit{false}, m_framePending{false};

    // OpenGL task queue
    struct Task {
        std::function<void()> fn;
        bool done = false;
    };
    mutable std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    bool m_renderFailed = false; // m_taskMutex held
    static std::atomic<bool> s_failRenderStart;
    std::deque<std::shared_ptr<Task>> m_tasks;

    // Output window (read by the render thread)
    // Each output window has a context of its own, sharing textures and programs with the engine's: its
    // drawable never changes (on macOS, moving one context from window to window every frame left them black).
    struct OutputSurface {
        QWindow *window = nullptr;
        bool exposed = false;
        QSize pixels;
        QOpenGLContext *context = nullptr; // created on first presentation (render thread)
        GLuint vao = 0;                    // vertex arrays are not shared between contexts
        std::shared_ptr<MetalOutput> metal; // macOS: the window shown through Metal instead (PresentMetal.h)
        quint64 presented = 0;              // pictures it was given
        quint64 dropped = 0;                // pictures it could not show (Metal), as last counted
        double hz = 0;                      // its screen's refresh rate (0: not known)
        // Pictures not shown, by cause: no surface from macOS, every shared surface still read, an error, no
        // picture yet, not drawn by the render thread (macOS: OpenGL into a window is the main thread's)
        quint64 noDrawable = 0, surfacesBusy = 0, failed = 0, noPicture = 0, notRenderThread = 0;
    };
    // What each output window did, as last sampled (render thread → any thread)
    struct OutputStats {
        quint64 viewport = 0;
        QString name;
        bool metal = false, exposed = false;
        double hz = 0;
        quint64 presented = 0, noDrawable = 0, surfacesBusy = 0, failed = 0, noPicture = 0, notRenderThread = 0;
    };
    mutable std::mutex m_outputStatsMutex;
    std::vector<OutputStats> m_outputStats;
    double m_presentHz = 0; // render thread: the refresh rate of the window that waits for its screen (0: none)
    qint64 m_paceNext = -1; // render thread: when the next frame is due (steady clock ns; -1: from now)
    void releaseOutputSurface(OutputSurface &o);
    void present(const Layer &viewport, OutputSurface &out);
    std::map<quint64, OutputSurface> m_outWindows; // by viewport id (render thread)

    GLuint m_quadVao = 0, m_quadVbo = 0;
    GLuint m_meshVao = 0, m_meshVbo = 0, m_meshIbo = 0;
    GLsizei m_meshIndexCount = 0;
    GLuint m_diffProgram = 0, m_blitProgram = 0, m_compProgram = 0, m_presentProgram = 0, m_prepProgram = 0;
    GLint m_compRotLoc = -1, m_compAspLoc = -1, m_diffRotLoc = -1, m_diffAspLoc = -1;
    GLint m_diffTexLoc = -1, m_diffDstLoc = -1, m_diffOpacityLoc = -1, m_diffViewLoc = -1;
    RenderTarget m_dstCopy; // what is drawn so far, for Difference
    static inline std::atomic<bool> s_failDifferenceCopyForTests{false};
    GLint m_blitTexLoc = -1, m_compTexLoc = -1, m_compOpacityLoc = -1, m_compViewLoc = -1, m_presentTexLoc = -1;
    GLint m_prepTexLoc = -1, m_prepRoiLoc = -1, m_prepAddLoc = -1, m_prepRemoveLoc = -1, m_prepUnpremulLoc = -1, m_prepBalanceLoc = -1;
    GLint m_prepMaskLoc = -1, m_prepMaskModeLoc = -1;
    GLuint m_blackTex = 0;
    VideoConverter m_videoConv; // video frames to RGBA (render thread)
    TestCard m_testCard;        // the viewports' test cards (render thread)
    RenderTarget m_output[2];
    std::atomic<int> m_published{0};
    int m_back = 1;

    QSize m_compSize{1920, 1080};
    std::vector<std::unique_ptr<Layer>> m_layers;
    quint64 m_nextId = 1;
    std::vector<float> m_meshScratch;
    std::vector<uint8_t> m_renderMark; // render pass: layer already rendered this frame (reused, no allocation)

    std::atomic<double> m_compositionSpeed{1.0};
    std::atomic<bool> m_paused{false};
    void updateTimeScale();
    std::atomic<double> m_compositionOpacity{1.0};
    double m_compositionOpacityTarget = 1.0, m_compositionOpacitySpeed = 0.0; // units per second (0 = immediate)
    struct CompositionFade {
        double opacityElapsed = 0, volumeElapsed = 0; // each on its own clock: two snapshots can drive them
        bool opacity = false, volume = false;
        double opacityFrom = 1, opacityTo = 1, opacityDur = 0, volumeDur = 0;
        float volumeFrom = 1, volumeTo = 1;
        int opacityCurve = 0, volumeCurve = 0;
    } m_compFade;
    std::atomic<double> m_blackLevel{1.0};
    double m_blackTarget = 1.0, m_blackSpeed = 0.0, m_blackFade = 1.0;

    // Source preview
    quint64 m_previewId = 0;
    int m_previewSide = 360, m_previewTick = 0;
    RenderTarget m_previewTarget;
    GLuint m_previewPbo = 0; // read back without waiting: mapped at the next preview tick
    int m_previewPboW = 0, m_previewPboH = 0;
    quint64 m_previewPboId = 0; // the layer the buffer holds (0: nothing pending)
    mutable std::mutex m_previewMutex;
    QImage m_previewImage;
    quint64 m_previewImageId = 0;

    // The frame's end, for the other contexts (output windows, interface)
    GLsync m_frameFence = nullptr;
    mutable std::mutex m_fenceMutex;

    QElapsedTimer m_clock;
    qint64 m_lastNs = 0;
    double m_realDt = 0; // unclamped frame interval: playheads follow real time, as the sound does
    int64_t m_frameStampNs = 0; // steady_clock time the playheads of the current frame correspond to

    std::unique_ptr<AudioOutput> m_audio;
    PlayMode m_defaultPlayMode = PlayMode::Loop;
    int m_defaultColorModels = 1;
    std::vector<Snapshot> m_snapshots;
    std::vector<SnapshotNode> m_snapshotTree;
    quint64 m_nextSnapshotGroupId = 1;
    void normalizeSnapshotTree(); // lock held: every snapshot once, their order the tree's
    std::vector<Preset> m_presets;
    quint64 m_nextPresetId = 1;
    std::vector<std::shared_ptr<FadeJob>> m_fades;
    QString m_glRenderer, m_glVersion, m_glVendor;
    // A snapshot gives a layer another source: the outgoing one keeps playing, invisible, and the two pictures
    // are mixed by an ISF transition into the layer's picture (before its mapping) until it is over
    struct SourceTransition {
        std::unique_ptr<Layer> from; // the outgoing source, with its own ROI, color and effects
        QString shaderPath;
        std::unique_ptr<IsfInstance> shader; // loaded by the render thread (none: a crossfade)
        bool loaded = false;
        bool crossfadeOnly = false; // a sound on either side: no shader, the levels cross (tests: transitionUsesShader)
        RenderTarget target;
        double elapsed = 0, duration = 1;
    };
    std::map<quint64, std::unique_ptr<SourceTransition>> m_transitions; // by layer id
    QString m_defaultTransition;
    void startSourceTransition(int index, const QJsonObject &state, double duration);
    void stepTransitions(double dt);                                      // render thread, lock held
    void renderTransition(Layer &l, SourceTransition &t, const IsfRenderContext &rc);
    void retireTransition(std::unique_ptr<SourceTransition> t);           // sound now, OpenGL on the render thread
    double m_fadeElapsed = 0; // seconds since the last recall
    quint64 m_recalledSnapshot = 0;
    double m_recallTotal = 0;
    quint64 m_nextSnapshotId = 1;
    RenderSettings m_render, m_renderDefaults{0, 0, 0};
    double m_screenHz = 60;
    std::vector<Animation> m_animations;
    quint64 m_nextAnimationId = 1;
    void applyAnimation(Animation &a); // sets its values where it is (lock held)
    // owner: the layer of one of its own animations (its values are set on it even when it is locked)
    void controlLocked(Animation *a, AnimAction action, double time, AnimLoop loop, int repeat, Layer *owner = nullptr);
    void applyLayerAnim(Layer &l, Animation &a); // sets its value where it is (lock held)
    // A number of a layer an animation sets: the snapshots' fades running leave it to it from now on (lock held)
    void releaseFromFades(quint64 layer, const QString &path);
    bool setAnimParam(quint64 layer, const QString &path, double v); // lock held
    const std::vector<Parameter *> &compositionParameters(); // its opacity, volume, speed (lock held)
    Parameter *findParameter(quint64 layer, const QString &path); // a layer's (0: the composition's); lock held
    std::vector<std::unique_ptr<Parameter>> m_compParams;
    std::vector<Parameter *> m_compParamList;
    QJsonArray animationsToJson() const;
    void animationsFromJson(const QJsonArray &a);
    void startLayerAnim(Animation &a); // from the start: the "current value" keys read now (lock held)
    void assignLayerAnims(Layer &l, const std::vector<Animation> &anims); // see setLayerAnims (lock held)
    void recallLayerAnims(Layer &l, const QJsonArray &stored);            // a snapshot's (lock held)
    Sequencer *m_sequencer = nullptr; // a child QObject
    std::atomic<bool> m_fadesManual{false};
    void attachAudio(Layer &l, std::shared_ptr<AudioStream> s);
    std::atomic<double> m_fps{0};
    FrameTiming m_frameTiming;
    std::atomic<quint64> m_outputsDropped{0}; // frames an output window could not show (Metal: surfaces in use)
    std::atomic<quint64> m_sharesSkipped{0};  // Syphon / Spout one frame later: the previous frame not finished
    std::atomic<bool> m_shareOneFrameLate{true};
    std::atomic<quint64> m_mipmapsMade{0}; // glGenerateMipmap calls (tests: once per layer and frame at most)
    void sampleResources(); // render thread, context current: once a second
    qint64 m_resourcesSampledNs = 0;
    int m_vramQuery = -1; // -1 not looked at yet, 0 none, 1 NVX, 2 ATI
    std::atomic<qint64> m_vramFreeKb{-1}, m_vramTotalKb{-1};
    std::atomic<quint32> m_overBudget{0}; // bits: 1 decoded frames, 2 upload buffers, 4 threads, 8 graphics memory
    std::atomic<quint64> m_frameCount{0};
    bool m_initialized = false;

    // Interactions
    std::vector<Interaction> m_interactions; // under m_mutex
    quint64 m_nextInteractionId = 1;
    struct InteractionRun {
        InteractionProcessor proc;
        InteractionTargetState target;
        std::optional<double> lastOut;
        double lastInternal = std::numeric_limits<double>::quiet_NaN();
        bool choiceSet = false;
        int lastChoice = -1;
        bool lastBool = false;
        std::array<double, 4> color{{0, 0, 0, 1}};
        int colorComponents = 0; // last OSC message: RGB(A), or 0 for scalar
        std::array<InteractionProcessor, 4> channels;  // a whole color: each channel's chain (Interactions.h)
        std::array<std::optional<double>, 4> channelOut; // their last results
        InteractionColorTargetState colorTarget;          // its pick-up
    };
    std::map<quint64, InteractionRun> m_interactionRun; // render thread (m_mutex held)
    std::mutex m_interactionEventsMutex;
    std::vector<InteractionEvent> m_interactionEvents; // since the last frame
    bool m_interactionLearn = false;
    std::optional<InteractionSource> m_learnKind;
    std::vector<InteractionEvent> m_learned; // distinct sources, the first one first
    QElapsedTimer m_learnClock;              // since the first
    std::vector<std::unique_ptr<midi::Input>> m_midiIns; // every MIDI input, open while a MIDI interaction is there
    QStringList m_midiInNames;
    QTimer *m_midiScan = nullptr;
    void stepInteractions(double dt); // lock held
    // Their values as the composition's parameters ("interaction/<id>/master"…): presets, timelines, OSC, other
    // interactions reach them; made again when the list changes (lock held)
    std::vector<std::unique_ptr<Parameter>> m_interactionParams;
    bool m_interactionParamsDirty = true;
    std::atomic<bool> m_interactionValuesPending{false};
    void interactionValuesTouched(); // any thread: interactionValuesChanged soon, once
    void makeInteractionParams();    // lock held
    const std::vector<std::unique_ptr<Parameter>> &interactionParams(); // made again when needed (lock held)
    struct InteractionFade {
        QString path; // "interaction/<id>/master"
        double from = 0, to = 0, time = 0, elapsed = 0;
        int curve = 0;
    };
    std::vector<InteractionFade> m_interactionFades; // render thread (lock held)
    void stepInteractionFades(double dt);
    void updateMidiInputs();          // main thread
    std::vector<TimecodeOutput> m_tcOutputs;  // under m_mutex
    std::vector<MidiOutput> m_midiOutputs;
    std::vector<AudioChannelOutput> m_audioOutputs;
    quint64 m_nextPatchId = 1;                 // ids of all three
    std::map<quint64, std::shared_ptr<TimecodeClock>> m_atc; // ATC: the clock each viewport's pictures carry
    std::unique_ptr<TimecodeRouter> m_tcRouter;
    void updateTimecodeRouting();              // the router told what each clock sends where (main thread)
    enum class PatchKind { Timecode, Midi, Audio };
    QString uniquePatchName(const QString &base, PatchKind kind) const; // lock held
    bool routeTargetExists(const QString &route) const;                 // lock held
    bool hasTimecodeOutput(quint64 id) const;

    QString m_projectPath;
    IsfLibrary m_library;
    QStringList m_binItems;

    // Publishing: the viewports' publishers (Publishing.h)
    Publishing m_publishing{[this] { drawQuad(); }};
    GLuint m_maskProgram = 0; // an effect's result over its input, through a mask
    GLint m_maskInLoc = -1, m_maskFxLoc = -1, m_maskMaskLoc = -1, m_maskInvertLoc = -1;
    GLuint m_shapeMaskProgram = 0; // a picture through its masks' alpha
    GLint m_shapeMaskTexLoc = -1, m_shapeMaskMaskLoc = -1, m_shapeMaskRgbLoc = -1;
    GLint m_compMaskLoc = -1, m_compMaskOnLoc = -1, m_diffMaskLoc = -1, m_diffMaskOnLoc = -1;
    void applySurfaceMasks(Layer &l, GLuint &tex, int w, int h);  // render thread
    bool bindCompositionMasks(Layer &l);                          // render thread: true if it has some
    GLuint m_fadeProgram = 0; // a crossfade of two pictures (a source transition without a shader)
    GLint m_fadeALoc = -1, m_fadeBLoc = -1, m_fadeKLoc = -1;
};
