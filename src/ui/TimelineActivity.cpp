#include "TimelineActivity.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <cmath>

namespace {
constexpr int header = 48, rowHeight = 34;
QColor ink(Engine::AnimState state) {
    return state == Engine::AnimState::Playing ? QColor("#6fe3bc") : state == Engine::AnimState::Paused ? QColor("#efc374") : QColor("#8c91a0");
}
QString timeText(double t) {
    if (!std::isfinite(t)) return QStringLiteral("∞");
    t = std::max(0.,t);
    return QStringLiteral("%1:%2").arg(int(t)/60).arg(std::fmod(t,60.),4,'f',1,QLatin1Char('0'));
}
}

TimelineActivity::TimelineActivity(Engine *engine, QWidget *parent) : QAbstractScrollArea(parent), m_engine(engine) {
    setObjectName(QStringLiteral("timelineActivity"));
    setAccessibleName(QStringLiteral("Live timelines"));
    setMinimumHeight(118); setMaximumHeight(230);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setMouseTracking(true);
    m_timer.setInterval(80);
    connect(&m_timer, &QTimer::timeout, this, [this] { if (isVisible()) poll(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    m_timer.start();
    poll();
}
void TimelineActivity::setShowStopped(bool show) { m_showStopped=show; poll(); }
void TimelineActivity::showEvent(QShowEvent *e) { QAbstractScrollArea::showEvent(e); poll(); }
void TimelineActivity::resizeEvent(QResizeEvent *e) { QAbstractScrollArea::resizeEvent(e); updateRange(); }
void TimelineActivity::updateRange() {
    verticalScrollBar()->setPageStep(std::max(1,viewport()->height()-header));
    verticalScrollBar()->setSingleStep(rowHeight);
    verticalScrollBar()->setRange(0,std::max(0,int(m_rows.size())*rowHeight - (viewport()->height()-header)));
}
void TimelineActivity::poll() {
    std::vector<Engine::AnimationPlayback> sample;
    if (!m_engine->sampleAnimations(&sample)) return; // transport display must never wait for a scene load
    m_rows.clear(); m_playing=m_paused=0; m_total=int(sample.size());
    for (auto &a : sample) {
        m_playing += a.state==Engine::AnimState::Playing;
        m_paused += a.state==Engine::AnimState::Paused;
        if (m_showStopped || a.state!=Engine::AnimState::Stopped) m_rows.push_back(std::move(a));
    }
    updateRange();
    setAccessibleDescription(describe());
    viewport()->update();
}
QString TimelineActivity::describe() const {
    QStringList text;
    text << QStringLiteral("%1 playing · %2 paused · %3 timelines").arg(m_playing).arg(m_paused).arg(m_total);
    for (const auto &a : m_rows) text << QStringLiteral("%1 · %2 · %3 / %4 · pass %5").arg(a.name, a.state==Engine::AnimState::Playing?"playing":a.state==Engine::AnimState::Paused?"paused":"stopped",timeText(a.position),timeText(a.duration)).arg(a.pass);
    return text.join('\n');
}
void TimelineActivity::paintEvent(QPaintEvent *) {
    QPainter p(viewport()); p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(viewport()->rect(),QColor("#171c24"));
    const int w=viewport()->width();
    p.setPen(QColor("#dce4f0"));
    auto title=font(); title.setBold(true); p.setFont(title);
    p.drawText(QRect(10,3,w-85,20),Qt::AlignVCenter,QStringLiteral("LIVE   %1 playing   ·   %2 paused").arg(m_playing).arg(m_paused));
    p.setFont(font()); p.setPen(QColor("#a1b3ca"));
    p.drawText(QRect(w-82,3,72,20),Qt::AlignRight|Qt::AlignVCenter,m_showStopped?QStringLiteral("ALL ▾"):QStringLiteral("ACTIVE ▾"));
    if (!m_rows.empty()) {
        const double cell=double(w-20)/m_rows.size();
        for (size_t i=0;i<m_rows.size();++i) {
            const auto &a=m_rows[i]; const QRectF tile(10+i*cell,28,std::max(1.,cell-3),12);
            p.fillRect(tile,QColor("#2b3442"));
            p.fillRect(QRectF(tile.left(),tile.top(),tile.width()*std::clamp(a.position/std::max(.001,a.duration),0.,1.),tile.height()),ink(a.state));
            if (a.id==m_selected) { p.setPen(QColor("#ffffff")); p.drawRect(tile); }
        }
    }
    p.save(); p.setClipRect(QRect(0,header,w,viewport()->height()-header));
    if (m_rows.empty()) { p.setPen(QColor("#8e9bae")); p.drawText(QRect(10,header,w-20,48),Qt::AlignVCenter|Qt::TextWordWrap,QStringLiteral("No active timeline. Start one from a cue or the timeline editor. Choose ALL to inspect stopped timelines.")); }
    for (int i=0;i<int(m_rows.size());++i) {
        const auto &a=m_rows[size_t(i)]; const int y=header+i*rowHeight-verticalScrollBar()->value();
        if (y+rowHeight<header || y>viewport()->height()) continue;
        if (a.id==m_selected) p.fillRect(QRect(0,y,w,rowHeight),QColor("#283647"));
        const QColor color=ink(a.state); p.setPen(color);
        p.drawText(QRect(10,y+1,20,18),Qt::AlignVCenter,QString::number(i+1));
        p.setPen(QColor("#e0e7f0"));
        p.drawText(QRect(34,y+1,std::max(10,w-260),18),Qt::AlignVCenter,p.fontMetrics().elidedText(a.name,Qt::ElideRight,std::max(10,w-260)));
        const auto state=a.state==Engine::AnimState::Playing?QStringLiteral("PLAY"):a.state==Engine::AnimState::Paused?QStringLiteral("PAUSE"):QStringLiteral("STOP");
        const QString loop=a.loop==Engine::AnimLoop::Once?QStringLiteral("ONCE"):QStringLiteral("↻ %1").arg(a.pass);
        p.setPen(color); p.drawText(QRect(std::max(50,w-224),y+1,160,18),Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("%1  %2  %3 / %4").arg(state,loop,timeText(a.position),timeText(a.duration)));
        p.drawText(QRect(w-57,y,25,22),Qt::AlignCenter,a.state==Engine::AnimState::Playing?QStringLiteral("Ⅱ"):QStringLiteral("▶"));
        p.drawText(QRect(w-29,y,24,22),Qt::AlignCenter,QStringLiteral("■"));
        p.fillRect(QRect(34,y+24,std::max(1,w-98),3),QColor("#303a48"));
        p.fillRect(QRectF(34,y+24,std::max(1,w-98)*std::clamp(a.position/std::max(.001,a.duration),0.,1.),3),color);
    }
    p.restore();
}
int TimelineActivity::rowAt(const QPoint &point) const { return point.y()<header?-1:(point.y()-header+verticalScrollBar()->value())/rowHeight; }
void TimelineActivity::select(int row) {
    if (row<0 || row>=int(m_rows.size())) return;
    m_selected=m_rows[size_t(row)].id;
    const int top=row*rowHeight, value=verticalScrollBar()->value(), page=verticalScrollBar()->pageStep();
    if (top<value) verticalScrollBar()->setValue(top);
    else if (top+rowHeight>value+page) verticalScrollBar()->setValue(top+rowHeight-page);
    viewport()->update();
}
void TimelineActivity::mousePressEvent(QMouseEvent *event) {
    if (event->button()!=Qt::LeftButton) return;
    const auto point=event->pos();
    if (point.y()<25 && point.x()>viewport()->width()-85) { setShowStopped(!m_showStopped); return; }
    if (point.y()>=26 && point.y()<header && !m_rows.empty()) {
        select(std::clamp(int(double(point.x()-10)/std::max(1,viewport()->width()-20)*m_rows.size()),0,int(m_rows.size())-1)); return;
    }
    const int row=rowAt(point); if (row<0 || row>=int(m_rows.size())) return;
    select(row); const auto a=m_rows[size_t(row)];
    if (point.x()>viewport()->width()-30) m_engine->controlAnimation(a.id,Engine::AnimAction::Stop);
    else if (point.x()>viewport()->width()-60) m_engine->controlAnimation(a.id,a.state==Engine::AnimState::Playing?Engine::AnimAction::Pause:Engine::AnimAction::Play);
    poll();
}
void TimelineActivity::mouseDoubleClickEvent(QMouseEvent *event) {
    const int row=rowAt(event->pos());
    if (event->button()==Qt::LeftButton && event->pos().x()<viewport()->width()-60 && row>=0 && row<int(m_rows.size())) emit editRequested(m_rows[size_t(row)].id);
}
void TimelineActivity::keyPressEvent(QKeyEvent *event) {
    int selected=-1; for (int i=0;i<int(m_rows.size());++i) if (m_rows[size_t(i)].id==m_selected) selected=i;
    if (event->key()==Qt::Key_Up || event->key()==Qt::Key_Down) select(std::clamp(selected+(event->key()==Qt::Key_Down?1:-1),0,std::max(0,int(m_rows.size())-1)));
    else if (selected>=0 && event->key()==Qt::Key_Space) { const auto a=m_rows[size_t(selected)]; m_engine->controlAnimation(a.id,a.state==Engine::AnimState::Playing?Engine::AnimAction::Pause:Engine::AnimAction::Play); poll(); }
    else if (selected>=0 && (event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter)) emit editRequested(m_selected);
    else QAbstractScrollArea::keyPressEvent(event);
}
