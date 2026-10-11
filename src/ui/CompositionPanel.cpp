#include "CompositionPanel.h"
#include "SettingsPanel.h"
#include "Widgets.h"
#include "Engine.h"

#include <QCheckBox>
#include <algorithm>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTimer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

CompositionPanel::CompositionPanel(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(10);
    v->addWidget(buildCompositionLevel());
    v->addWidget(buildAudio());
    v->addWidget(buildComposition());
    v->addWidget(buildTimecodePatch());
    v->addStretch();
    syncFromEngine();
    // Long screen or sound card names must not widen the panel beyond its column (they are elided).
    for (QComboBox *c : findChildren<QComboBox *>()) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
    }
}

// ---------------------------------------------------------------------------
// Composition Level
// ---------------------------------------------------------------------------

QWidget *CompositionPanel::buildCompositionLevel()
{
    auto *g = new QGroupBox(QStringLiteral("Opacity"));
    auto *v = new QVBoxLayout(g);
    auto *row = new QHBoxLayout;
    m_composition = new SliderField;
    m_composition->setRange(0, 100);
    m_composition->setDecimals(0);
    m_composition->setSuffix(QStringLiteral(" %"));
    m_composition->setSingleStep(1);
    m_composition->setTicks(10);
    m_composition->setSnaps({0, 100});
    m_composition->setValue(100);
    m_composition->setToolTip(QStringLiteral("Composition opacity"));
    m_compositionLabel = new QLabel(QStringLiteral("out 100%"));
    m_compositionLabel->setToolTip(QStringLiteral("What goes out now (the fader, and the blackout over it)"));
    m_compositionLabel->setMinimumWidth(64);
    m_compositionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(new ResetLabel(QStringLiteral("Level"), [this] {
        m_composition->setValue(100);
        emit m_composition->valueEdited(100);
    }));
    row->addWidget(m_composition, 1);
    row->addWidget(m_compositionLabel);
    v->addLayout(row);

    auto *row2 = new QHBoxLayout;
    m_blackout = new QPushButton(QStringLiteral("Blackout"));
    m_blackout->setCheckable(true);
    m_blackout->setMinimumHeight(34);
    m_blackout->setToolTip(QStringLiteral("Fade the picture and the sound out / back in (Ctrl+B, ⌘B on Mac)"));
    m_blackout->setStyleSheet("QPushButton { font-weight:bold; } QPushButton:checked { background:#b3261e; color:white; }");
    m_fade = new NumberBox;
    m_fade->setRange(0.0, 30.0);
    m_fade->setSingleStep(0.5);
    m_fade->setDecimals(1);
    m_fade->setSuffix(QStringLiteral(" s"));
    m_fade->setValue(QSettings().value("composition/fade", 1.0).toDouble());
    m_fade->setToolTip(QStringLiteral("Fade duration for blackout and fade back in"));
    row2->addWidget(m_blackout, 1);
    row2->addWidget(new ResetLabel(QStringLiteral("Fade"), [this] { m_fade->setValue(1.0); }));
    row2->addWidget(m_fade);
    v->addLayout(row2);

    // Speed of everything that moves in time (media, shaders, timelines, fades, sequences); Pause holds it at 0
    auto *row3 = new QHBoxLayout;
    m_pause = new QPushButton(QStringLiteral("Pause"));
    m_pause->setCheckable(true);
    m_pause->setMinimumHeight(34);
    m_pause->setToolTip(QStringLiteral("Stops everything that moves in time (speed 0) / back to the current speed"));
    m_pause->setStyleSheet("QPushButton { font-weight:bold; } QPushButton:checked { background:#b8860b; color:white; }");
    m_speed = new SliderField;
    m_speed->setRange(0, 10);
    m_speed->setDecimals(2);
    m_speed->setSuffix(QStringLiteral(" ×"));
    m_speed->setSingleStep(0.05);
    m_speed->setTicks(10);
    m_speed->setSnaps({1});
    m_speed->setValue(1);
    m_speed->setToolTip(QStringLiteral("Composition speed: a coefficient on every speed (media, shaders' TIME, timelines, fades, sequences). "
                                       "0 stops them, 1 is normal"));
    row3->addWidget(m_pause);
    row3->addWidget(new ResetLabel(QStringLiteral("Speed"), [this] {
        m_speed->setValue(1);
        emit m_speed->valueEdited(1);
    }));
    row3->addWidget(m_speed, 1);
    v->addLayout(row3);
    connect(m_pause, &QPushButton::toggled, this, [this](bool on) {
        if (!m_syncing) m_engine->setPaused(on);
    });
    connect(m_speed, &SliderField::valueEdited, this, [this](double val) {
        if (!m_syncing) m_engine->setCompositionSpeed(val);
    });

    // The fader sets the picture's level; the blackout (picture and sound) is applied on top of it.
    connect(m_composition, &SliderField::valueEdited, this, [this](double val) {
        if (!m_syncing) m_engine->fadeCompositionOpacity(val / 100.0, 0.05);
    });
    connect(m_blackout, &QPushButton::toggled, this, [this](bool on) {
        if (!m_syncing) m_engine->setBlackout(on, fadeTime());
        emit blackoutChanged(on);
    });
    m_engine->setBlackoutFade(m_fade->value());
    connect(m_fade, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double val) {
        QSettings().setValue("composition/fade", val);
        m_engine->setBlackoutFade(val);
    });
    return g;
}

