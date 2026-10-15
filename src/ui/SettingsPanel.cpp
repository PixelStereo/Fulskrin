#include "SettingsPanel.h"
#include "Widgets.h"
#include "ColorPicker.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QNetworkInterface>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static const char *kPlayModeKey = "playback/defaultMode";
static const char *kHardwareKey = "playback/hardwareDecoding";
static const char *kFollowKey = "ui/followFades";
static const char *kOscKey = "osc/enabled";
static const char *kOscPortKey = "osc/udpPort";        // (osc/port, osc/queryPort: earlier defaults, ignored)
static const char *kQueryPortKey = "osc/oscQueryPort";
static const char *kOscListenKey = "osc/listen"; // the address listened on ("": every interface)
static const char *kTransitionKey = "snapshots/transition";
static const char *kRateKey = "render/frameRate";
static const char *kSamplesKey = "render/antialiasing";
static const char *kMipmapsKey = "render/mipmaps";
static const char *kDepthKey = "render/depth";
static const char *kShareLateKey = "render/gpuShareOneFrameLate";
static const char *kStartupUseKey = "startup/useProject"; // false: an empty project at launch
static const char *kStartupPathKey = "startup/project";

QList<double> renderChoice::frameRates() { return {0, 24, 25, 30, 50, 60, 120}; }
QString renderChoice::frameRateName(double fps)
{
    return fps <= 0 ? QStringLiteral("Screen refresh") : QStringLiteral("%1 fps").arg(fps);
}
QList<int> renderChoice::depths() { return {10, 8}; }
QString renderChoice::depthName(int bits)
{
    return bits >= 10 ? QStringLiteral("16 bits/channel (RGBA16)") : QStringLiteral("8 bits/channel (lighter)");
}
QList<int> renderChoice::samples() { return {0, 2, 4, 8}; }
QString renderChoice::samplesName(int n) { return n <= 1 ? QStringLiteral("Off") : QStringLiteral("%1× MSAA").arg(n); }

Engine::RenderSettings SettingsPanel::renderDefaults()
{
    QSettings s;
    Engine::RenderSettings r;
    r.frameRate = std::max(0.0, s.value(kRateKey, 0.0).toDouble());
    r.samples = std::max(0, s.value(kSamplesKey, 4).toInt());
    r.mipmaps = s.value(kMipmapsKey, true).toBool() ? 1 : 0;
    r.depth = s.value(kDepthKey, 10).toInt() >= 10 ? 10 : 8; // RGBA16 unless 8 bits are chosen
    return r;
}
static constexpr int kDefaultOscPort = 1234, kDefaultQueryPort = 5678;

PlayMode SettingsPanel::defaultPlayMode()
{
    return playModeFromKey(QSettings().value(kPlayModeKey).toString(), PlayMode::Loop);
}
bool SettingsPanel::gpuShareOneFrameLate() { return QSettings().value(kShareLateKey, true).toBool(); }
bool SettingsPanel::hardwareDecoding() { return QSettings().value(kHardwareKey, true).toBool(); }
static int s_followFades = -1; // read once, then kept up to date by the check box (asked 10 times a second)
bool SettingsPanel::followFades()
{
    if (s_followFades < 0) s_followFades = QSettings().value(kFollowKey, true).toBool() ? 1 : 0;
    return s_followFades == 1;
}
int SettingsPanel::colorModels() { return ColorPicker::defaultView() == ColorPicker::HSL ? ColorEditor::Hsl : ColorEditor::Rgb; }
QString SettingsPanel::startupProject()
{
    QSettings s;
    return s.value(kStartupUseKey, false).toBool() ? s.value(kStartupPathKey).toString() : QString();
}
bool SettingsPanel::oscEnabled() { return QSettings().value(kOscKey, true).toBool(); }
int SettingsPanel::oscPort() { return QSettings().value(kOscPortKey, kDefaultOscPort).toInt(); }
int SettingsPanel::oscQueryPort() { return QSettings().value(kQueryPortKey, kDefaultQueryPort).toInt(); }
QString SettingsPanel::oscListen() { return QSettings().value(kOscListenKey).toString(); }

QString SettingsPanel::defaultTransition(const IsfLibrary &library)
{
    const QString saved = QSettings().value(kTransitionKey, QStringLiteral("Crossfade.fs")).toString();
    if (QFileInfo::exists(saved)) return saved;
    return library.findByFileName(QFileInfo(saved).fileName()); // empty: a built-in crossfade
}

