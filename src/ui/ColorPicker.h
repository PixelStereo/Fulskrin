#pragma once
// Reusable Qt-only color control: channel sliders, HSV wheel and swatch palette.
// Does not depend on Engine, layers, undo history or parameter metadata.

#include <QColor>
#include <QWidget>
#include <array>

class QLabel;
class QToolButton;
class SliderField;
class HueWheelWidget;

class ColorPicker : public QWidget
{
    Q_OBJECT
public:
    enum View { RGB = 0, HSV = 1, Wheel = 2, Palette = 3 };
    explicit ColorPicker(bool withAlpha = true, QWidget *parent = nullptr);

    bool hasAlpha() const { return m_withAlpha; }
    QColor color() const { return m_color; }
    View view() const { return m_view; }

    void setColor(const QColor &color); // programmatic updates never emit
    void setView(View view);
    // Each instance can remember its own view. Optional legacy key migrates
    // previous RGBA/HSVA/Palette indices (0/1/2) to the new four-view format.
    void setSettingsKey(const QString &key, const QString &legacyKey = QString());

signals:
    void colorEdited(const QColor &color);

private:
    void editColor(const QColor &color);
    void editChannel(int channel, double value);
    void updateControls();
    void updateGradients();

    bool m_withAlpha;
    bool m_syncing = false;
    int m_hue = 0; // preserved when saturation becomes zero
    QColor m_color = Qt::white;
    View m_view = Wheel;
    QString m_settingsKey;
    QLabel *m_swatch = nullptr;
    std::array<QToolButton *, 4> m_buttons{};
    std::array<QLabel *, 4> m_labels{};
    std::array<SliderField *, 4> m_fields{};
    SliderField *m_wheelAlpha = nullptr;
    HueWheelWidget *m_wheel = nullptr;
    QWidget *m_channelPage = nullptr;
    QWidget *m_wheelPage = nullptr;
    QWidget *m_palettePage = nullptr;
};