void CompositionPanel::setBlackout(bool on)
{
    if (m_blackout->isChecked() == on) {
        // Replay the fade (e.g. after a recovery where the level was forced)
        m_engine->setBlackout(on, fadeTime());
        return;
    }
    m_blackout->setChecked(on); // triggers the fade and blackoutChanged
}

bool CompositionPanel::isBlackout() const { return m_blackout->isChecked(); }
double CompositionPanel::compositionValue() const { return m_composition->value() / 100.0; }
double CompositionPanel::fadeTime() const { return m_fade->value(); }

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

static QString volumeText(int pct)
{
    if (pct <= 0) return QStringLiteral("−∞ dB");
    const double db = 20.0 * std::log10(pct / 100.0);
    return QStringLiteral("%1%2 dB").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

QWidget *CompositionPanel::buildAudio()
{
    auto *g = new QGroupBox(QStringLiteral("Sound"));
    auto *v = new QVBoxLayout(g);

    auto *volRow = new QHBoxLayout;
    m_audioVolume = new SliderField;
    m_audioVolume->setRange(0, 200);
    m_audioVolume->setDecimals(0);
    m_audioVolume->setSuffix(QStringLiteral(" %"));
    m_audioVolume->setSingleStep(1);
    m_audioVolume->setTicks(8);
    m_audioVolume->setSnaps({0, 100});
    m_audioVolume->setValue(100);
    m_audioVolume->setToolTip(QStringLiteral("Volume (100% = unity gain)"));
    m_audioVolumeLabel = new QLabel(volumeText(100));
    m_audioVolumeLabel->setMinimumWidth(64);
    m_audioVolumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volRow->addWidget(new ResetLabel(QStringLiteral("Volume"), [this] {
        m_audioVolume->setValue(100);
        emit m_audioVolume->valueEdited(100);
    }));
    volRow->addWidget(m_audioVolume, 1);
    volRow->addWidget(m_audioVolumeLabel);
    v->addLayout(volRow);

    for (int c = 0; c < 2; ++c) {
        m_meter[c] = new QProgressBar;
        m_meter[c]->setRange(0, 600); // -60 dB .. 0 dB, in tenths of a dB
        m_meter[c]->setTextVisible(false);
        m_meter[c]->setFixedHeight(6);
        m_meter[c]->setStyleSheet("QProgressBar { background:#1b1b1d; border:none; }"
                                  "QProgressBar::chunk { background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
                                  "stop:0 #3fae5a, stop:0.8 #3fae5a, stop:0.93 #e0b43a, stop:1 #e5483c); }");
        v->addWidget(m_meter[c]);
    }
    v->addWidget(note(QStringLiteral("The sound card is chosen in Settings ▸ Audio.")));
    connect(m_audioVolume, &SliderField::valueEdited, this, [this](double v) {
        const int pct = int(std::lround(v));
        m_audioVolumeLabel->setText(volumeText(pct));
        if (m_syncing) return;
        m_engine->setAudioVolume(pct / 100.0f);
        emit audioEdited();
    });
    m_meterTimer.setInterval(33);
    connect(&m_meterTimer, &QTimer::timeout, this, &CompositionPanel::refreshMeters);
    m_meterTimer.start();
    return g;
}

void CompositionPanel::refreshMeters()
{
    if (!isVisible()) return;
    for (int c = 0; c < 2; ++c) {
        const float pk = m_engine->audioOutput().peak(c);
        const double db = pk > 1e-6f ? 20.0 * std::log10(pk) : -60.0;
        m_meter[c]->setValue(int(std::clamp(db + 60.0, 0.0, 60.0) * 10));
    }
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

QWidget *CompositionPanel::buildComposition()
{
    auto *g = new QGroupBox(QStringLiteral("Composition"));
    auto *form = new QFormLayout(g);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Custom"));
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {4096, 2160}, {1920, 1200}, {2560, 1600},
                                {1400, 1050}, {1024, 768}, {3840, 1080}, {5760, 1080}};
    for (const QSize &s : sizes) m_preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    m_width = new IntBox;
    m_height = new IntBox;
    for (auto *sb : {m_width, m_height}) {
        sb->setRange(16, 16384);
        sb->setKeyboardTracking(false);
    }
    auto *size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("×")));
    size->addWidget(m_height);
    form->addRow(QStringLiteral("Preset"), m_preset);
    form->addRow(new ResetLabel(QStringLiteral("Size"), [this] { // 1920 × 1080, the size of a new show
        m_syncing = true;
        m_width->setValue(1920);
        m_syncing = false;
        m_height->setValue(1080);
        m_preset->setCurrentIndex(qMax(0, m_preset->findData(QSize(1920, 1080))));
    }),
                 size);
    m_rate = new QComboBox;
    m_rate->addItem(QStringLiteral("Machine default"), -1.0);
    for (double r : renderChoice::frameRates()) m_rate->addItem(renderChoice::frameRateName(r), r);
    m_rate->setToolTip(QStringLiteral("Frame rate of the render (Machine default: the choice made in Settings)"));
    form->addRow(new ResetLabel(QStringLiteral("Frame rate"), [this] {
        m_rate->setCurrentIndex(0);
        emit m_rate->activated(0);
    }),
                 m_rate);
    form->addRow(note(QStringLiteral("The pixel space every layer lives in. Each viewport shows a part of it, "
                                     "placed by its Spatial tab: three 1920 × 1080 projectors side by side "
                                     "make a 5760 × 1080 composition. The spatial placement is relative: it follows size changes.")));

    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QSize s = m_preset->itemData(i).toSize();
        if (!s.isValid()) return;
        m_syncing = true;
        m_width->setValue(s.width());
        m_height->setValue(s.height());
        m_syncing = false;
        applyComposition();
    });
    connect(m_rate, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (m_syncing) return;
        Engine::RenderSettings rs = m_engine->renderSettings();
        rs.frameRate = m_rate->itemData(i).toDouble();
        m_engine->setRenderSettings(rs);
        emit compositionEdited();
    });
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, [this] { applyComposition(); });
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, [this] { applyComposition(); });
    return g;
}

