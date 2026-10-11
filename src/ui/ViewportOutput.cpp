#include "ViewportOutput.h"
#include "DeckLink.h"
#include "Engine.h"
#include "OutputWindow.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

static QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

static QSize nativeSize(const QScreen *sc) { return sc->geometry().size() * sc->devicePixelRatio(); }

ViewportOutputPanel::ViewportOutputPanel(Engine *engine, quint64 viewport, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_viewport(viewport)
{
    QSize size;
    QString screen;
    int mode = 0;
    bool testCard = false;
    PublishSettings ps = m_engine->publishSettings(viewport);
    {
        Engine::Lock lk(&m_engine->mutex());
        if (const Layer *l = m_engine->layer(index())) {
            size = l->viewportSize();
            screen = l->vpScreen;
            mode = l->vpMode;
            testCard = l->testCard;
        }
    }
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    // --- Test card: sent instead of the picture while on (a setup aid: not saved)
    m_testCard = new QPushButton(QStringLiteral("Test Card"));
    m_testCard->setCheckable(true);
    m_testCard->setChecked(testCard);
    m_testCard->setToolTip(QStringLiteral("Shows this viewport's test card on its screen instead of its picture: its name "
                                          "and size, a grid and a circle (round when the aspect is right), the corners "
                                          "numbered 1 to 4 clockwise from the top left, color bars, grey steps, and "
                                          "motion (a frozen output shows)."));
    m_testCard->setStyleSheet("QPushButton { padding:6px; } QPushButton:checked { background:#2f6f8f; color:white; font-weight:bold; }");
    v->addWidget(m_testCard);
    connect(m_testCard, &QPushButton::toggled, this, [this](bool on) {
        Engine::Lock lk(&m_engine->mutex());
        if (Layer *l = m_engine->layer(index())) l->testCard = on;
    });

    // --- Picture: its size in pixels
    auto *picture = new QGroupBox(QStringLiteral("Picture"));
    auto *pf = new QFormLayout(picture);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Custom"));
    const QList<QSize> sizes = {{1920, 1080}, {1280, 720}, {3840, 2160}, {4096, 2160}, {1920, 1200},
                                {2560, 1600}, {1400, 1050}, {1024, 768}};
    for (const QSize &s : sizes) m_preset->addItem(QStringLiteral("%1 × %2").arg(s.width()).arg(s.height()), s);
    m_width = new IntBox;
    m_height = new IntBox;
    for (QSpinBox *sb : {m_width, m_height}) {
        sb->setRange(1, 16384);
        sb->setKeyboardTracking(false);
    }
    m_width->setValue(size.width());
    m_height->setValue(size.height());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(size)));
    auto *row = new QHBoxLayout;
    row->addWidget(m_width);
    row->addWidget(new QLabel(QStringLiteral("×")));
    row->addWidget(m_height);
    auto *fit = new QPushButton(QStringLiteral("= screen"));
    fit->setToolTip(QStringLiteral("The native resolution of the screen chosen below"));
    row->addWidget(fit);
    pf->addRow(QStringLiteral("Preset"), m_preset);
    pf->addRow(new ResetLabel(QStringLiteral("Size"), [this] { // 1920 × 1080, the size of a new viewport
        m_syncing = true;
        m_width->setValue(1920);
        m_height->setValue(1080);
        m_syncing = false;
        applySize();
    }),
               row);
    pf->addRow(note(QStringLiteral("Its place in the composition is set in the Spatial tab.")));
    v->addWidget(picture);
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QSize s = m_preset->itemData(i).toSize();
        if (!s.isValid()) return;
        m_syncing = true;
        m_width->setValue(s.width());
        m_height->setValue(s.height());
        m_syncing = false;
        applySize();
    });
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &ViewportOutputPanel::applySize);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &ViewportOutputPanel::applySize);

    // --- Screen and how it is shown there
    auto *out = new QGroupBox(QStringLiteral("Screen"));
    auto *of = new QFormLayout(out);
    m_screen = new QComboBox;
    m_screen->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_screen->setMinimumContentsLength(8);
    for (QScreen *sc : QGuiApplication::screens()) {
        const QSize px = nativeSize(sc);
        m_screen->addItem(QStringLiteral("%1  (%2 × %3)%4")
                              .arg(sc->name())
                              .arg(px.width())
                              .arg(px.height())
                              .arg(sc == QGuiApplication::primaryScreen() ? QStringLiteral(" — main") : QString()),
                          sc->name());
    }
    // No screen chosen yet (or unplugged): the one the window would go to
    const QScreen *target = OutputWindow::screenNamed(screen);
    m_screen->setCurrentIndex(qMax(0, m_screen->findData(target ? target->name() : screen)));
    // The main screen; the two below are set together (applyOutput, further down)
    auto *screenLabel = new ResetLabel(QStringLiteral("Screen"), nullptr);
    of->addRow(screenLabel, m_screen);
    auto *modes = new QHBoxLayout;
    m_mode = new QButtonGroup(this);
    m_mode->setExclusive(true);
    const char *names[] = {"Hidden", "Window", "Fullscreen"};
    for (int k = 0; k < 3; ++k) {
        auto *b = new QPushButton(QString::fromUtf8(names[k]));
        b->setCheckable(true);
        b->setChecked(mode == k);
        b->setStyleSheet(QStringLiteral("QPushButton:checked { background:%1; color:%2; }")
                             .arg(theme::css(), theme::onAccent().name()));
        m_mode->addButton(b, k);
        modes->addWidget(b);
    }
    auto *showLabel = new ResetLabel(QStringLiteral("Show"), nullptr);
    of->addRow(showLabel, modes);
    of->addRow(note(QStringLiteral("⌘F / Ctrl+F puts every viewport fullscreen on its screen at once, "
                                   "and back to how each one was.")));
    v->addWidget(out);
    auto applyOutput = [this] {
        if (m_syncing) return;
        m_engine->setViewportOutput(index(), m_screen->currentData().toString(), m_mode->checkedId());
        emit edited();
        emit outputChanged();
    };
    connect(m_screen, qOverload<int>(&QComboBox::activated), this, applyOutput);
    screenLabel->setReset([this, applyOutput] {
        m_screen->setCurrentIndex(qMax(0, m_screen->findData(QGuiApplication::primaryScreen()->name())));
        applyOutput();
    });
    showLabel->setReset([this, applyOutput] {
        m_mode->button(0)->setChecked(true); // hidden
        applyOutput();
    });
    connect(m_mode, &QButtonGroup::idClicked, this, applyOutput);
    connect(fit, &QPushButton::clicked, this, [this] {
        for (QScreen *sc : QGuiApplication::screens())
            if (sc->name() == m_screen->currentData().toString()) {
                const QSize px = nativeSize(sc);
                m_syncing = true;
                m_width->setValue(px.width());
                m_height->setValue(px.height());
                m_syncing = false;
                applySize();
            }
    });

    // --- Publishing
    auto *pub = new QGroupBox(QStringLiteral("Publishing"));
    auto *pv = new QVBoxLayout(pub);
    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    const QString tips[kPublishKindCount] = {
        QStringLiteral("NDI: network. Requires NDI Tools or the NDI Runtime installed on this machine."),
        QStringLiteral("OMT (Open Media Transport): network, free and open. Requires libomt and libvmx."),
        QStringLiteral("Syphon: shares the picture with apps on the same Mac (MadMapper, Resolume, OBS…)."),
        QStringLiteral("Spout: shares the picture with apps on the same PC (Resolume, TouchDesigner, OBS…)."),
        QString(),
    };
    for (int k = 0; k < kPublishKindCount; ++k) {
        if (k == int(PublishKind::DeckLink)) continue; // its own box, below
        m_pubEnabled[k] = new FlagBox(publishKindName(PublishKind(k)));
        m_pubEnabled[k]->setToolTip(tips[k]);
        m_pubEnabled[k]->setChecked(ps.targets[k].enabled);
        m_pubName[k] = new QLineEdit(ps.targets[k].name);
        m_pubName[k]->setPlaceholderText(QStringLiteral("Source name"));
        m_pubName[k]->setToolTip(QStringLiteral("Name under which this viewport appears on receivers"));
        m_pubState[k] = new QLabel;
        m_pubState[k]->setWordWrap(true);
        m_pubState[k]->setStyleSheet("font-size:11px;");
        grid->addWidget(m_pubEnabled[k], k * 2, 0);
        grid->addWidget(m_pubName[k], k * 2, 1);
        grid->addWidget(m_pubState[k], k * 2 + 1, 0, 1, 2);
        if (!publishCompiledIn(PublishKind(k))) {
            m_pubEnabled[k]->setEnabled(false);
            m_pubName[k]->setEnabled(false);
        }
        connect(m_pubEnabled[k], &QCheckBox::toggled, this, &ViewportOutputPanel::applyPublish);
        connect(m_pubName[k], &QLineEdit::editingFinished, this, &ViewportOutputPanel::applyPublish);
    }
    pv->addLayout(grid);
    auto *form = new QFormLayout;
    m_omtQuality = new QComboBox;
    for (const auto &[label, q] : std::initializer_list<std::pair<const char *, int>>{
             {"Auto", 0}, {"Low", 1}, {"Medium", 50}, {"High", 100}})
        m_omtQuality->addItem(QString::fromUtf8(label), q);
    m_omtQuality->setCurrentIndex(qMax(0, m_omtQuality->findData(ps.omtQuality)));
    form->addRow(new ResetLabel(QStringLiteral("OMT Quality"), [this] {
        m_omtQuality->setCurrentIndex(0); // auto
        applyPublish();
    }),
                 m_omtQuality);
    auto *lib = new QHBoxLayout;
    m_libFolder = new QLineEdit(ps.libraryFolder);
    m_libFolder->setPlaceholderText(QStringLiteral("standard locations"));
    m_libFolder->setToolTip(QStringLiteral("Additional folder to search for the NDI (libndi) and OMT (libomt, libvmx) libraries"));
    auto *browse = new QPushButton(QStringLiteral("…"));
    browse->setFixedWidth(30);
    lib->addWidget(m_libFolder, 1);
    lib->addWidget(browse);
    form->addRow(new ResetLabel(QStringLiteral("Libraries"), [this] {
        m_libFolder->clear(); // the standard locations
        applyPublish();
    }),
                 lib);
    pv->addLayout(form);
    m_libInfo = note(QString());
    pv->addWidget(m_libInfo);
    pv->addWidget(note(QStringLiteral("NDI and OMT send the picture at the viewport's size; "
                                      "Syphon and Spout share it directly on the GPU.")));
    v->addWidget(pub);

    // --- DeckLink: SDI / HDMI out of a Blackmagic card (shown when this machine has a port that plays out)
    m_dlGroup = new QGroupBox(QStringLiteral("DeckLink"));
    auto *df = new QFormLayout(m_dlGroup);
    m_dlEnabled = new FlagBox(QStringLiteral("Play out"));
    m_dlEnabled->setToolTip(QStringLiteral("Sends this viewport to a DeckLink or UltraStudio output (SDI, HDMI), "
                                           "on the card's own clock"));
    m_dlEnabled->setChecked(ps[PublishKind::DeckLink].enabled);
    m_dlPort = new QComboBox;
    m_dlPort->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_dlPort->setMinimumContentsLength(8);
    m_dlMode = new QComboBox;
    m_dlMode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_dlMode->setMinimumContentsLength(8);
    m_dlMode->setToolTip(QStringLiteral("Auto: the mode of the viewport's size, at the closest rate to the render's"));
    auto *dlFit = new QPushButton(QStringLiteral("= mode"));
    dlFit->setToolTip(QStringLiteral("The viewport's size set to the mode's (else its picture is scaled)"));
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(m_dlMode, 1);
    modeRow->addWidget(dlFit);
    m_dlState = new QLabel;
    m_dlState->setWordWrap(true);
    m_dlState->setStyleSheet("font-size:11px;");
    df->addRow(m_dlEnabled);
    df->addRow(QStringLiteral("Output"), m_dlPort);
    df->addRow(QStringLiteral("Mode"), modeRow);
    df->addRow(m_dlState);
    m_dlGroup->setVisible(false);
    v->addWidget(m_dlGroup);
    connect(m_dlEnabled, &QCheckBox::toggled, this, &ViewportOutputPanel::applyPublish);
    connect(m_dlPort, qOverload<int>(&QComboBox::activated), this, [this] {
        m_dlSeen.clear(); // its modes
        refreshDeckLink();
        applyPublish();
    });
    connect(m_dlMode, qOverload<int>(&QComboBox::activated), this, &ViewportOutputPanel::applyPublish);
    connect(dlFit, &QPushButton::clicked, this, [this] {
        const QSize s = m_dlMode->currentData(Qt::UserRole + 1).toSize();
        if (!s.isValid()) return;
        m_syncing = true;
        m_width->setValue(s.width());
        m_height->setValue(s.height());
        m_syncing = false;
        applySize();
    });
    refreshDeckLink();

    connect(m_omtQuality, qOverload<int>(&QComboBox::activated), this, &ViewportOutputPanel::applyPublish);
    connect(m_libFolder, &QLineEdit::editingFinished, this, &ViewportOutputPanel::applyPublish);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("NDI / OMT Library Folder"),
                                                            m_libFolder->text());
        if (d.isEmpty()) return;
        m_libFolder->setText(d);
        applyPublish();
    });
    v->addStretch();
    refreshStatus();
}