void SettingsPanel::setTransitions(const QVector<IsfEntry> &transitions, const QString &current)
{
    QSignalBlocker b(m_transition);
    m_transition->clear();
    for (const IsfEntry &t : transitions) {
        m_transition->addItem(t.name, t.path);
        m_transition->setItemData(m_transition->count() - 1, t.description, Qt::ToolTipRole);
    }
    if (m_transition->findData(current) < 0) m_transition->addItem(QStringLiteral("Crossfade (built in)"), QString());
    m_transition->setCurrentIndex(std::max(0, m_transition->findData(current)));
}

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

SettingsPanel::SettingsPanel(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    auto *tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(theme::tabStyle()); // as the inspector's sub-tabs
    connect(theme::notifier(), &theme::Notifier::changed, tabs, [tabs] { tabs->setStyleSheet(theme::tabStyle()); });
    root->addWidget(tabs);
    // One page per subject; the page shown is kept from one session to the next
    auto page = [tabs](const QString &title) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(8, 8, 8, 8);
        l->setSpacing(10);
        tabs->addTab(w, title);
        return l;
    };
    QVBoxLayout *playbackPage = page(QStringLiteral("Playback"));
    QVBoxLayout *renderPage = page(QStringLiteral("Rendering"));
    QVBoxLayout *audioPage = page(QStringLiteral("Audio"));
    QVBoxLayout *interfacePage = page(QStringLiteral("Interface"));
    QVBoxLayout *oscPage = page(QStringLiteral("OSC"));
    tabs->setCurrentIndex(std::clamp(QSettings().value(QStringLiteral("ui/settingsTab"), 0).toInt(), 0, tabs->count() - 1));
    connect(tabs, &QTabWidget::currentChanged, this, [](int i) { QSettings().setValue(QStringLiteral("ui/settingsTab"), i); });

    auto *playback = new QGroupBox(QStringLiteral("Playback"));
    auto *form = new QFormLayout(playback);
    m_playMode = new QComboBox;
    for (PlayMode m : {PlayMode::OneShot, PlayMode::Loop, PlayMode::PingPong, PlayMode::Stop})
        m_playMode->addItem(playModeName(m), int(m));
    m_playMode->setCurrentIndex(m_playMode->findData(int(defaultPlayMode())));
    form->addRow(new ResetLabel(QStringLiteral("Default play mode"), [this] { m_playMode->setCurrentIndex(1); }), m_playMode);
    form->addRow(note(QStringLiteral("Given to a video or a sound when it is loaded into a layer. "
                                     "Layers already loaded keep their own mode.")));
    m_transition = new QComboBox;
    form->addRow(new ResetLabel(QStringLiteral("Snapshot transition"),
                                [this] {
                                    const int k = m_transition->findText(QStringLiteral("Crossfade"));
                                    m_transition->setCurrentIndex(std::max(0, k));
                                }),
                 m_transition);
    form->addRow(note(QStringLiteral("When a snapshot gives a layer another source: the outgoing one keeps playing and "
                                     "this ISF transition takes it to the new one, over the snapshot's fade. "
                                     "A layer can choose its own (Source tab).")));
    m_hardware = new FlagBox(QStringLiteral("Hardware decoding"));
    m_hardware->setChecked(hardwareDecoding());
    form->addRow(m_hardware);
    form->addRow(note(QStringLiteral("H.264, HEVC, ProRes… decoded by the graphics hardware when it can (VideoToolbox on "
                                     "macOS, Direct3D 11 on Windows, VA-API on Linux), which leaves the processor to the "
                                     "other videos. HAP never needs it. For the videos loaded from now on.")));
    connect(m_hardware, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(kHardwareKey, on);
        emit hardwareDecodingChanged();
    });
    connect(m_transition, qOverload<int>(&QComboBox::activated), this, [this] {
        const QString path = m_transition->currentData().toString();
        QSettings().setValue(kTransitionKey, path);
        emit transitionChanged(path);
    });
    playbackPage->addWidget(playback);
    connect(m_playMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(kPlayModeKey, playModeKey(PlayMode(m_playMode->currentData().toInt())));
        emit playModeChanged();
    });

    // Rendering: the machine's, for every project
    {
        auto *box = new QGroupBox(QStringLiteral("Rendering"));
        auto *rf = new QFormLayout(box);
        const Engine::RenderSettings d = renderDefaults();
        m_rate = new QComboBox;
        for (double r : renderChoice::frameRates()) m_rate->addItem(renderChoice::frameRateName(r), r);
        m_rate->setCurrentIndex(std::max(0, m_rate->findData(d.frameRate)));
        m_samples = new QComboBox;
        for (int n : renderChoice::samples()) m_samples->addItem(renderChoice::samplesName(n), n);
        m_samples->setCurrentIndex(std::max(0, m_samples->findData(d.samples)));
        m_depth = new QComboBox;
        for (int b : renderChoice::depths()) m_depth->addItem(renderChoice::depthName(b), b);
        m_depth->setCurrentIndex(std::max(0, m_depth->findData(d.depth)));
        m_mipmaps = new FlagBox(QStringLiteral("Smooth pictures drawn smaller (mipmaps)"));
        m_mipmaps->setChecked(d.mipmaps > 0);
        rf->addRow(new ResetLabel(QStringLiteral("Frame rate"), [this] { m_rate->setCurrentIndex(0); }), m_rate);
        rf->addRow(new ResetLabel(QStringLiteral("Antialiasing"), [this] { m_samples->setCurrentIndex(2); }), m_samples);
        rf->addRow(new ResetLabel(QStringLiteral("Internal precision"), [this] { m_depth->setCurrentIndex(0); }), m_depth);
        rf->addRow(note(QStringLiteral("16 bits (default): composition, ISF and effect render targets in RGBA16, so "
                                       "generated gradients are not cut to 256 shades inside PINOL. 8 bits: half the GPU "
                                       "memory and bandwidth, for heavy 4K / 8K shows. Displays, media decoding and "
                                       "network outputs can still be 8-bit: no HDR or 10-bit output signal is implied.")));
        rf->addRow(m_mipmaps);
        auto *shareLate = new FlagBox(QStringLiteral("Syphon / Spout one frame later, without waiting for the GPU"));
        shareLate->setChecked(gpuShareOneFrameLate());
        shareLate->setToolTip(QStringLiteral("On (default): publish the prior completed picture without glFinish "
                                             "using a third viewport target. Off: wait for the current frame to "
                                             "finish on the GPU before sharing it."));
        rf->addRow(shareLate);
        connect(shareLate, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(kShareLateKey, on);
            emit renderDefaultsChanged();
        });
        rf->addRow(note(QStringLiteral("For every project opened on this machine. Screen refresh: "
                                       "the outputs' vertical sync, or the main screen's rate. Antialiasing smooths the "
                                       "edges of the mapped layers; mipmaps the pictures drawn much smaller than they are.")));
        renderPage->addWidget(box);
        auto save = [this] {
            QSettings s;
            s.setValue(kRateKey, m_rate->currentData().toDouble());
            s.setValue(kSamplesKey, m_samples->currentData().toInt());
            s.setValue(kMipmapsKey, m_mipmaps->isChecked());
            s.setValue(kDepthKey, m_depth->currentData().toInt());
            emit renderDefaultsChanged();
        };
        connect(m_rate, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_samples, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_depth, qOverload<int>(&QComboBox::activated), this, save);
        connect(m_mipmaps, &QCheckBox::toggled, this, save);
    }

    // Accent color of the interface
    auto *look = new QGroupBox(QStringLiteral("Interface"));
    auto *lookForm = new QFormLayout(look);
    auto *swatch = new QPushButton;
    swatch->setFixedSize(60, 22);
    swatch->setToolTip(QStringLiteral("Color of the selection, the bars and the controls that are on"));
    auto paintSwatch = [swatch] {
        swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #55555c; border-radius:3px;")
                                  .arg(theme::css()));
    };
    paintSwatch();
    lookForm->addRow(new ResetLabel(QStringLiteral("Accent color"),
                                    [paintSwatch] { theme::setAccent(theme::defaultAccent()); paintSwatch(); }),
                     swatch);
    lookForm->addRow(note(QStringLiteral("Used for the selected layer, the bars, the buttons that are on and the "
                                         "links. A click on the name puts it back to the default light grey.")));
    // Magnetism: its state when Pinol starts (the button next to the zoom changes it meanwhile), its reach
    auto *magnetOn = new FlagBox(QStringLiteral("Magnetism on at start"));
    magnetOn->setChecked(magnet::enabledAtStart());
    auto *reach = new IntBox;
    reach->setRange(2, 40);
    reach->setSuffix(QStringLiteral(" px"));
    reach->setValue(magnet::distance());
    lookForm->addRow(magnetOn);
    lookForm->addRow(new ResetLabel(QStringLiteral("Magnet distance"), [reach] { reach->setValue(8); }), reach);
    lookForm->addRow(note(QStringLiteral("Dragged points, layers and viewports are caught by the edges, centers and corners "
                                         "within this distance on screen, and the bars by their notable values. The "
                                         "magnet button next to the zoom turns it on or off; Ctrl / ⌘ held while dragging "
                                         "moves freely.")));
    connect(magnetOn, &QCheckBox::toggled, this, [](bool on) {
        magnet::setEnabledAtStart(on);
        magnet::setEnabled(on);
    });
    connect(reach, qOverload<int>(&QSpinBox::valueChanged), this, [](int px) { magnet::setDistance(px); });
    auto *follow = new FlagBox(QStringLiteral("Fields follow the values during snapshot fades"));
    follow->setChecked(followFades());
    lookForm->addRow(follow);
    lookForm->addRow(note(QStringLiteral("On: the inspector's sliders and fields (opacity, viewports, color, "
                                         "position, volume, speed, ISF parameters…) move with the values while a snapshot "
                                         "fades them. Off: they stay still and show the snapshot's values once the fade is "
                                         "over. A field being edited is never moved.")));
    connect(follow, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(kFollowKey, on);
        s_followFades = on ? 1 : 0;
    });
    connect(swatch, &QPushButton::clicked, this, [this, paintSwatch] {
        const QColor c = ColorPicker::getColor(theme::accent(), this, QStringLiteral("Accent color"),
                                               QStringLiteral("ui/colorPickerMode/settings/accent"), false);
        if (!c.isValid()) return;
        theme::setAccent(c);
        paintSwatch();
    });
    interfacePage->addWidget(look);

    { // At launch: an empty project, or a project of one's own used as a template
        auto *box = new QGroupBox(QStringLiteral("Startup"));
        auto *sf = new QFormLayout(box);
        auto *mode = new QComboBox;
        mode->addItem(QStringLiteral("Empty project"), false);
        mode->addItem(QStringLiteral("Startup project"), true);
        auto *path = new QLabel;
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        path->setWordWrap(true);
        auto *choose = new QPushButton(QStringLiteral("Choose…"));
        auto *useCurrent = new QPushButton(QStringLiteral("Use Current Project"));
        auto *buttons = new QHBoxLayout;
        buttons->addWidget(choose);
        buttons->addWidget(useCurrent);
        buttons->addStretch(1);
        const auto showPath = [path, mode, choose, useCurrent] {
            const QString p = QSettings().value(kStartupPathKey).toString();
            const bool on = mode->currentData().toBool();
            path->setText(p.isEmpty() ? QStringLiteral("(none chosen)")
                                      : QFileInfo::exists(p) ? p : p + QStringLiteral("  — not found"));
            path->setEnabled(on);
            choose->setEnabled(on);
            useCurrent->setEnabled(on);
        };
        {
            const QSignalBlocker b(mode);
            mode->setCurrentIndex(QSettings().value(kStartupUseKey, false).toBool() ? 1 : 0);
        }
        sf->addRow(new ResetLabel(QStringLiteral("At launch"), [mode] { mode->setCurrentIndex(0); }), mode);
        sf->addRow(QStringLiteral("Project"), path);
        sf->addRow(QString(), buttons);
        sf->addRow(note(QStringLiteral("Opened when Pinol starts, instead of an empty project. It is used as a "
                                       "template: it opens untitled, \"Save\" asks where to save, and the startup "
                                       "file itself is never written over. Its startup sequence (Composition), if it has one, plays. A project given to open (double-click, "
                                       "command line) or a session to recover comes first.")));
        connect(mode, &QComboBox::currentIndexChanged, this, [mode, showPath] {
            QSettings().setValue(kStartupUseKey, mode->currentData().toBool());
            showPath();
        });
        connect(choose, &QPushButton::clicked, this, [this, showPath] {
            QSettings s;
            const QString cur = s.value(kStartupPathKey).toString();
            const QString f = QFileDialog::getOpenFileName(
                this, QStringLiteral("Startup Project"),
                cur.isEmpty() ? s.value("dirs/project").toString() : QFileInfo(cur).absolutePath(),
                QStringLiteral("Pinol Projects (*.pinol *.json)"));
            if (f.isEmpty()) return;
            s.setValue(kStartupPathKey, QFileInfo(f).absoluteFilePath());
            showPath();
        });
        connect(useCurrent, &QPushButton::clicked, this, [this, showPath, path] {
            const QString p = m_engine->projectPath();
            if (p.isEmpty()) { // untitled: nothing on disk to open at launch yet
                path->setText(QStringLiteral("The current project is not saved yet: save it first."));
                return;
            }
            QSettings().setValue(kStartupPathKey, QFileInfo(p).absoluteFilePath());
            showPath();
        });
        showPath();
        interfacePage->addWidget(box);
    }

    auto *color = new QGroupBox(QStringLiteral("Color"));
    auto *cf = new QFormLayout(color);
    m_colorModels = new QComboBox;
    m_colorModels->setObjectName(QStringLiteral("defaultColorPickerView"));
    m_colorModels->addItem(QStringLiteral("HSL"), int(ColorPicker::HSL));
    m_colorModels->addItem(QStringLiteral("RGB"), int(ColorPicker::RGB));
    m_colorModels->addItem(QStringLiteral("HSV"), int(ColorPicker::HSV));
    m_colorModels->addItem(QStringLiteral("Wheel"), int(ColorPicker::Wheel));
    m_colorModels->addItem(QStringLiteral("Palette"), int(ColorPicker::Palette));
    m_colorModels->setCurrentIndex(m_colorModels->findData(int(ColorPicker::defaultView())));
    cf->addRow(new ResetLabel(QStringLiteral("Color widgets"), [this] {
        m_colorModels->setCurrentIndex(m_colorModels->findData(int(ColorPicker::HSL)));
    }), m_colorModels);
    cf->addRow(note(QStringLiteral("Initial view for every color picker. Each location remembers its own view "
                                   "once you choose one there. Locations without a choice follow this setting.")));
    interfacePage->addWidget(color);
    connect(m_colorModels, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        ColorPicker::setDefaultView(ColorPicker::View(m_colorModels->currentData().toInt()));
        emit colorModelsChanged();
    });

    auto *osc = new QGroupBox(QStringLiteral("OSC"));
    auto *of = new QFormLayout(osc);
    m_osc = new FlagBox(QStringLiteral("Control by OSC and publish the namespace (OSCQuery)"));
    m_osc->setChecked(oscEnabled());
    m_oscPort = new IntBox;
    m_queryPort = new IntBox;
    for (QSpinBox *sb : {m_oscPort, m_queryPort}) {
        sb->setRange(1024, 65535);
        sb->setKeyboardTracking(false);
    }
    m_oscPort->setValue(oscPort());
    m_queryPort->setValue(oscQueryPort());
    of->addRow(m_osc);
    of->addRow(new ResetLabel(QStringLiteral("OSC port (UDP)"), [this] { m_oscPort->setValue(kDefaultOscPort); }), m_oscPort);
    of->addRow(new ResetLabel(QStringLiteral("OSCQuery port"), [this] { m_queryPort->setValue(kDefaultQueryPort); }),
               m_queryPort);
    // Where OSC and OSCQuery listen: every network interface, this computer only, or one interface (a show network)
    m_oscListen = new QComboBox;
    m_oscListen->addItem(QStringLiteral("All network interfaces"), QString());
    m_oscListen->addItem(QStringLiteral("This computer only"), QStringLiteral("127.0.0.1"));
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        if (!(nif.flags() & QNetworkInterface::IsUp) || (nif.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry &e : nif.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                m_oscListen->addItem(QStringLiteral("%1 · %2").arg(nif.humanReadableName(), e.ip().toString()), e.ip().toString());
    }
    {
        const QString saved = oscListen();
        int i = m_oscListen->findData(saved);
        if (i < 0) { // an interface that is not up now: kept as chosen
            m_oscListen->addItem(QStringLiteral("%1 (not connected)").arg(saved), saved);
            i = m_oscListen->count() - 1;
        }
        m_oscListen->setCurrentIndex(i);
    }
    of->addRow(new ResetLabel(QStringLiteral("Listen on"), [this] { m_oscListen->setCurrentIndex(0); }), m_oscListen);
    m_oscStatus = note(QString());
    of->addRow(m_oscStatus);
    of->addRow(note(QStringLiteral(
        "Announced by zeroconf (_oscjson._tcp, _osc._udp): OSCQuery clients (score, Chataigne, Vezér…) find it "
        "by themselves. Addresses: /composition/…, /layer/&lt;name&gt;/… "
        "(groups: /layer/&lt;group&gt;/layer/&lt;name&gt;/…). The whole tree: http://&lt;this machine&gt;:&lt;OSCQuery port&gt;/<br>"
        "No password: any machine on a network listened on can control the show. On a shared network, listen on the "
        "show's interface only, or on this computer only.")));
    oscPage->addWidget(osc);
    audioPage->addWidget(buildAudio());
    for (QVBoxLayout *l : {playbackPage, renderPage, audioPage, interfacePage, oscPage}) l->addStretch();
    auto apply = [this] {
        QSettings s;
        s.setValue(kOscKey, m_osc->isChecked());
        s.setValue(kOscPortKey, m_oscPort->value());
        s.setValue(kQueryPortKey, m_queryPort->value());
        s.setValue(kOscListenKey, m_oscListen->currentData().toString());
        emit oscChanged();
    };
    connect(m_osc, &QCheckBox::toggled, this, apply);
    connect(m_oscPort, qOverload<int>(&QSpinBox::valueChanged), this, apply);
    connect(m_queryPort, qOverload<int>(&QSpinBox::valueChanged), this, apply);
    connect(m_oscListen, qOverload<int>(&QComboBox::currentIndexChanged), this, apply);
}

