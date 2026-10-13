#include "ParamPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Osc.h"
#include "ParamAnimPanel.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QSpinBox>
#include <QVBoxLayout>
#include <array>
#include <memory>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QAbstractItemView>
#include <QPointer>
#include <QUndoStack>
#include <cmath>

static void setSwatch(QPushButton *b, const float c[4])
{
    const QColor col = QColor::fromRgbF(c[0], c[1], c[2], c[3]);
    b->setText(col.name(QColor::HexArgb).toUpper());
    b->setStyleSheet(QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid #555; padding:3px 8px; }")
                         .arg(col.name(QColor::HexRgb), col.lightnessF() > 0.55 ? "#000" : "#fff"));
}

void ParamPanel::setValue(int input, const QString &label, const std::function<void(IsfValue &)> &modify)
{
    IsfInput in;
    {
        Engine::Lock lk(&m_engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
        if (!inst || input >= int(inst->inputs().size())) return;
        in = inst->inputs()[size_t(input)];
    }
    IsfValue after = in.value();
    modify(after);
    if (after == in.value()) return;
    cmd::setValues(m_undo, m_engine, m_layerId, cmd::isfValueJson(m_engine->shaderPrefix(m_layerId, m_slot), in, after),
                   QStringLiteral("Change \"%1\"").arg(label));
}

ParamPanel::ParamPanel(Engine *engine, QUndoStack *undo, int layer, int slot, int options, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo), m_layer(layer), m_layerId(engine->layerId(layer)), m_slot(slot)
{
    // Copy metadata and values under the lock: widgets are then built without blocking rendering.
    std::vector<IsfInput> inputs;
    QString description;
    {
        Engine::Lock lk(&engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(engine, layer, slot);
        if (inst) {
            inputs = inst->inputs();
            description = inst->description();
        }
    }

    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 4, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    if (!description.isEmpty()) {
        auto *d = new QLabel(description);
        d->setWordWrap(true);
        d->setStyleSheet("color:#999; font-size:11px;");
        form->addRow(d);
    }

    // Speed of TIME, for every shader (TIME goes on from where it is: no jump)
    if (!(options & NoSpeed)) {
        double speed = 1.0;
        {
            Engine::Lock lk(&engine->mutex());
            if (IsfInstance *inst = cmd::resolveIsf(engine, layer, slot)) speed = inst->speed;
        }
        m_speed = new SliderField;
        m_speed->setRange(0, 10);
        m_speed->setDecimals(2);
        m_speed->setSingleStep(0.05);
        m_speed->setTicks(10);
        m_speed->setSnaps({1});
        m_speed->setValue(speed);
        m_speed->setToolTip(QStringLiteral("Speed of the shader's time (1 = normal, 0 = stopped)"));
        connect(m_speed, &SliderField::valueEdited, this, [this](double v) {
            cmd::setValue(m_undo, m_engine, m_layerId, m_engine->shaderPrefix(m_layerId, m_slot) + QStringLiteral("speed"), v,
                          QStringLiteral("Change Speed"));
        });
        auto *name = new ResetLabel(QStringLiteral("Speed"), [this] {
            m_speed->setValue(1);
            emit m_speed->valueEdited(1);
        });
        form->addRow(name, m_speed);
        m_rows.push_back({QString(), {name, m_speed}});
    }

    for (int idx = 0; idx < int(inputs.size()); ++idx) {
        const IsfInput &in = inputs[size_t(idx)];
        if (in.type == IsfInput::Image && in.isInputImage) continue;
        const QString label = in.label;
        QWidget *field = nullptr;
        QWidget *pointX = nullptr, *pointY = nullptr;
        std::function<void()> reset; // an image: cleared by a click on its name

        switch (in.type) {
        case IsfInput::Float: {
            const double span = in.fMax - in.fMin;
            auto *bar = new SliderField;
            bar->setRange(in.fMin, in.fMax);
            bar->setDecimals(span >= 100 ? 1 : span >= 10 ? 2 : 3);
            bar->setSingleStep(span / 100.0);
            bar->setTicks(10);
            bar->setValue(in.fValue);
            connect(bar, &SliderField::valueEdited, this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &x) { x.f = v; }); });
            m_followers.push_back({idx, [bar = QPointer<SliderField>(bar)](const IsfValue &x) {
                                       if (bar && !bar->isDragging() && std::abs(bar->value() - x.f) > 1e-6) bar->setValue(x.f);
                                   }});
            field = bar;
            break;
        }
        case IsfInput::Bool: {
            auto *c = new FlagBox;
            c->setChecked(in.bValue);
            connect(c, &QCheckBox::toggled, this, [=](bool on) { setValue(idx, label, [on](IsfValue &x) { x.b = on; }); });
            m_followers.push_back({idx, [c = QPointer<QCheckBox>(c)](const IsfValue &x) {
                                       if (!c || c->isChecked() == x.b) return;
                                       QSignalBlocker blk(c);
                                       c->setChecked(x.b);
                                   }});
            field = c;
            break;
        }
        case IsfInput::Event: {
            auto *b = new QPushButton(QStringLiteral("Trigger"));
            connect(b, &QPushButton::clicked, this, [=] {
                Engine::Lock lk(&m_engine->mutex());
                if (IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot))
                    if (idx < int(inst->inputs().size())) inst->inputs()[size_t(idx)].eventFired = true;
            });
            field = b;
            break;
        }
        case IsfInput::Long: {
            auto *c = new QComboBox;
            for (int k = 0; k < in.lValues.size(); ++k) c->addItem(in.lLabels.value(k), in.lValues[k]);
            c->setCurrentIndex(qMax(0, in.lValues.indexOf(in.lValue)));
            connect(c, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int k) {
                const int v = c->itemData(k).toInt();
                setValue(idx, label, [v](IsfValue &x) { x.l = v; });
            });
            m_followers.push_back({idx, [c = QPointer<QComboBox>(c)](const IsfValue &x) {
                                       if (!c || c->view()->isVisible()) return; // its list is open
                                       const int k = c->findData(x.l);
                                       if (k < 0 || k == c->currentIndex()) return;
                                       QSignalBlocker blk(c);
                                       c->setCurrentIndex(k);
                                   }});
            field = c;
            break;
        }
        case IsfInput::Point2D: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *x = new NumberBox, *y = new NumberBox;
            const QPointF mn = in.hasPointRange ? in.pMin : QPointF(-100000, -100000);
            const QPointF mx = in.hasPointRange ? in.pMax : QPointF(100000, 100000);
            x->setRange(mn.x(), mx.x());
            y->setRange(mn.y(), mx.y());
            for (auto *s : {x, y}) {
                s->setDecimals(3);
                s->setSingleStep(in.hasPointRange ? (mx.x() - mn.x()) / 100.0 : 1.0);
                s->setKeyboardTracking(false);
            }
            x->setValue(in.pValue.x());
            y->setValue(in.pValue.y());
            x->setPrefix("x ");
            y->setPrefix("y ");
            pointX = x;
            pointY = y;
            h->addWidget(x);
            h->addWidget(y);
            connect(x, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setX(v); }); });
            connect(y, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [=](double v) { setValue(idx, label, [v](IsfValue &p) { p.p.setY(v); }); });
            m_followers.push_back({idx, [x = QPointer<QDoubleSpinBox>(x), y = QPointer<QDoubleSpinBox>(y)](const IsfValue &v) {
                                       for (auto [box, value] : {std::pair{x.data(), v.p.x()}, std::pair{y.data(), v.p.y()}}) {
                                           if (!box || box->hasFocus() || std::abs(box->value() - value) < 1e-6) continue;
                                           QSignalBlocker blk(box);
                                           box->setValue(value);
                                       }
                                   }});
            field = w;
            break;
        }
        case IsfInput::Color: {
            // One embedded editor. Each parameter remembers its own view.
            auto *editor = new QWidget;
            auto *layout = new QVBoxLayout(editor);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            auto *header = new QHBoxLayout;
            auto *swatch = new QLabel;
            swatch->setFixedSize(64, 22);
            auto *mode = new QComboBox;
            mode->addItems({QStringLiteral("RGBA"), QStringLiteral("HSVA"), QStringLiteral("Palette")});
            const QString key = QStringLiteral("ui/colorMode/%1/%2/%3")
                                    .arg(m_layerId).arg(m_slot).arg(in.name);
            mode->setCurrentIndex(std::clamp(QSettings().value(key, 0).toInt(), 0, 2));
            header->addWidget(swatch);
            header->addWidget(mode, 1);
            layout->addLayout(header);
            auto *channels = new QWidget;
            auto *rows = new QFormLayout(channels);
            rows->setContentsMargins(0, 0, 0, 0);
            // Callbacks capture these arrays before the widgets are populated.
            // Keep shared arrays so the captures see the initialized pointers.
            auto sliders = std::make_shared<std::array<QSlider *, 4>>();
            auto captions = std::make_shared<std::array<QLabel *, 4>>();
            auto numbers = std::make_shared<std::array<QSpinBox *, 4>>();
            auto current = [=]() -> QColor {
                Engine::Lock lk(&m_engine->mutex());
                IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
                if (!inst || idx >= int(inst->inputs().size())) return QColor();
                const IsfValue v = inst->inputs()[size_t(idx)].value();
                return QColor::fromRgbF(v.c[0], v.c[1], v.c[2], v.c[3]);
            };
            auto refresh = [=](const QColor &c) {
                if (!c.isValid()) return;
                swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #777;")
                                          .arg(c.name(QColor::HexArgb)));
                const bool hsv = mode->currentIndex() == 1;
                const int values[4] = {hsv ? c.hsvHue() : c.red(),
                                       hsv ? c.hsvSaturation() : c.green(),
                                       hsv ? c.value() : c.blue(), c.alpha()};
                const char *rgbNames[] = {"R", "G", "B", "A"};
                const char *hsvNames[] = {"H", "S", "V", "A"};
                for (int k = 0; k < 4; ++k) {
                    (*captions)[k]->setText(QLatin1String(hsv ? hsvNames[k] : rgbNames[k]));
                    const int maximum = hsv && k == 0 ? 359 : 255;
                    // Block range-change signals as well as value-change signals.
                    // Otherwise setMaximum can trigger a reentrant refresh while
                    // the color editor is still being constructed.
                    QSignalBlocker bs((*sliders)[k]), bn((*numbers)[k]);
                    (*sliders)[k]->setMaximum(maximum);
                    (*numbers)[k]->setMaximum(maximum);
                    (*sliders)[k]->setValue(std::max(0, values[k]));
                    (*numbers)[k]->setValue(std::max(0, values[k]));
                }
            };
            for (int k = 0; k < 4; ++k) {
                auto *line = new QWidget;
                auto *h = new QHBoxLayout(line);
                h->setContentsMargins(0, 0, 0, 0);
                (*captions)[k] = new QLabel;
                (*sliders)[k] = new QSlider(Qt::Horizontal);
                (*numbers)[k] = new QSpinBox;
                (*numbers)[k]->setFixedWidth(65);
                h->addWidget((*sliders)[k], 1);
                h->addWidget((*numbers)[k]);
                rows->addRow((*captions)[k], line);
                auto update = [=](int value) {
                    QColor c = current();
                    if (!c.isValid()) return;
                    if (mode->currentIndex() == 1) {
                        int h = std::max(0, c.hsvHue()), s = c.hsvSaturation(), v = c.value(), alpha = c.alpha();
                        if (k == 0) h = value;
                        if (k == 1) s = value;
                        if (k == 2) v = value;
                        if (k == 3) alpha = value;
                        c.setHsv(h, s, v, alpha);
                    } else {
                        int v[4] = {c.red(), c.green(), c.blue(), c.alpha()};
                        v[k] = value;
                        c.setRgb(v[0], v[1], v[2], v[3]);
                    }
                    const float rgba[4] = {float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())};
                    setValue(idx, label, [rgba](IsfValue &x) { std::copy(rgba, rgba + 4, x.c); });
                    refresh(c);
                };
                connect((*sliders)[k], &QSlider::valueChanged, editor, update);
                connect((*numbers)[k], qOverload<int>(&QSpinBox::valueChanged), editor, update);
            }
            layout->addWidget(channels);
            auto *palette = new QWidget;
            auto *paletteGrid = new QGridLayout(palette);
            paletteGrid->setContentsMargins(0, 0, 0, 0);
            const QStringList defaults = { "#ffffff", "#808080", "#000000", "#ffff00", "#ff00ff", "#00ffff",
                "#ff0000", "#00ff00", "#0000ff", "#ff8000", "#8000ff", "#00a080", "#e0b080", "#404040",
                "#804020", "#ffc0cb", "#4080ff", "#80ff80", "#c0c0c0", "#101010" };
            for (int n = 0; n < defaults.size(); ++n) {
                auto *pick = new QPushButton;
                pick->setFixedHeight(22);
                pick->setStyleSheet(QStringLiteral("background:%1; border:1px solid #444;").arg(defaults[n]));
                paletteGrid->addWidget(pick, n / 5, n % 5);
                connect(pick, &QPushButton::clicked, editor, [=] {
                    QColor c(defaults[n]);
                    c.setAlpha(current().alpha());
                    const float rgba[4] = {float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())};
                    setValue(idx, label, [rgba](IsfValue &x) { std::copy(rgba, rgba + 4, x.c); });
                    refresh(c);
                });
            }
            layout->addWidget(palette);
            connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), editor, [=](int index) {
                QSettings().setValue(key, index);
                channels->setVisible(index != 2);
                palette->setVisible(index == 2);
                refresh(current());
            });
            channels->setVisible(mode->currentIndex() != 2);
            palette->setVisible(mode->currentIndex() == 2);
            refresh(QColor::fromRgbF(in.cValue[0], in.cValue[1], in.cValue[2], in.cValue[3]));
            m_followers.push_back({idx, [refresh](const IsfValue &v) {
                refresh(QColor::fromRgbF(v.c[0], v.c[1], v.c[2], v.c[3]));
            }});
            field = editor;
            break;
        }
        case IsfInput::Image: {
            auto *w = new QWidget;
            auto *h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto *name = new QLabel(in.imagePath.isEmpty() ? QStringLiteral("(none)") : QFileInfo(in.imagePath).fileName());
            name->setStyleSheet("color:#bbb;");
            auto *pick = new QPushButton(QStringLiteral("Image…"));
            auto *clear = new QPushButton(QStringLiteral("×"));
            clear->setFixedWidth(28);
            h->addWidget(name, 1);
            h->addWidget(pick);
            h->addWidget(clear);
            auto instance = [this] {
                Engine::Lock lk(&m_engine->mutex());
                return cmd::resolveIsf(m_engine, m_layer, m_slot);
            };
            connect(pick, &QPushButton::clicked, this, [=] {
                QSettings s;
                const QString f = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Image for %1").arg(label), s.value("dirs/image").toString(),
                    QStringLiteral("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.gif *.webp *.tga)"));
                if (f.isEmpty()) return;
                s.setValue("dirs/image", QFileInfo(f).absolutePath());
                QString err;
                if (m_engine->setIsfImageInput(instance(), idx, f, &err)) name->setText(QFileInfo(f).fileName());
                else name->setText(err);
            });
            connect(clear, &QPushButton::clicked, this, [=] {
                m_engine->setIsfImageInput(instance(), idx, QString());
                name->setText(QStringLiteral("(none)"));
            });
            reset = [clear] { clear->click(); };
            field = w;
            break;
        }
        case IsfInput::Audio:
        case IsfInput::AudioFFT: {
            auto *l = new QLabel(QStringLiteral("audio input not supported (V1)"));
            l->setStyleSheet("color:#888;");
            field = l;
            break;
        }
        default: continue;
        }
        if (in.type == IsfInput::Event || in.type == IsfInput::Image || in.type == IsfInput::Audio ||
            in.type == IsfInput::AudioFFT) {
            if (reset) form->addRow(new ResetLabel(label, reset), field);
            else form->addRow(label, field); // a trigger, or nothing to set
            continue;
        }
        // A click on the parameter's name puts it back to its default value
        IsfValue def;
        def.f = in.fDefault;
        def.b = in.bDefault;
        def.l = in.lDefault;
        def.p = in.pDefault;
        std::copy(in.cDefault, in.cDefault + 4, def.c);
        auto *name = new ResetLabel(label, [this, idx, label, def] {
            setValue(idx, label, [def](IsfValue &x) { x = def; });
            emit rebuildRequested();
        });
        form->addRow(name, field);
        m_rows.push_back({in.name, {name, field}, pointX, pointY});
    }

    if (!(options & NoReset)) {
        auto *reset = new QPushButton(QStringLiteral("Reset to Defaults"));
        reset->setFlat(true);
        reset->setStyleSheet("color:#aaa; text-align:left;");
        connect(reset, &QPushButton::clicked, this, &ParamPanel::resetToDefaults);
        form->addRow(reset);
    }
}