void CompositionPanel::applyComposition()
{
    if (m_syncing) return;
    const QSize s(m_width->value(), m_height->value());
    if (s == m_engine->compositionSize()) return;
    m_engine->setCompositionSize(s);
    emit compositionEdited();
}

void CompositionPanel::syncFromEngine()
{
    m_syncing = true;
    const QSize c = m_engine->compositionSize();
    m_width->setValue(c.width());
    m_height->setValue(c.height());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
    syncRate();
    m_audioVolume->setValue(std::lround(m_engine->audioVolume() * 100));
    m_audioVolumeLabel->setText(volumeText(int(m_audioVolume->value())));
    m_syncing = false;
    refreshStatus();
}

void CompositionPanel::syncRate()
{
    const double fps = m_engine->renderSettings().frameRate;
    int i = m_rate->findData(fps);
    if (i < 0) { // a rate set by OSC that the list does not have
        m_rate->addItem(QStringLiteral("%1 fps").arg(fps, 0, 'g', 6), fps);
        i = m_rate->count() - 1;
    }
    if (m_rate->currentIndex() != i) m_rate->setCurrentIndex(i);
}

void CompositionPanel::refreshStatus()
{
    // Fader, blackout, fade time and sound follow the changes made elsewhere (OSC)
    m_syncing = true;
    const int fader = int(std::lround(m_engine->compositionOpacityTarget() * 100));
    if (!m_composition->isDragging() && fader != int(std::lround(m_composition->value()))) m_composition->setValue(fader);
    if (m_blackout->isChecked() != m_engine->blackout()) m_blackout->setChecked(m_engine->blackout());
    if (m_pause->isChecked() != m_engine->paused()) m_pause->setChecked(m_engine->paused());
    if (!m_speed->isDragging() && std::abs(m_speed->value() - m_engine->compositionSpeed()) > 1e-6) m_speed->setValue(m_engine->compositionSpeed());
    if (!m_fade->hasFocus() && std::abs(m_fade->value() - m_engine->blackoutFade()) > 1e-6) m_fade->setValue(m_engine->blackoutFade());
    const int vol = int(std::lround(m_engine->audioVolume() * 100));
    if (!m_audioVolume->isDragging() && vol != int(std::lround(m_audioVolume->value()))) {
        m_audioVolume->setValue(vol);
        m_audioVolumeLabel->setText(volumeText(vol));
    }
    m_syncing = false;
    refreshTimecodeStatus();

    const int pct = int(std::lround(m_engine->outputLevel() * 100));
    m_compositionLabel->setText(QStringLiteral("out %1%").arg(pct)); // the fader, and the blackout over it
    m_compositionLabel->setStyleSheet(pct == 0 ? "color:#ff5a4f; font-weight:bold;" : "");

    if (!m_rate->view()->isVisible()) {
        m_syncing = true;
        syncRate();
        m_syncing = false;
    }
    const QSize c = m_engine->compositionSize();
    if (!m_width->hasFocus() && !m_height->hasFocus() && (c.width() != m_width->value() || c.height() != m_height->value())) {
        m_syncing = true;
        m_width->setValue(c.width());
        m_height->setValue(c.height());
        m_preset->setCurrentIndex(qMax(0, m_preset->findData(c)));
        m_syncing = false;
    }

}

