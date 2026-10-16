#include "AudioSpectrum.h"
#include "AudioOutput.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <algorithm>
#include <cmath>
AudioSpectrum::AudioSpectrum(AudioOutput *audio,QWidget *parent):QWidget(parent),m_audio(audio){
    setObjectName("audioSpectrum");setMinimumHeight(145);setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("Audio spectrum frequency selection");
    setToolTip("Drag either frequency boundary, or click to move the nearest one. Arrow keys adjust the last boundary; Shift makes a fine adjustment.");
    auto *timer=new QTimer(this);timer->setInterval(40);
    connect(timer,&QTimer::timeout,this,[this]{
        if(!isVisible())return;
        auto next=m_audio->spectrum();
        if(next.rate==m_spectrum.rate && next.power==m_spectrum.power)return;
        m_spectrum=std::move(next);
        update();
    });
    timer->start();
}
QRectF AudioSpectrum::plot()const{return QRectF(10,22,std::max(1,width()-20),std::max(1,height()-47));}
double AudioSpectrum::position(double hz)const{return plot().left()+std::log(std::clamp(hz,20.,20000.)/20.)/std::log(1000.)*plot().width();}
double AudioSpectrum::frequency(double x)const{return 20.*std::pow(1000.,std::clamp((x-plot().left())/plot().width(),0.,1.));}
void AudioSpectrum::setBand(double low,double high){m_low=std::clamp(low,20.,19999.);m_high=std::clamp(high,m_low+1.,20000.);update();}
void AudioSpectrum::paintEvent(QPaintEvent *){
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const QRectF r=plot();
    p.fillRect(rect(),QColor(19,22,28));p.fillRect(QRectF(position(m_low),r.top(),position(m_high)-position(m_low),r.height()),QColor(28,71,83));
    p.setPen(QColor(65,71,83));
    for(double f:{20.,100.,1000.,10000.,20000.}){const double x=position(f);p.drawLine(QPointF(x,r.top()),QPointF(x,r.bottom()));p.setPen(QColor(155,166,180));p.drawText(QRectF(x-23,r.bottom()+4,46,18),Qt::AlignHCenter,f>=1000?QString::number(f/1000)+"k":QString::number(f));p.setPen(QColor(65,71,83));}
    QPainterPath path;path.moveTo(r.left(),r.bottom());
    // Maximum dB per displayed log-frequency slice keeps narrow peaks visible.
    for(int x=int(r.left());x<=int(r.right());++x){
        const double lo=frequency(x-.5),hi=frequency(x+.5),step=double(m_spectrum.rate)/AudioAnalysis::fftSize;
        float peak=0;const int a=std::clamp(int(lo/step),1,AudioAnalysis::binCount-1),b=std::clamp(int(std::ceil(hi/step)),a,AudioAnalysis::binCount-1);
        for(int k=a;k<=b;++k)peak=std::max(peak,m_spectrum.power[size_t(k)]);
        const double db=10*std::log10(std::max(1e-10f,peak));path.lineTo(x,r.bottom()-std::clamp((db+80)/80.,0.,1.)*r.height());
    }
    path.lineTo(r.right(),r.bottom());p.fillPath(path,QColor(86,207,219,100));p.setPen(QColor(115,231,235));p.drawPath(path);
    p.setPen(QPen(QColor(126,239,224),2));for(double f:{m_low,m_high})p.drawLine(QPointF(position(f),r.top()),QPointF(position(f),r.bottom()));
    p.setPen(QColor(205,231,232));p.drawText(QRectF(10,1,width()-20,19),Qt::AlignLeft,QStringLiteral("%1 – %2 Hz").arg(m_low,0,'f',0).arg(m_high,0,'f',0));
    if(width()>460)p.drawText(QRectF(10,1,width()-20,19),Qt::AlignRight,QStringLiteral("Program mix · −80…0 dB"));
}
void AudioSpectrum::drag(double x){const double f=frequency(x);if(m_handle==1)m_low=std::clamp(f,20.,m_high-1.);else m_high=std::clamp(f,m_low+1.,20000.);update();}
void AudioSpectrum::mousePressEvent(QMouseEvent *e){if(e->button()!=Qt::LeftButton)return;setFocus();m_handle=std::abs(e->position().x()-position(m_low))<std::abs(e->position().x()-position(m_high))?1:2;drag(e->position().x());}
void AudioSpectrum::mouseMoveEvent(QMouseEvent *e){if(e->buttons()&Qt::LeftButton)drag(e->position().x());}
void AudioSpectrum::mouseReleaseEvent(QMouseEvent *e){if(e->button()==Qt::LeftButton&&m_handle){drag(e->position().x());emit bandEdited(m_low,m_high);}}
void AudioSpectrum::keyPressEvent(QKeyEvent *e){if(e->key()!=Qt::Key_Left&&e->key()!=Qt::Key_Right){QWidget::keyPressEvent(e);return;}if(!m_handle)m_handle=1;double f=m_handle==1?m_low:m_high;f*=std::pow(2.,(e->key()==Qt::Key_Right?1.:-1.)*(e->modifiers()&Qt::ShiftModifier?.01:.08));drag(position(f));emit bandEdited(m_low,m_high);}