void ParamPanel::resetToDefaults()
{
    {
        // A single undo step for all parameters
        QJsonObject defaults;
        const QString prefix = m_engine->shaderPrefix(m_layerId, m_slot);
        {
            Engine::Lock lk(&m_engine->mutex());
            IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
            if (!inst) return;
            for (const IsfInput &in : inst->inputs()) {
                IsfValue def;
                def.f = in.fDefault;
                def.b = in.bDefault;
                def.l = in.lDefault;
                def.p = in.pDefault;
                std::copy(in.cDefault, in.cDefault + 4, def.c);
                const QJsonObject o = cmd::isfValueJson(prefix, in, def);
                for (auto it = o.begin(); it != o.end(); ++it) defaults.insert(it.key(), it.value());
            }
        }
        cmd::setValues(m_undo, m_engine, m_layerId, defaults, QStringLiteral("Reset to Defaults"));
        emit rebuildRequested();
    }
}

void ParamPanel::attachAnimate(AnimateMenu *menu)
{
    if (!menu) return;
    // The addresses of this shader's numbers in the layer: the generator's, or the effect's (its segment)
    QString base, speed;
    {
        Engine::Lock lk(&m_engine->mutex());
        const Layer *l = m_engine->layer(m_layer);
        if (!l) return;
        if (m_slot < 0) {
            base = QStringLiteral("param/");
            speed = QStringLiteral("speed");
        } else {
            QStringList names;
            for (const auto &x : l->effects) names << x->name();
            const QStringList segs = osc::uniqueSegments(names);
            if (m_slot >= segs.size()) return;
            base = QStringLiteral("fx/%1/param/").arg(segs[m_slot]);
            speed = QStringLiteral("fx/%1/speed").arg(segs[m_slot]);
        }
    }
    for (const Row &r : m_rows) {
        if (r.input.isEmpty()) {
            for (QWidget *w : r.all) menu->attach(w, {speed});
            continue;
        }
        const QString p = base + osc::safeName(r.input);
        // Every number this input has (only the ones declared animatable are offered)
        const QStringList all{p, p + "/x", p + "/y", p + "/r", p + "/g", p + "/b", p + "/a"};
        for (QWidget *w : r.all) menu->attach(w, all);
        menu->attach(r.x, {p + "/x"});
        menu->attach(r.y, {p + "/y"});
    }
}

void ParamPanel::refresh()
{
    std::vector<IsfValue> values;
    double speed = 1.0;
    {
        Engine::Lock lk(&m_engine->mutex());
        IsfInstance *inst = cmd::resolveIsf(m_engine, m_layer, m_slot);
        if (!inst) return;
        speed = inst->speed;
        for (const IsfInput &in : inst->inputs()) values.push_back(in.value());
    }
    if (m_speed && !m_speed->isDragging() && std::abs(m_speed->value() - speed) > 1e-6) m_speed->setValue(speed);
    for (const auto &[input, show] : m_followers)
        if (input < int(values.size())) show(values[size_t(input)]);
}
