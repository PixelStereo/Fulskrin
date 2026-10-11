#pragma once
#include <QPointer>
#include <QVariant>
#include <QJsonObject>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class Engine;
class Mapping;
struct LayerValues;
class QUndoStack;
class QVBoxLayout;
class QSlider;
class QLabel;
class QPushButton;
class QListWidget;
class QProgressBar;
class QDoubleSpinBox;
class SliderField;
class RangeField;
class QButtonGroup;
class QAbstractButton;
class QLineEdit;
class RoiEditor;
class ColorEditor;
class QComboBox;
class QGroupBox;
class ParamPanel;
class ParamAnimPanel;
class AnimateMenu;
class QTabWidget;
class ViewportOutputPanel;

// Inspector for the selected layer, group or viewport: at the top, always shown, its source (drop zone, transport,
// sound, generator parameters) and its compositing (opacity, blend, the viewports it appears in); below, in sub-tabs:
// ROI, Color (added / removed), Spatial (mapping), FX (ISF chain), Anim (the animations of its parameters), and for a
// viewport, Output (size, screen, publishing).
// All edits go through the undo stack. A locked layer shows its settings without allowing edits
// (the transport stays available). A click on a parameter's name resets it; a right-click animates it.
class MaskPanel;

class LayerInspector : public QWidget
{
    Q_OBJECT
public:
    LayerInspector(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);

    void setLayer(int index);
    int layerIndex() const { return m_layer; }
    void rebuild();
    void refreshDynamic(); // playback position (called periodically)
    void refreshMasks();   // the masks changed elsewhere (the preview, undo)
    void selectMask(int index); // a mask picked or drawn in the preview
    int maskIndex() const { return m_maskIndex; }

signals:
    void layerChanged();   // name, visibility, source: the list must be refreshed
    void mappingChanged(); // the mapping view must be redrawn
    void fileDropped(const QString &path); // media or ISF dropped on the Source tab: load it into the layer
    void setInOutRequested(bool in);       // in / out point at the current position
    void rescanLibraryRequested();         // re-read the ISF folders (shaders added in the Finder)
    void projectEdited();                  // saved interface state of the layer changed (not undoable)
    void kindChanged(const QString &kind); // "Layer", "Group" or "Viewport": the title of the tab
    void maskSelected(int index);          // the mask the preview edits (-1: none)
    void maskDrawRequested();              // the preview draws a new mask point by point

private:
    QWidget *buildSource(const LayerValues &s);    // above the tabs: the drop zone, the transition, the speed
    QWidget *buildSourceTab(const LayerValues &s); // the Source tab
    QWidget *buildViewports(const LayerValues &s); // the Viewports tab: how much of it each one shows
    void useLayer(int row, const std::vector<std::pair<quint64, QString>> &candidates); // a layer dropped on the source
    QWidget *buildRoi(const LayerValues &s);
    QWidget *buildColor(const LayerValues &s);
    QWidget *buildCompositing(const LayerValues &s);
    QWidget *buildMapping(const LayerValues &s);
    QPointer<MaskPanel> m_maskPanel;
    unsigned m_masksRev = 0; // the masks' revision the panel shows
    int m_maskIndex = -1; // the mask selected (kept while the inspector is rebuilt for the same layer)
    QWidget *buildEffects(const LayerValues &s);
    void showEffectParams(); // parameters of the selected effect, swapped without rebuilding the list
    void editMapping(const QString &text, const std::function<void(Mapping &)> &fn, bool merge = false);
    void refreshSpatial(); // position / scale fields follow the mapping (handles dragged in the preview)
    void editEffects(const QString &text, const std::function<void()> &op);
    void editSource(const QString &text, const std::function<void()> &op);
    void setProp(int prop, const QVariant &value); // cmd::SetLayerProp::Prop
    // Values by address (Parameter.h): one undo step; nothing on a locked layer
    void setValues(const QJsonObject &values, const QString &text);
    void setValue(const QString &path, const QJsonValue &v, const QString &text) { setValues({{path, v}}, text); }
    void setRoi(const QRectF &r);
    void showAnimation(const QString &param); // the Anim tab, that number's card unfolded
    // The limits of the layer's number at `path`, as declared (Parameter.h); `lo`, `hi` if it has none
    std::pair<double, double> limitsOf(const QString &path, double lo, double hi) const;

    Engine *m_engine;
    QUndoStack *m_undo;
    int m_layer = -1;
    int m_selectedEffect = 0;
    QString m_subTab = QStringLiteral("Source"); // the tab shown, by its name: kept from one layer to the next
    quint64 m_layerId = 0;
    QPointer<SliderField> m_fxSpeed; // the selected FX's speed
    QString m_fxSpeedPath;
    bool m_locked = false;
    QVBoxLayout *m_layout = nullptr;
    QWidget *m_content = nullptr;

    QPointer<QWidget> m_fxDetail;
    QPointer<SliderField> m_position, m_speed;
    QPointer<RangeField> m_loop;
    QPointer<QButtonGroup> m_playButtons;
    QPointer<RoiEditor> m_roi;
    QPointer<ColorEditor> m_colorAdd, m_colorRemove;
    QPointer<SliderField> m_temp, m_tint;
    // Followed while a snapshot fades them (as temp / tint)
    QPointer<SliderField> m_opacity, m_volume;
    QPointer<ParamPanel> m_generatorParams, m_effectParams;
    struct RouteField {
        quint64 viewport;
        QPointer<SliderField> field;
        std::shared_ptr<float> current; // the value an edit starts from (undo)
    };
    std::vector<RouteField> m_routeFields;
    bool m_previewing = false;
    QPointer<QProgressBar> m_meter;
    QPointer<QLabel> m_pictureFact; // known once frames are decoded: kept up to date
    QPointer<QLabel> m_timecodeStatus; // a Timecode generator: where its timecode comes from, kept up to date
    struct TcTransport { // a free run's transport, kept up to date
        QPointer<QAbstractButton> play, pause;
        QPointer<QLineEdit> position, in, out;
        QPointer<QButtonGroup> modes;
        std::function<QString(double)> shown; // seconds of transport as the timecode shown
    };
    std::shared_ptr<TcTransport> m_tcTransport;
    void refreshTimecodeTransport();
    QPointer<SliderField> m_genSpeed; // the generator's speed (above the tabs)
    QPointer<ViewportOutputPanel> m_output;
    QPointer<QTabWidget> m_tabs;
    QPointer<AnimateMenu> m_animate; // right-click on a number: Animate
    QPointer<ParamAnimPanel> m_anims; // the Anim tab
    int m_animTab = -1;
    QStringList m_animated; // the numbers animated when the tabs were built (a ∿ by their names)
    QString m_revealAnim;   // the card to show once rebuilt
    QPointer<QDoubleSpinBox> m_posX, m_posY, m_scaleX, m_scaleY, m_rotation, m_pivotX, m_pivotY;
    bool m_scaleLinked = true;
    bool m_sizePx = false; // a viewport: width and height in composition pixels (a layer: scale in %)
};