int ViewportOutputPanel::index() const { return m_engine->indexOfId(m_viewport); }

void ViewportOutputPanel::applySize()
{
    if (m_syncing) return;
    const QSize s(m_width->value(), m_height->value());
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(s)));
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(index());
        if (!l || s == l->viewportSize()) return;
    }
    m_engine->setViewportSize(index(), s);
    emit edited();
}

void ViewportOutputPanel::applyPublish()
{
    PublishSettings s = m_engine->publishSettings(m_viewport);
    for (int k = 0; k < kPublishKindCount; ++k) {
        if (!m_pubEnabled[k]) continue;
        s.targets[k].enabled = m_pubEnabled[k]->isChecked();
        const QString n = m_pubName[k]->text().trimmed();
        s.targets[k].name = n.isEmpty() ? QStringLiteral("Fulskrin") : n;
    }
    s.omtQuality = m_omtQuality->currentData().toInt();
    s.libraryFolder = m_libFolder->text().trimmed();
    if (m_dlGroup->isVisible()) { // no port now: its settings stay as they are (it may come back)
        s[PublishKind::DeckLink].enabled = m_dlEnabled->isChecked();
        s[PublishKind::DeckLink].name = m_dlPort->currentData().toString();
        s.decklinkMode = m_dlMode->currentData().toString();
    }
    if (s == m_engine->publishSettings(m_viewport)) return;
    m_engine->setPublishSettings(m_viewport, s);
    emit edited();
    refreshStatus();
}

