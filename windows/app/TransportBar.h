#pragma once

#include <QWidget>

class MotionDocument;
class QLabel;
class QToolButton;
class ValueSlider;

// Play, scrub and step (TransportBar.swift). Space plays and pauses, the
// arrow keys step a frame.
class TransportBar : public QWidget
{
    Q_OBJECT
public:
    explicit TransportBar(MotionDocument &document, QWidget *parent = nullptr);

private:
    void refresh();
    // minutes, seconds and frames: what an editor shows
    QString timecode(double seconds) const;

    MotionDocument &m_doc;
    QToolButton *m_back, *m_play, *m_forward;
    QLabel *m_time, *m_duration;
    ValueSlider *m_scrub;
    bool m_showsPause = false;
};
