#pragma once

#include "Canvas.h"

#include <QWidget>

#include <optional>

class MotionDocument;
class QLabel;
class QProgressBar;
class QPushButton;

// The Motion preview (PreviewView, MotionContentView's canvas): the graded
// frame fitted into the window on its surround, turned the way the clip's
// track asks; the placeholder before a clip is open; the status line, which
// shows what is under the pointer, and the export's progress.
class MotionCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit MotionCanvas(MotionDocument &document, QWidget *parent = nullptr);
    void setSurround(Surround surround);

signals:
    void openRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    // Where the frame lands (FramePipeline.Placement): fitted with padding
    // all round, turned by the track's quarter turns, and the size it is
    // graded at, never above the clip's own.
    struct Placement {
        QSize graded;           // device pixels, the clip's way up
        QRectF frame;           // on the canvas, turned
    };
    std::optional<Placement> placement() const;
    void updateViewSize();
    std::optional<QPointF> sourcePixel(QPointF at) const;
    void layoutChildren();

    MotionDocument &m_doc;
    Surround m_surround = Surround::Dark;
    QPushButton *m_open;
    QWidget *m_exportBar;
    QLabel *m_exportText, *m_exportPercent;
    QProgressBar *m_exportProgress;
    QRectF m_errorRect;
};