// Audio: the sound card of this machine (the volume is the composition's)
QWidget *SettingsPanel::buildAudio()
{
    auto *g = new QGroupBox(QStringLiteral("Audio Output"));
    auto *v = new QVBoxLayout(g);
    auto *devRow = new QHBoxLayout;
    m_audioDevice = new QComboBox;
    m_audioDevice->setToolTip(QStringLiteral("Sound card or audio interface used for the sound of every layer"));
    m_audioDevice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_audioDevice->setMinimumContentsLength(8);
    auto *rescan = new QToolButton;
    rescan->setText(QStringLiteral("⟳"));
    rescan->setToolTip(QStringLiteral("Refresh the list of audio devices"));
    devRow->addWidget(new ResetLabel(QStringLiteral("Device"), [this] {
        m_audioDevice->setCurrentIndex(0); // the system's default
        emit m_audioDevice->activated(0);
    }));
    devRow->addWidget(m_audioDevice, 1);
    devRow->addWidget(rescan);
    v->addLayout(devRow);
    m_audioState = note(QString());
    v->addWidget(m_audioState);
    v->addWidget(note(QStringLiteral("Saved on this machine. The volume and the blackout are in the Composition tab.")));
    connect(rescan, &QToolButton::clicked, this, [this] { fillAudioDevices(); });
    connect(m_audioDevice, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString name = m_audioDevice->currentData().toString();
        QSettings().setValue("audio/device", name);
        openAudioDevice(name);
    });
    return g;
}

