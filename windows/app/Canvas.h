#pragma once

#include "FrameGeometry.h"

#include <QImage>
#include <QPixmap>
#include <QWidget>

#include <optional>

class Document;
class QLabel;
class QPushButton;

// What surrounds the picture (CanvasBackground). Brightness and colour are
// judged against their surround: the same print reads dark against white
// and washed against black. A viewing preference, never in the recipe.
enum class Surround { Dark, MiddleGrey, White };

// The picture, fitted into the window on its surround, with a contact shadow;
// the placeholder before a file is open; the status and any error.
class Canvas : public QWidget
{
    Q_OBJECT
public:
    explicit Canvas(Document &document, QWidget *parent = nullptr);
    void setSurround(Surround surround);
    Surround surround() const { return m_surround; }

signals:
    void openRequested();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QRect pictureRect() const;
    void layoutPlaceholder();
    void layoutCropBar();

    // MARK: the crop, drawn on the whole frame while cropping (CropOverlay)
    enum class Part { None, Corner, Edge, Move, Level };
    struct Hit {
        Part part = Part::None;
        int x = 0, y = 0;           // -1 for the left or top side, 1 for the right or bottom
    };
    PointD frame() const;           // the turned frame, in the preview's pixels
    double scale() const;           // screen pixels per frame pixel
    bool frameShowing() const;      // the frame on screen is the one the crop is drawn on
    QRectF view(const RectD &r) const;
    Hit hit(QPointF at, const QRectF &rect) const;
    void paintCrop(QPainter &p);
    RectD dragged(const Hit &h, const RectD &start, double dx, double dy) const;
    RectD furthest(const RectD &from, const RectD &to) const;
    std::optional<RectD> place(PointD size, double fixed, bool fixedIsX, double near) const;
    // MARK: masks and gradients, drawn and dragged on the print (MaskOverlay)
    QPointF toView(PointD normalised) const;    // a point of the frame on the canvas
    PointD fromView(QPointF v) const;           // and back, kept near the frame
    std::vector<PointD> selectedHandles(std::vector<int> *kinds = nullptr) const;
    void paintMasks(QPainter &p);
    // MARK: the loupe (LoupeView), over the part of the print it shows
    static constexpr double kLoupe = 300;          // its side, in the screen's points
    QRectF loupeRect() const;
    void paintLoupe(QPainter &p);
    std::optional<QPointF> m_loupeGrab;             // where in the loupe a drag took hold of it
    int m_handle = -1;                          // the handle being dragged, -1 none
    std::vector<PointD> m_handleOrigin;         // the selected item's points as the drag began
    QPointF m_handleStart;

    Hit m_drag;
    std::optional<RectD> m_startRect, m_liveRect;
    QPointF m_dragStart, m_dragNow;
    QWidget *m_cropBar;
    QLabel *m_cropHint;
    QPushButton *m_levelButton;

    Document &m_doc;
    Surround m_surround = Surround::Dark;
    QPushButton *m_open;
    // the picture scaled to its place on screen, made once a frame and size
    QPixmap m_scaled;
    qint64 m_scaledKey = 0;
    QSize m_scaledSize;
};