// ---------------------------------------------------------------------------
// Timecode patch
// ---------------------------------------------------------------------------

static QToolButton *smallButton(const QString &text, const QString &tip)
{
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFixedWidth(26);
    return b;
}

static QLabel *statusLabel()
{
    auto *l = new QLabel;
    l->setWordWrap(true);
    l->setStyleSheet("color:#9ad; font-size:11px;");
    return l;
}

QWidget *CompositionPanel::buildTimecodePatch()
{
    m_tcPatch = new QWidget;
    auto *v = new QVBoxLayout(m_tcPatch);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    // Made again when the patch, a layer's name or the viewports change (not while a field of it is being typed in)
    auto later = [this] {
        QTimer::singleShot(0, this, [this] {
            if (timecodePatchKey() == m_tcPatchKey) return;
            if (QWidget *f = QApplication::focusWidget(); f && m_tcPatch->isAncestorOf(f) && qobject_cast<QLineEdit *>(f)) return;
            rebuildTimecodePatch();
        });
    };
    connect(m_engine, &Engine::timecodeRoutingChanged, this, later);
    connect(m_engine, &Engine::layersChanged, this, later);
    rebuildTimecodePatch();
    return m_tcPatch;
}

QString CompositionPanel::timecodePatchKey() const
{
    QStringList k;
    for (const TimecodeOutput &o : m_engine->timecodeOutputs()) {
        const int user = m_engine->timecodeOutputUser(o.id);
        k << QString::number(o.id) << o.name << o.routes.join(',') << QString::number(user);
        if (user >= 0) {
            Engine::Lock lk(&m_engine->mutex());
            if (const Layer *l = m_engine->layer(user)) k << l->name;
        }
    }
    for (const MidiOutput &m : m_engine->midiOutputs()) k << QString::number(m.id) << m.name << m.port;
    for (const AudioChannelOutput &a : m_engine->audioChannelOutputs())
        k << QString::number(a.id) << a.name << a.device << QString::number(a.channel);
    Engine::Lock lk(&m_engine->mutex());
    for (int i = 0; i < m_engine->layerCount(); ++i)
        if (const Layer *l = m_engine->layer(i); l && l->isViewport) k << QString::number(l->id) << l->name;
    return k.join('\x1f');
}