void SettingsPanel::fillAudioDevices()
{
    const QString saved = QSettings().value("audio/device").toString();
    QSignalBlocker b(m_audioDevice);
    m_audioDevice->clear();
    m_audioDevice->addItem(QStringLiteral("System default"), QString());
    for (const QString &n : AudioOutput::deviceNames()) m_audioDevice->addItem(n, n);
    int idx = m_audioDevice->findData(saved);
    if (idx < 0 && !saved.isEmpty()) { // saved device currently unplugged: keep it visible
        m_audioDevice->addItem(saved + QStringLiteral(" (not connected)"), saved);
        idx = m_audioDevice->count() - 1;
    }
    m_audioDevice->setCurrentIndex(qMax(0, idx));
}

void SettingsPanel::openAudioDevice(const QString &name)
{
    QString err;
    if (m_engine->startAudio(name, &err)) {
        m_audioState->setText(QStringLiteral("Playing on \"%1\" · %2 kHz · latency %3 ms")
                                  .arg(m_engine->audioOutput().deviceName().toHtmlEscaped())
                                  .arg(AudioOutput::kSampleRate / 1000)
                                  .arg(int(std::lround(m_engine->audioOutput().latency() * 1000))));
        m_audioState->setStyleSheet("color:#888; font-size:11px;");
    } else {
        m_audioState->setText(err.toHtmlEscaped());
        m_audioState->setStyleSheet("color:#ff6b5b; font-size:11px;");
    }
}

void SettingsPanel::startAudio()
{
    fillAudioDevices();
    openAudioDevice(QSettings().value("audio/device").toString());
}

void SettingsPanel::setOscStatus(const QString &s) { m_oscStatus->setText(s.toHtmlEscaped()); }
