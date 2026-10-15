#include "ColorPicker.h"
#include "Widgets.h"

#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    app.setOrganizationName("PinolTests");
    app.setApplicationName("ColorPicker");
    theme::applyToApplication(app);
    int failures = 0;
    auto check = [&](bool ok, const char *reason) {
        if (!ok) { ++failures; std::printf("FAIL color_picker_ui: %s\n", reason); }
    };
    auto near = [](double a, double b) { return std::abs(a - b) < .015; };
    check(ColorPicker::defaultView() == ColorPicker::HSL, "fresh settings default to HSL");
    QSettings().setValue("ui/colorPickerMode/first", int(ColorPicker::Wheel));
    ColorPicker first, second, following;
    first.setSettingsKey("ui/colorPickerMode/first");
    second.setSettingsKey("ui/colorPickerMode/second");
    following.setSettingsKey("ui/colorPickerMode/following");
    check(first.view() == ColorPicker::HSL && second.view() == ColorPicker::HSL,
          "every unchosen location follows the global preference, ignoring old auto-written Wheel defaults");
    check(!QSettings().contains("ui/colorPickerMode/first/selectedView"), "opening does not create a local override");
    QSettings().setValue("ui/colorPickerMode/oldExplicit", int(ColorPicker::Palette));
    ColorPicker oldExplicit;
    oldExplicit.setSettingsKey("ui/colorPickerMode/oldExplicit");
    check(oldExplicit.view() == ColorPicker::Palette, "an old explicit non-default choice is preserved");
    first.setView(ColorPicker::RGB);
    second.setView(ColorPicker::Palette);
    ColorPicker::setDefaultView(ColorPicker::HSV);
    check(first.view() == ColorPicker::RGB && second.view() == ColorPicker::Palette,
          "global changes preserve independent explicit choices");
    check(following.view() == ColorPicker::HSV, "already open unchosen pickers follow global changes");
    ColorPicker restoredFirst, restoredSecond;
    restoredFirst.setSettingsKey("ui/colorPickerMode/first");
    restoredSecond.setSettingsKey("ui/colorPickerMode/second");
    check(restoredFirst.view() == ColorPicker::RGB && restoredSecond.view() == ColorPicker::Palette,
          "recreating an inspector restores each location separately");
    QSettings().setValue("ui/colorPickerMode/invalid/selectedView", 999);
    ColorPicker invalid;
    invalid.setSettingsKey("ui/colorPickerMode/invalid");
    check(invalid.view() == ColorPicker::HSV, "invalid local preferences fall back to the global default");
    ColorPicker::setDefaultView(ColorPicker::HSL);
    following.setColor(QColor::fromHslF(1.0 / 3.0, 1, .5, .4));
    int edits = 0;
    QObject::connect(&following, &ColorPicker::colorEdited, &following, [&](const QColor &) { ++edits; });
    const QColor original = following.color();
    for (ColorPicker::View view : {ColorPicker::RGB, ColorPicker::HSV, ColorPicker::Wheel, ColorPicker::Palette, ColorPicker::HSL})
        following.setView(view);
    check(edits == 0 && following.color() == original, "view changes preserve exact RGBA without emitting edits");
    auto fields = following.findChildren<SliderField *>();
    check(fields.size() >= 4 && near(fields[0]->value(), 120) && near(fields[1]->value(), 100) && near(fields[2]->value(), 50),
          "HSL displays hue, saturation percent and lightness percent");
    auto edit = [&](int channel, double value) {
        auto *spin = fields[channel]->findChild<QDoubleSpinBox *>();
        check(spin != nullptr, "HSL channel has an editable numeric value");
        if (spin) spin->setValue(value);
    };
    edit(1, 25);
    check(near(following.color().hslSaturationF(), .25) && near(following.color().lightnessF(), .5)
              && near(following.color().alphaF(), .4) && edits == 1,
          "editing HSL updates color once and preserves alpha");
    following.setColor(Qt::white);
    edit(0, 240); edit(1, 100); edit(2, 50);
    check(following.color().blue() > 250 && following.color().red() < 3 && following.color().green() < 3,
          "HSL remembers hue and saturation chosen on white when lightness is lowered");
    ColorPicker opaque(false);
    opaque.setColor(QColor(12, 34, 56, 40));
    check(opaque.color().alpha() == 255, "non-alpha picker stays opaque");

    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        check(dialog != nullptr, "shared color dialog opens");
        if (!dialog) return;
        auto *picker = dialog->findChild<ColorPicker *>();
        check(picker && picker->view() == ColorPicker::HSL, "dialog uses the same global default");
        if (picker) picker->setView(ColorPicker::Wheel);
        dialog->reject();
    });
    check(!ColorPicker::getColor(Qt::red, nullptr, "Cancel test", "ui/colorPickerMode/dialog").isValid(),
          "Cancel leaves the original color unchanged");
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *picker = dialog->findChild<ColorPicker *>();
        check(picker && picker->view() == ColorPicker::Wheel, "dialog location remembers its view after reopening");
        if (picker) picker->setColor(Qt::blue);
        dialog->accept();
    });
    check(ColorPicker::getColor(Qt::red, nullptr, "Accept test", "ui/colorPickerMode/dialog") == QColor(Qt::blue),
          "Accept returns the selected color");
    if (argc > 1) {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *picker = new ColorPicker;
        picker->setColor(QColor(70, 155, 210, 190));
        layout->addWidget(picker);
        window.resize(380, window.sizeHint().height());
        window.show(); app.processEvents();
        check(window.grab().save(QString::fromLocal8Bit(argv[1])), "save HSL capture");
    }
    return failures ? 1 : 0;
}