void CompositionPanel::rebuildTimecodePatch()
{
    m_tcPatchKey = timecodePatchKey();
    m_tcStatus.clear();
    auto *v = static_cast<QVBoxLayout *>(m_tcPatch->layout());
    while (QLayoutItem *it = v->takeAt(0)) {
        if (QWidget *w = it->widget()) w->deleteLater();
        delete it;
    }
    auto edited = [this] { emit compositionEdited(); }; // saved with the project
    auto nameField = [](const QString &name) {
        auto *e = new QLineEdit(name);
        e->setToolTip(QStringLiteral("Its name, as the layers and the patch show it"));
        return e;
    };

    // Timecode outputs: name, the layer driving it, its destinations
    {
        auto *g = new QGroupBox(QStringLiteral("Timecode Outputs"));
        auto *gv = new QVBoxLayout(g);
        gv->setSpacing(4);
        const std::vector<TimecodeOutput> outputs = m_engine->timecodeOutputs();
        // Every destination, and the output that has it (a destination takes one)
        struct Dest {
            QString route, label;
        };
        std::vector<Dest> dests;
        for (const MidiOutput &m : m_engine->midiOutputs()) dests.push_back({tcroute::midi(m.id), QStringLiteral("MTC → %1").arg(m.name)});
        for (const AudioChannelOutput &a : m_engine->audioChannelOutputs())
            dests.push_back({tcroute::audio(a.id), QStringLiteral("LTC → %1").arg(a.name)});
        {
            Engine::Lock lk(&m_engine->mutex());
            for (int i = 0; i < m_engine->layerCount(); ++i)
                if (const Layer *l = m_engine->layer(i); l && l->isViewport)
                    dests.push_back({tcroute::viewport(l->id), QStringLiteral("ATC → %1 (NDI, DeckLink)").arg(l->name)});
        }
        for (const TimecodeOutput &o : outputs) {
            auto *row = new QHBoxLayout;
            auto *name = nameField(o.name);
            row->addWidget(name, 1);
            auto *patch = new QToolButton;
            patch->setText(o.routes.isEmpty() ? QStringLiteral("Patch…") : QStringLiteral("%1 destination%2").arg(o.routes.size()).arg(o.routes.size() > 1 ? "s" : ""));
            patch->setToolTip(QStringLiteral("Where it goes: the destination gives the protocol (a MIDI output: MTC, an audio "
                                             "channel: LTC, a viewport: ATC in its NDI and DeckLink pictures)"));
            patch->setPopupMode(QToolButton::InstantPopup);
            auto *menu = new QMenu(patch);
            if (dests.empty()) menu->addAction(QStringLiteral("Add a MIDI output or an LTC channel below"))->setEnabled(false);
            for (const Dest &d : dests) {
                QAction *a = menu->addAction(d.label);
                a->setCheckable(true);
                a->setChecked(o.routes.contains(d.route));
                for (const TimecodeOutput &x : outputs)
                    if (x.id != o.id && x.routes.contains(d.route)) {
                        a->setEnabled(false); // another output's
                        a->setText(QStringLiteral("%1 (%2)").arg(d.label, x.name));
                    }
                const QString route = d.route;
                const quint64 id = o.id;
                connect(a, &QAction::toggled, this, [this, id, route, edited](bool on) {
                    for (TimecodeOutput t : m_engine->timecodeOutputs()) {
                        if (t.id != id) continue;
                        if (on) t.routes << route;
                        else t.routes.removeAll(route);
                        m_engine->setTimecodeOutput(t);
                        edited();
                    }
                });
            }
            patch->setMenu(menu);
            row->addWidget(patch);
            auto *remove = smallButton(QStringLiteral("−"), QStringLiteral("Remove this output (the layer driving it drives none)"));
            row->addWidget(remove);
            gv->addLayout(row);
            auto *status = statusLabel();
            gv->addWidget(status);
            const quint64 id = o.id;
            m_tcStatus.push_back({status, [this, id] { return m_engine->timecodeOutputStatus(id); }});
            connect(name, &QLineEdit::editingFinished, this, [this, id, name, edited] {
                for (TimecodeOutput t : m_engine->timecodeOutputs())
                    if (t.id == id && t.name != name->text().trimmed()) {
                        t.name = name->text();
                        m_engine->setTimecodeOutput(t);
                        edited();
                    }
            });
            connect(remove, &QToolButton::clicked, this, [this, id, edited] {
                m_engine->removeTimecodeOutput(id);
                edited();
            });
        }
        auto *add = new QPushButton(QStringLiteral("+ Timecode Output"));
        add->setToolTip(QStringLiteral("A new output; a Timecode layer picks it (its Output), the patch here says where it goes"));
        connect(add, &QPushButton::clicked, this, [this, edited] {
            m_engine->addTimecodeOutput();
            edited();
        });
        gv->addWidget(add, 0, Qt::AlignLeft);
        gv->addWidget(note(QStringLiteral("A Timecode layer drives one output; an output goes to any number of destinations, "
                                          "each destination to one output.")));
        static_cast<QVBoxLayout *>(m_tcPatch->layout())->addWidget(g);
    }

    // MIDI outputs: name, port of this machine
    {
        auto *g = new QGroupBox(QStringLiteral("MIDI Outputs"));
        auto *gv = new QVBoxLayout(g);
        gv->setSpacing(4);
        const QStringList ports = Engine::midiPorts();
        for (const MidiOutput &m : m_engine->midiOutputs()) {
            auto *row = new QHBoxLayout;
            auto *name = nameField(m.name);
            row->addWidget(name, 1);
            auto *port = new QComboBox;
            port->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            port->setMinimumContentsLength(8);
            port->addItem(QStringLiteral("No port"), QString());
            if (Engine::midiVirtualPorts())
                port->addItem(QStringLiteral("Virtual port \"%1\"").arg(m.name), QString::fromLatin1(MidiOutput::kVirtualPort));
            for (const QString &p : ports) port->addItem(p, p);
            int at = port->findData(m.port);
            if (at < 0) {
                port->addItem(QStringLiteral("%1 (not found)").arg(m.port), m.port);
                at = port->count() - 1;
            }
            port->setCurrentIndex(at);
            port->setToolTip(QStringLiteral("The MIDI port of this machine it sends to, or a port of its own other software reads"));
            row->addWidget(port, 1);
            auto *remove = smallButton(QStringLiteral("−"), QStringLiteral("Remove this MIDI output (and the routes to it)"));
            row->addWidget(remove);
            gv->addLayout(row);
            auto *status = statusLabel();
            gv->addWidget(status);
            const quint64 id = m.id;
            m_tcStatus.push_back({status, [this, id] { return m_engine->midiOutputStatus(id); }});
            connect(name, &QLineEdit::editingFinished, this, [this, id, name, edited] {
                for (MidiOutput x : m_engine->midiOutputs())
                    if (x.id == id && x.name != name->text().trimmed()) {
                        x.name = name->text();
                        m_engine->setMidiOutput(x);
                        edited();
                    }
            });
            connect(port, qOverload<int>(&QComboBox::activated), this, [this, id, port, edited](int) {
                for (MidiOutput x : m_engine->midiOutputs())
                    if (x.id == id) {
                        x.port = port->currentData().toString();
                        m_engine->setMidiOutput(x);
                        edited();
                    }
            });
            connect(remove, &QToolButton::clicked, this, [this, id, edited] {
                m_engine->removeMidiOutput(id);
                edited();
            });
        }
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton(QStringLiteral("+ MIDI Output"));
        connect(add, &QPushButton::clicked, this, [this, edited] {
            m_engine->addMidiOutput();
            edited();
        });
        auto *rescan = new QPushButton(QStringLiteral("↻"));
        rescan->setFixedWidth(28);
        rescan->setToolTip(QStringLiteral("Look for the MIDI ports again"));
        connect(rescan, &QPushButton::clicked, this, [this] { rebuildTimecodePatch(); });
        buttons->addWidget(add);
        buttons->addWidget(rescan);
        buttons->addStretch();
        gv->addLayout(buttons);
        if (ports.isEmpty() && !Engine::midiVirtualPorts()) gv->addWidget(note(QStringLiteral("No MIDI port on this machine.")));
        static_cast<QVBoxLayout *>(m_tcPatch->layout())->addWidget(g);
    }

    // Audio channels (LTC): name, device, channel
    {
        auto *g = new QGroupBox(QStringLiteral("LTC Audio Outputs"));
        auto *gv = new QVBoxLayout(g);
        gv->setSpacing(4);
        const QStringList devices = Engine::audioOutputDevices();
        for (const AudioChannelOutput &a : m_engine->audioChannelOutputs()) {
            auto *row = new QHBoxLayout;
            auto *name = nameField(a.name);
            row->addWidget(name, 1);
            auto *device = new QComboBox;
            device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            device->setMinimumContentsLength(8);
            device->addItem(QStringLiteral("No device"), QString());
            device->addItem(QStringLiteral("Default output"), QString::fromLatin1(AudioChannelOutput::kDefaultDevice));
            for (const QString &d : devices) device->addItem(d, d);
            int at = device->findData(a.device);
            if (at < 0) {
                device->addItem(QStringLiteral("%1 (not found)").arg(a.device), a.device);
                at = device->count() - 1;
            }
            device->setCurrentIndex(at);
            device->setToolTip(QStringLiteral("The audio output of this machine the LTC comes out of"));
            row->addWidget(device, 1);
            auto *channel = new IntBox;
            channel->setRange(1, 64);
            channel->setValue(a.channel + 1);
            channel->setPrefix(QStringLiteral("ch "));
            channel->setToolTip(QStringLiteral("Its channel on the device (each LTC on a channel of its own)"));
            row->addWidget(channel);
            auto *remove = smallButton(QStringLiteral("−"), QStringLiteral("Remove this audio output (and the routes to it)"));
            row->addWidget(remove);
            gv->addLayout(row);
            auto *status = statusLabel();
            gv->addWidget(status);
            const quint64 id = a.id;
            m_tcStatus.push_back({status, [this, id] { return m_engine->audioChannelOutputStatus(id); }});
            auto change = [this, id, edited](const std::function<void(AudioChannelOutput &)> &fn) {
                for (AudioChannelOutput x : m_engine->audioChannelOutputs())
                    if (x.id == id) {
                        fn(x);
                        m_engine->setAudioChannelOutput(x);
                        edited();
                    }
            };
            connect(name, &QLineEdit::editingFinished, this, [name, change] {
                change([name](AudioChannelOutput &x) {
                    if (!name->text().trimmed().isEmpty()) x.name = name->text();
                });
            });
            connect(device, qOverload<int>(&QComboBox::activated), this, [device, change](int) {
                const QString d = device->currentData().toString();
                change([d](AudioChannelOutput &x) { x.device = d; });
            });
            connect(channel, &QSpinBox::editingFinished, this, [channel, change] {
                const int c = channel->value() - 1;
                change([c](AudioChannelOutput &x) { x.channel = c; });
            });
            connect(remove, &QToolButton::clicked, this, [this, id, edited] {
                m_engine->removeAudioChannelOutput(id);
                edited();
            });
        }
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton(QStringLiteral("+ LTC Output"));
        connect(add, &QPushButton::clicked, this, [this, edited] {
            m_engine->addAudioChannelOutput();
            edited();
        });
        auto *rescan = new QPushButton(QStringLiteral("↻"));
        rescan->setFixedWidth(28);
        rescan->setToolTip(QStringLiteral("Look for the audio devices again"));
        connect(rescan, &QPushButton::clicked, this, [this] { rebuildTimecodePatch(); });
        buttons->addWidget(add);
        buttons->addWidget(rescan);
        buttons->addStretch();
        gv->addLayout(buttons);
        static_cast<QVBoxLayout *>(m_tcPatch->layout())->addWidget(g);
    }
    for (QComboBox *c : m_tcPatch->findChildren<QComboBox *>()) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(8);
    }
    refreshTimecodeStatus();
}

void CompositionPanel::refreshTimecodeStatus()
{
    for (auto &[label, text] : m_tcStatus)
        if (label) {
            const QString t = text();
            if (label->text() != t) label->setText(t);
            label->setVisible(!t.isEmpty());
        }
}