void ViewportOutputPanel::refreshStatus()
{
    int mode = 0;
    bool testCard = false;
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(index());
        if (!l) return;
        mode = l->vpMode;
        testCard = l->testCard;
    }
    if (QAbstractButton *b = m_mode->button(mode); b && !b->isChecked()) b->setChecked(true);
    if (m_testCard && m_testCard->isChecked() != testCard) { // set by OSC
        const QSignalBlocker block(m_testCard);
        m_testCard->setChecked(testCard);
    }
    for (int k = 0; k < kPublishKindCount; ++k) {
        if (!m_pubState[k]) continue;
        const PublishState st = m_engine->publishState(m_viewport, PublishKind(k));
        QString text = st.text, color = "#888";
        if (!publishCompiledIn(PublishKind(k))) {
            text = k == int(PublishKind::Syphon) ? QStringLiteral("macOS only") : QStringLiteral("Windows only");
        } else if (st.level == PublishState::Ok) {
            color = "#5fd47a";
            if (st.receivers > 0) text += k == int(PublishKind::Syphon) ? QStringLiteral(" — client connected")
                                                                        : QStringLiteral(" — %1 receiver(s)").arg(st.receivers);
            else if (st.receivers == 0) text += QStringLiteral(" — no receivers");
        } else if (st.level == PublishState::Error) {
            color = "#ff6e5f";
        }
        if (text.isEmpty()) text = QStringLiteral("Disabled");
        m_pubState[k]->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, text.toHtmlEscaped()));
    }
    {
        const PublishState st = m_engine->publishState(m_viewport, PublishKind::DeckLink);
        const QString color = st.level == PublishState::Ok ? "#5fd47a" : st.level == PublishState::Error ? "#ff6e5f" : "#888";
        m_dlState->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                               .arg(color, (st.text.isEmpty() ? QStringLiteral("Off") : st.text).toHtmlEscaped()));
    }
    // Library lookup and the DeckLink ports: roughly every two seconds (disk access, the driver)
    if (m_libTick++ % 20 != 0) return;
    refreshDeckLink();
    const QString folder = m_libFolder->text().trimmed();
    const QString ndi = ndiLibraryPath(folder), omt = omtLibraryPath(folder);
    m_libInfo->setText(QStringLiteral("NDI: %1<br>OMT: %2")
                           .arg(ndi.isEmpty() ? QStringLiteral("not found") : ndi.toHtmlEscaped(),
                                omt.isEmpty() ? QStringLiteral("not found") : omt.toHtmlEscaped()));
}

