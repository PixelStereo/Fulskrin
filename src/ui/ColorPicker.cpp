#include "ColorPicker.h"
#include "Widgets.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
constexpr double kPi = 3.14159265358979323846;

QGradientStops channelGradient(const QColor &a, const QColor &b)
{
    return {{0.0, a}, {1.0, b}};
}

QGradientStops hueGradient()
{
    QGradientStops stops;
    for (int i = 0; i <= 6; ++i)
        stops.append({i / 6.0, QColor::fromHsv(i == 6 ? 0 : i * 60, 255, 255)});
    return stops;
}
} // namespace

// Painted in Qt, without platform color dialogs or graphics backends.
class HueWheelWidget final : public QWidget
{
public:
    explicit HueWheelWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(232, 232);
        setCursor(Qt::CrossCursor);
    }

    std::function<void(const QColor &)> onEdited;
    int hue() const { return m_hue; }
    void setHue(int hue) { m_hue = (hue % 360 + 360) % 360; update(); }

    void setColor(const QColor &c)
    {
        if (!c.isValid()) return;
        m_color = c;
        if (c.hsvSaturation() > 0 && c.hsvHue() >= 0) m_hue = c.hsvHue();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF origin(width() / 2.0, height() / 2.0);
        const double outer = radius(), inner = outer - 23.0;
        // Each ring stroke is independently colored: clockwise HSV hues.
        for (int h = 0; h < 360; ++h) {
            const double a = double(h) * kPi / 180.0;
            const QPointF direction(std::cos(a), std::sin(a));
            p.setPen(QPen(QColor::fromHsv(h, 255, 255), 2.8));
            p.drawLine(origin + direction * inner, origin + direction * outer);
        }

        const QRectF square = svRect();
        const QColor hueColor = QColor::fromHsv(m_hue, 255, 255);
        QLinearGradient saturation(square.topLeft(), square.topRight());
        saturation.setColorAt(0, Qt::white);
        saturation.setColorAt(1, hueColor);
        p.fillRect(square, saturation);
        QLinearGradient brightness(square.topLeft(), square.bottomLeft());
        brightness.setColorAt(0, QColor(0, 0, 0, 0));
        brightness.setColorAt(1, Qt::black);
        p.fillRect(square, brightness);
        p.setPen(QPen(QColor(30, 30, 30), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(square);

        const double angle = double(m_hue) * kPi / 180.0;
        const QPointF huePoint = origin + QPointF(std::cos(angle), std::sin(angle)) * ((inner + outer) / 2.0);
        p.setPen(QPen(Qt::white, 2.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(huePoint, 6.5, 6.5);
        p.setPen(QPen(Qt::black, 1));
        p.drawEllipse(huePoint, 8, 8);

        const QPointF svPoint(square.left() + square.width() * m_color.hsvSaturationF(),
                              square.top() + square.height() * (1.0 - m_color.valueF()));
        p.setPen(QPen(Qt::black, 2.5));
        p.drawEllipse(svPoint, 8, 8);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawEllipse(svPoint, 6.5, 6.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        const QPointF d = event->position() - QPointF(width() / 2.0, height() / 2.0);
        const double r = std::hypot(d.x(), d.y());
        m_drag = r >= radius() - 26 && r <= radius() + 5 ? 1 : svRect().contains(event->position()) ? 2 : 0;
        if (!m_drag) return;
        updateFromPoint(event->position());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_drag) {
            updateFromPoint(event->position());
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) m_drag = 0;
    }

private:
    double radius() const { return std::min(width(), height()) / 2.0 - 6; }
    QRectF svRect() const
    {
        const double side = (radius() - 23.0) * 1.36;
        return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    }

    void updateFromPoint(QPointF pos)
    {
        int saturation = m_color.hsvSaturation(), value = m_color.value();
        if (m_drag == 1) {
            const QPointF d = pos - QPointF(width() / 2.0, height() / 2.0);
            double angle = std::atan2(d.y(), d.x());
            if (angle < 0) angle += 2 * kPi;
            m_hue = int(std::lround(angle * 180.0 / kPi)) % 360;
        } else if (m_drag == 2) {
            const QRectF square = svRect();
            saturation = int(std::lround(255.0 * std::clamp((pos.x() - square.left()) / square.width(), 0.0, 1.0)));
            value = int(std::lround(255.0 * (1.0 - std::clamp((pos.y() - square.top()) / square.height(), 0.0, 1.0))));
        }
        m_color = QColor::fromHsv(m_hue, saturation, value, m_color.alpha());
        update();
        if (onEdited) onEdited(m_color);
    }

    QColor m_color = Qt::white;
    int m_hue = 0, m_drag = 0;
};

ColorPicker::ColorPicker(bool withAlpha, QWidget *parent) : QWidget(parent), m_withAlpha(withAlpha)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *head = new QHBoxLayout;
    head->setSpacing(3);
    m_swatch = new QLabel;
    m_swatch->setFixedSize(66, 22);
    head->addWidget(m_swatch);
    head->addStretch(1);
    const QStringList names = {QStringLiteral("RGB"), QStringLiteral("HSV"), QStringLiteral("◉"), QStringLiteral("▦")};
    const QStringList tips = {QStringLiteral("RGB channels"), QStringLiteral("HSV channels"),
                              QStringLiteral("Hue wheel and saturation/value square"), QStringLiteral("Color palette")};
    for (int i = 0; i < 4; ++i) {
        auto *button = new QToolButton;
        button->setText(names[i]);
        button->setToolTip(tips[i]);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFixedHeight(25);
        button->setProperty("allowLocked", true);
        head->addWidget(button);
        m_buttons[i] = button;
        connect(button, &QToolButton::clicked, this, [this, i] { setView(static_cast<View>(i)); });
    }
    layout->addLayout(head);

    m_channelPage = new QWidget;
    auto *channels = new QGridLayout(m_channelPage);
    channels->setContentsMargins(0, 0, 0, 0);
    channels->setHorizontalSpacing(5);
    channels->setVerticalSpacing(3);
    for (int i = 0; i < (m_withAlpha ? 4 : 3); ++i) {
        m_labels[i] = new QLabel;
        m_labels[i]->setFixedWidth(15);
        m_fields[i] = new SliderField;
        m_fields[i]->setCompact(true);
        m_fields[i]->setRange(0, 255);
        m_fields[i]->setDecimals(0);
        m_fields[i]->setSingleStep(1);
        channels->addWidget(m_labels[i], i, 0);
        channels->addWidget(m_fields[i], i, 1);
        connect(m_fields[i], &SliderField::valueEdited, this,
                [this, i](double v) { if (!m_syncing) editChannel(i, v); });
    }
    channels->setColumnStretch(1, 1);
    layout->addWidget(m_channelPage);

    m_wheelPage = new QWidget;
    auto *wheelLayout = new QVBoxLayout(m_wheelPage);
    wheelLayout->setContentsMargins(0, 0, 0, 0);
    wheelLayout->setSpacing(5);
    m_wheel = new HueWheelWidget;
    wheelLayout->addWidget(m_wheel, 0, Qt::AlignHCenter);
    m_wheel->onEdited = [this](const QColor &c) {
        m_hue = m_wheel->hue();
        editColor(c);
    };
    if (m_withAlpha) {
        auto *alphaRow = new QHBoxLayout;
        auto *name = new QLabel(QStringLiteral("A"));
        name->setFixedWidth(15);
        m_wheelAlpha = new SliderField;
        m_wheelAlpha->setCompact(true);
        m_wheelAlpha->setRange(0, 255);
        m_wheelAlpha->setDecimals(0);
        m_wheelAlpha->setSingleStep(1);
        alphaRow->addWidget(name);
        alphaRow->addWidget(m_wheelAlpha, 1);
        wheelLayout->addLayout(alphaRow);
        connect(m_wheelAlpha, &SliderField::valueEdited, this,
                [this](double v) { if (!m_syncing) editChannel(3, v); });
    }
    layout->addWidget(m_wheelPage);

    m_palettePage = new QWidget;
    auto *palette = new QGridLayout(m_palettePage);
    palette->setContentsMargins(0, 0, 0, 0);
    palette->setSpacing(3);
    const QStringList defaults = {
        "#ffffff", "#808080", "#000000", "#ffff00", "#ff00ff", "#00ffff",
        "#ff0000", "#00ff00", "#0000ff", "#ff8000", "#8000ff", "#00a080",
        "#e0b080", "#404040", "#804020", "#ffc0cb", "#4080ff", "#80ff80",
        "#c0c0c0", "#101010"
    };
    for (int i = 0; i < defaults.size(); ++i) {
        auto *pick = new QToolButton;
        pick->setFixedHeight(23);
        pick->setMinimumWidth(22);
        pick->setStyleSheet(QStringLiteral("background:%1; border:1px solid #555;").arg(defaults[i]));
        pick->setToolTip(defaults[i]);
        palette->addWidget(pick, i / 5, i % 5);
        connect(pick, &QToolButton::clicked, this, [this, color = QColor(defaults[i])] {
            QColor c = color;
            c.setAlpha(m_color.alpha());
            editColor(c);
        });
    }
    for (int i = 0; i < 5; ++i) palette->setColumnStretch(i, 1);
    layout->addWidget(m_palettePage);

    updateControls();
    setView(Wheel);
}

void ColorPicker::setSettingsKey(const QString &key, const QString &legacyKey)
{
    m_settingsKey = key;
    QSettings settings;
    int value = static_cast<int>(m_view);
    if (!key.isEmpty() && settings.contains(key)) {
        value = settings.value(key).toInt();
    } else if (!legacyKey.isEmpty() && settings.contains(legacyKey)) {
        const int old = settings.value(legacyKey).toInt();
        value = old == 2 ? Palette : std::clamp(old, 0, 1);
    }
    setView(static_cast<View>(std::clamp(value, 0, 3)));
}

void ColorPicker::setView(View view)
{
    const int index = std::clamp(static_cast<int>(view), 0, 3);
    m_view = static_cast<View>(index);
    for (int i = 0; i < 4; ++i) m_buttons[i]->setChecked(i == index);
    m_channelPage->setVisible(m_view == RGB || m_view == HSV);
    m_wheelPage->setVisible(m_view == Wheel);
    m_palettePage->setVisible(m_view == Palette);
    updateControls();
    if (!m_settingsKey.isEmpty()) QSettings().setValue(m_settingsKey, index);
}

void ColorPicker::setColor(const QColor &color)
{
    if (!color.isValid()) return;
    QColor c = color.toRgb();
    if (!m_withAlpha) c.setAlpha(255);
    if (c == m_color) return;
    m_color = c;
    if (c.hsvSaturation() > 0 && c.hsvHue() >= 0) m_hue = c.hsvHue();
    updateControls();
}

void ColorPicker::editColor(const QColor &color)
{
    if (!color.isValid()) return;
    QColor c = color.toRgb();
    if (!m_withAlpha) c.setAlpha(255);
    if (m_color == c) return;
    m_color = c;
    if (c.hsvSaturation() > 0 && c.hsvHue() >= 0) m_hue = c.hsvHue();
    updateControls();
    emit colorEdited(m_color);
}

void ColorPicker::editChannel(int channel, double value)
{
    const int v = std::clamp(int(std::lround(value)), 0, channel == 0 && m_view == HSV ? 359 : 255);
    QColor c = m_color;
    if (m_view == HSV && channel < 3) {
        int h = m_hue, s = c.hsvSaturation(), brightness = c.value();
        if (channel == 0) h = v;
        if (channel == 1) s = v;
        if (channel == 2) brightness = v;
        m_hue = h;
        c.setHsv(h, s, brightness, c.alpha());
    } else if (channel == 3) {
        c.setAlpha(v);
    } else {
        int rgb[3] = {c.red(), c.green(), c.blue()};
        rgb[channel] = v;
        c.setRgb(rgb[0], rgb[1], rgb[2], c.alpha());
    }
    editColor(c);
}

void ColorPicker::updateControls()
{
    m_syncing = true;
    m_swatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #777;").arg(m_color.name(QColor::HexArgb)));
    m_swatch->setToolTip(m_color.name(m_withAlpha ? QColor::HexArgb : QColor::HexRgb).toUpper());
    for (int i = 0; i < (m_withAlpha ? 4 : 3); ++i) {
        const bool hsv = m_view == HSV;
        m_labels[i]->setText(QString::fromLatin1(hsv ? "HSVA" : "RGBA").mid(i, 1));
        m_fields[i]->setRange(0, hsv && i == 0 ? 359 : 255);
        m_fields[i]->setValue(i == 3 ? m_color.alpha()
                                      : hsv ? (i == 0 ? m_hue : i == 1 ? m_color.hsvSaturation() : m_color.value())
                                            : (i == 0 ? m_color.red() : i == 1 ? m_color.green() : m_color.blue()));
    }
    m_wheel->setColor(m_color);
    m_wheel->setHue(m_hue); // Keep a chosen hue when editing a neutral/black color.
    if (m_wheelAlpha) m_wheelAlpha->setValue(m_color.alpha());
    updateGradients();
    m_syncing = false;
}

void ColorPicker::updateGradients()
{
    for (int i = 0; i < (m_withAlpha ? 4 : 3); ++i) {
        QGradientStops stops;
        if (i == 3) {
            QColor transparent = m_color, opaque = m_color;
            transparent.setAlpha(0);
            opaque.setAlpha(255);
            stops = channelGradient(transparent, opaque);
        } else if (m_view == HSV) {
            const int h = m_hue, s = m_color.hsvSaturation(), v = m_color.value();
            if (i == 0) stops = hueGradient();
            else if (i == 1) stops = channelGradient(QColor::fromHsv(h, 0, v), QColor::fromHsv(h, 255, v));
            else stops = channelGradient(Qt::black, QColor::fromHsv(h, s, 255));
        } else {
            QColor lo = m_color, hi = m_color;
            int low[3] = {lo.red(), lo.green(), lo.blue()};
            int high[3] = {hi.red(), hi.green(), hi.blue()};
            low[i] = 0;
            high[i] = 255;
            lo.setRgb(low[0], low[1], low[2]);
            hi.setRgb(high[0], high[1], high[2]);
            stops = channelGradient(lo, hi);
        }
        m_fields[i]->setGradient(stops);
    }
    if (m_wheelAlpha) {
        QColor lo = m_color, hi = m_color;
        lo.setAlpha(0);
        hi.setAlpha(255);
        m_wheelAlpha->setGradient(channelGradient(lo, hi));
    }
}
