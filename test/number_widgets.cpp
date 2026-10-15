#include "Widgets.h"

#include <QApplication>
#include <QFormLayout>
#include <QImage>
#include <QLineEdit>
#include <QSignalBlocker>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    theme::applyToApplication(app);
    QWidget window;
    auto *layout = new QFormLayout(&window);
    auto *number = new NumberBox;
    number->setRange(-1000, 1000); number->setValue(123.45);
    layout->addRow("Decimal", number);
    auto *integer = new IntBox;
    integer->setRange(-1000, 1000); integer->setValue(123);
    layout->addRow("Integer", integer);
    auto *slider = new SliderField;
    slider->setRange(0, 100); slider->setValue(75); slider->setSuffix(" %");
    layout->addRow("Slider", slider);
    auto *compact = new SliderField;
    compact->setRange(0, 100); compact->setValue(25); compact->setCompact(true);
    layout->addRow("Compact slider", compact);
    auto *range = new RangeField;
    range->setRange(0, 120); range->setValues(12, 89); range->setSuffix(" s");
    layout->addRow("Range", range);
    window.resize(500, 200);
    window.show(); app.processEvents();
    int failures = 0;
    for (auto *spin : window.findChildren<QAbstractSpinBox *>()) {
        auto *edit = spin->findChild<QLineEdit *>();
        if (!edit) { ++failures; continue; }
        spin->clearFocus(); edit->deselect();
        const QString text = edit->text();
        const QImage filled = spin->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
        const QSignalBlocker block(spin), editBlock(edit);
        edit->setText(QString());
        const QImage empty = spin->grab().toImage().convertToFormat(QImage::Format_RGBA8888);
        edit->setText(text);
        int changed = 0;
        for (int y = 0; y < filled.height(); ++y)
            for (int x = 0; x < filled.width(); ++x)
                if (filled.pixel(x, y) != empty.pixel(x, y)) ++changed;
        const QRect rect = edit->geometry();
        std::printf("number '%s': spin %dx%d, editor (%d,%d %dx%d), text pixels %d\n",
                    text.toUtf8().constData(), spin->width(), spin->height(),
                    rect.x(), rect.y(), rect.width(), rect.height(), changed);
        if (text.isEmpty() || changed < 40 || edit->width() < spin->fontMetrics().horizontalAdvance(text)) ++failures;
    }
    if (argc > 1) window.grab().save(QString::fromLocal8Bit(argv[1]));
    return failures ? 1 : 0;
}
