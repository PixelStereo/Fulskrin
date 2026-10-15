#pragma once
#include "AudioAnalysis.h"
#include <QWidget>
class AudioOutput;
class AudioSpectrum : public QWidget {
    Q_OBJECT
public:
    explicit AudioSpectrum(AudioOutput *audio, QWidget *parent=nullptr);
    void setBand(double low,double high);
    QSize minimumSizeHint() const override {return {220,135};}
signals:
    void bandEdited(double low,double high);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    QRectF plot() const;
    double frequency(double x) const;
    double position(double hz) const;
    void drag(double x);
    AudioOutput *m_audio;
    AudioAnalysis::Spectrum m_spectrum;
    double m_low=20,m_high=20000;
    int m_handle=0;
};
