#pragma once
// Reusable Qt-only color control: RGB/HSL/HSV channels, wheel and swatch palette.
// Does not depend on Engine, layers, undo history or parameter metadata.

#include <QColor>
#include <QString>
#include <QWidget>
#include <array>

class QLabel;
class QToolButton;
class SliderField;
class HueWheelWidget;

class ColorPickerPreferences : public QObject
{
    Q_OBJECT
signals:
    void defaultViewChanged();
};

class ColorPicker : public QWidget
{
    Q_OBJECT
public:
    enum View { RGB = 0, HSV = 1, Wheel = 2, Palette = 3, HSL = 4 };
    explicit ColorPicker(bool withAlpha = true, QWidget *parent = nullptr);

    bool hasAlpha() const { return m_withAlpha; }
    QColor color() const { return m_color; }
    View view() const { return m_view; }

    void setColor(const QColor &color); // programmatic updates never emit
    void setView(View view);
    static View defaultView(); // application preference; HSL when unset
    static void setDefaultView(View view);
    // Only an explicit view choice is saved; opening a picker does not create an override.
    void setSettingsKey(const QString &key);
    // Its own swatch, hidden where the host already shows the color (ColorEditor: next to the hexadecimal field)
    void setSwatchVisible(bool on);
    static QColor getColor(const QColor &initial, QWidget *parent, const QString &title,
                           const QString &settingsKey, bool withAlpha = true);

signals:
    void colorEdited(const QColor &color);

private:
    void editColor(const QColor &color);
    void editChannel(int channel, double value);
    void applyView(View view);
    void updateControls();
    void updateGradients();

    bool m_withAlpha;
    bool m_syncing = false;
    int m_hue = 0; // preserved when saturation becomes zero
    double m_hslSaturation = 0; // HSL saturation is undefined in RGB black/white
    QColor m_color = Qt::white;
    View m_view = HSL;
    bool m_hasLocalView = false;
    QString m_settingsKey;
    QLabel *m_swatch = nullptr;
    std::array<QToolButton *, 5> m_buttons{};
    std::array<QLabel *, 4> m_labels{};
    std::array<SliderField *, 4> m_fields{};
    SliderField *m_wheelAlpha = nullptr;
    HueWheelWidget *m_wheel = nullptr;
    QWidget *m_channelPage = nullptr;
    QWidget *m_wheelPage = nullptr;
    QWidget *m_palettePage = nullptr;
};