void ViewportOutputPanel::refreshDeckLink()
{
    const std::vector<decklink::Port> ports = decklink::outputPorts();
    QString seen;
    for (const decklink::Port &p : ports) seen += p.id + '\n';
    m_dlGroup->setVisible(!ports.empty());
    if (seen == m_dlSeen) return;
    m_dlSeen = seen;
    const PublishSettings ps = m_engine->publishSettings(m_viewport);
    // The port: the one chosen if it is here (else the first; one chosen but gone is kept, marked)
    const QString want = m_dlPort->count() ? m_dlPort->currentData().toString() : ps[PublishKind::DeckLink].name;
    m_dlPort->clear();
    for (const decklink::Port &p : ports) m_dlPort->addItem(p.name, p.id);
    int at = m_dlPort->findData(want);
    if (at < 0 && !want.isEmpty()) {
        m_dlPort->addItem(QStringLiteral("%1 (not found)").arg(want), want);
        at = m_dlPort->count() - 1;
    }
    m_dlPort->setCurrentIndex(std::max(0, at));
    // Its modes
    const QString mode = m_dlMode->count() ? m_dlMode->currentData().toString() : ps.decklinkMode;
    m_dlMode->clear();
    m_dlMode->addItem(QStringLiteral("Auto"), QString());
    for (const decklink::Mode &m : decklink::outputModes(m_dlPort->currentData().toString())) {
        m_dlMode->addItem(QStringLiteral("%1  (%2 × %3)").arg(m.name).arg(m.width).arg(m.height), m.key);
        m_dlMode->setItemData(m_dlMode->count() - 1, QSize(m.width, m.height), Qt::UserRole + 1);
    }
    m_dlMode->setCurrentIndex(std::max(0, m_dlMode->findData(mode)));
}
