#include "Icons.h"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPixmap>

#include <algorithm>

namespace icons {

namespace {

constexpr qreal kGrid = 24;     // the drawing's height, in its units
constexpr qreal kLine = 1.6;    // its line
constexpr qreal kGap = 1.2;     // between a shape and one drawn behind it

QPainterPath rounded(qreal x, qreal y, qreal w, qreal h, qreal r)
{
    QPainterPath p;
    p.addRoundedRect(QRectF(x, y, w, h), r, r);
    return p;
}

QPainterPath circle(qreal cx, qreal cy, qreal r)
{
    QPainterPath p;
    p.addEllipse(QPointF(cx, cy), r, r);
    return p;
}

// a line's own outline, as a shape to fill or to cut away
QPainterPath outline(const QPainterPath &path, qreal width)
{
    QPainterPathStroker s;
    s.setWidth(width);
    s.setCapStyle(Qt::RoundCap);
    s.setJoinStyle(Qt::RoundJoin);
    return s.createStroke(path);
}

qreal gridWidth(Icon which)
{
    return which == Icon::Presets ? 32 : kGrid;
}

// One icon on its grid, the painter's units being the grid's.
void draw(QPainter &p, Icon which, const QColor &ink)
{
    const QPen pen(ink, kLine, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QPen thin = pen;
    thin.setWidthF(1.4);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto fill = [&](const QPainterPath &shape, qreal alpha = 1) {
        QColor c = ink;
        c.setAlphaF(c.alphaF() * alpha);
        p.fillPath(shape, c);
    };
    // `back`'s line behind `front`: left out where `front` covers it, with a gap
    auto behind = [&](const QPainterPath &back, const QPainterPath &front) {
        const QPainterPath cover = front.united(outline(front, kLine + 2 * kGap));
        fill(outline(back, kLine).subtracted(cover));
    };

    switch (which) {
    case Icon::Sidebar: {
        // a window with its left pane filled
        const QPainterPath frame = rounded(3, 5, 18, 14, 2.4);
        QPainterPath pane;
        pane.addRect(QRectF(3, 5, 6.4, 14));
        fill(frame.intersected(pane));
        p.drawPath(frame);
        break;
    }
    case Icon::Folder: {
        // a folder, open: its back with the tab, its front tipped forward
        QPainterPath back;
        back.moveTo(3.2, 18.6);
        back.lineTo(3.2, 7);
        back.quadTo(3.2, 5.4, 4.8, 5.4);
        back.lineTo(8.6, 5.4);
        back.lineTo(10.4, 7.2);
        back.lineTo(17.2, 7.2);
        back.quadTo(18.8, 7.2, 18.8, 8.8);
        back.lineTo(18.8, 10.6);
        p.drawPath(back);
        QPainterPath front;
        front.moveTo(3.2, 18.6);
        front.lineTo(6.1, 11.4);
        front.quadTo(6.4, 10.6, 7.3, 10.6);
        front.lineTo(20.6, 10.6);
        front.quadTo(21.7, 10.6, 21.3, 11.6);
        front.lineTo(18.6, 18);
        front.quadTo(18.3, 18.6, 17.6, 18.6);
        front.closeSubpath();
        p.drawPath(front);
        break;
    }
    case Icon::SurroundDark:
        // as wide as an outlined circle's line reaches
        fill(circle(12, 12, 7 + kLine / 2));
        break;
    case Icon::SurroundGrey: {
        QPainterPath half;
        half.moveTo(12, 12);
        half.arcTo(QRectF(5, 5, 14, 14), 90, 180);
        half.closeSubpath();
        fill(half);
        p.drawPath(circle(12, 12, 7));
        break;
    }
    case Icon::SurroundWhite:
        p.drawPath(circle(12, 12, 7));
        break;
    case Icon::Stills:
        // a rangefinder: flat-topped, its lens to one side, the viewfinder's
        // window and the shutter release on top
        fill(rounded(15.2, 4.6, 3.6, 3, 0.8));
        p.drawPath(rounded(2.6, 7, 18.8, 12.4, 2.2));
        p.drawPath(circle(13.4, 13.4, 3.5));
        p.setPen(thin);
        p.drawPath(rounded(5.2, 9.3, 3.6, 2.4, 0.7));
        break;
    case Icon::Motion: {
        // a cine camera: two reels on top, the lens in front
        p.drawPath(circle(6.8, 6.9, 2.9));
        p.drawPath(circle(13.2, 6.9, 2.9));
        p.drawPath(rounded(2.6, 11, 13.6, 9, 2));
        QPainterPath lens;
        lens.moveTo(16.2, 13.6);
        lens.lineTo(21.2, 11.6);
        lens.lineTo(21.2, 19.4);
        lens.lineTo(16.2, 17.4);
        p.drawPath(lens);
        break;
    }
    case Icon::Copy: {
        // two sheets, the copy in front
        const QPainterPath front = rounded(9, 8, 11.5, 13, 2);
        behind(rounded(3.5, 3, 11.5, 13, 2), front);
        p.drawPath(front);
        break;
    }
    case Icon::Paste: {
        // a clipboard, its clip a rounded tab
        const QPainterPath clip = rounded(8, 3, 8, 4.2, 2.1);
        behind(rounded(4.5, 4.8, 15, 16.4, 2.2), clip);
        p.drawPath(clip);
        p.setPen(thin);
        p.drawLine(QPointF(8.3, 11.4), QPointF(15.7, 11.4));
        p.drawLine(QPointF(8.3, 14.6), QPointF(15.7, 14.6));
        p.drawLine(QPointF(8.3, 17.8), QPointF(12.6, 17.8));
        break;
    }
    case Icon::Presets: {
        // a 35 mm cassette, its rims wider than its shell and its spool on
        // top, the leader's tongue out: looks kept, to put on again
        fill(rounded(6.4, 2.6, 3.2, 2.6, 0.8));
        p.drawPath(rounded(4.2, 7.2, 8.4, 11.6, 1.6));
        p.drawLine(QPointF(3.2, 5.6), QPointF(13.6, 5.6));
        p.drawLine(QPointF(3.2, 20.4), QPointF(13.6, 20.4));
        QPainterPath leader;
        leader.moveTo(12.6, 9.4);
        leader.lineTo(20.6, 9.4);
        leader.lineTo(20.6, 12.4);
        leader.lineTo(17, 12.4);
        leader.lineTo(15.8, 16.4);
        leader.lineTo(12.6, 16.4);
        p.drawPath(leader);
        // it opens a menu
        p.setPen(thin);
        p.drawPolyline(QPolygonF({QPointF(25.6, 11.2), QPointF(27.8, 13.4), QPointF(30, 11.2)}));
        break;
    }
    case Icon::Crop: {
        QPainterPath c;
        c.moveTo(7, 3);
        c.lineTo(7, 17);
        c.lineTo(21, 17);
        c.moveTo(3, 7);
        c.lineTo(17, 7);
        c.lineTo(17, 21);
        p.drawPath(c);
        break;
    }
    case Icon::Original: {
        // before and after: a frame divided by a slider, the before side shaded
        const QPainterPath frame = rounded(3, 5, 18, 14, 2.4);
        QPainterPath left;
        left.addRect(QRectF(3, 5, 9, 14));
        fill(frame.intersected(left), 0.35);
        p.drawPath(frame);
        p.drawLine(QPointF(12, 2.8), QPointF(12, 21.2));
        fill(circle(12, 12, 2.5));
        break;
    }
    case Icon::Loupe: {
        // a magnifier, enlarging
        p.drawPath(circle(10.3, 10.3, 6.4));
        QPen handle = pen;
        handle.setWidthF(2.4);
        p.setPen(handle);
        p.drawLine(QPointF(15.2, 15.2), QPointF(20.6, 20.6));
        p.setPen(thin);
        p.drawLine(QPointF(7.6, 10.3), QPointF(13, 10.3));
        p.drawLine(QPointF(10.3, 7.6), QPointF(10.3, 13));
        break;
    }
    case Icon::Pick:
    case Icon::Picked: {
        QPainterPath pennant;
        pennant.moveTo(7, 4);
        pennant.lineTo(19.5, 8.6);
        pennant.lineTo(7, 13.2);
        pennant.closeSubpath();
        if (which == Icon::Picked)
            fill(outline(pennant, kLine).united(pennant));
        else
            p.drawPath(pennant);
        QPen pole = pen;
        pole.setWidthF(2);
        p.setPen(pole);
        p.drawLine(QPointF(7, 4), QPointF(7, 21));
        break;
    }
    case Icon::Reject: {
        QPen cross = pen;
        cross.setWidthF(2.4);
        p.setPen(cross);
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
        break;
    }
    case Icon::RotateLeft:
    case Icon::RotateRight: {
        // drawn turning left; turning right is its mirror
        if (which == Icon::RotateRight) {
            p.translate(kGrid, 0);
            p.scale(-1, 1);
        }
        p.drawPath(rounded(8.6, 10.2, 11.4, 10, 1.8));
        QPainterPath arc;
        arc.moveTo(15.6, 7.4);
        arc.cubicTo(13.2, 4.2, 8.2, 3.6, 5.2, 7.2);
        p.drawPath(arc);
        p.drawPolyline(QPolygonF({QPointF(4.6, 3.6), QPointF(5, 7.6), QPointF(8.8, 7.2)}));
        break;
    }
    case Icon::FlipAcross:
    case Icon::FlipUpDown: {
        // drawn mirrored across an upright axis; up and down is it turned
        if (which == Icon::FlipUpDown) {
            p.translate(kGrid, 0);
            p.rotate(90);
        }
        QPainterPath solid;
        solid.moveTo(10, 6);
        solid.lineTo(10, 18.5);
        solid.lineTo(3.5, 18.5);
        solid.closeSubpath();
        fill(outline(solid, kLine).united(solid));
        QPainterPath open;
        open.moveTo(14, 6);
        open.lineTo(14, 18.5);
        open.lineTo(20.5, 18.5);
        open.closeSubpath();
        p.drawPath(open);
        QPen axis = thin;
        axis.setDashPattern({1.2, 1.6});
        p.setPen(axis);
        p.drawLine(QPointF(12, 2.5), QPointF(12, 21.5));
        break;
    }
    case Icon::Level: {
        p.drawPath(rounded(2.6, 8.4, 18.8, 7.2, 3.6));
        fill(circle(12, 12, 1.9));
        p.setPen(thin);
        p.drawLine(QPointF(8.4, 10.2), QPointF(8.4, 13.8));
        p.drawLine(QPointF(15.6, 10.2), QPointF(15.6, 13.8));
        break;
    }
    case Icon::SwapAspect:
        // standing and lying, sharing their lower left corner
        p.drawPath(rounded(3.5, 3.5, 10, 17, 1.6));
        p.drawPath(rounded(3.5, 10.5, 17, 10, 1.6));
        break;
    case Icon::Gradient: {
        const QPainterPath square = rounded(4, 4, 16, 16, 2.6);
        for (int band = 0; band < 3; ++band) {
            QPainterPath strip;
            strip.addRect(QRectF(4, 4 + band * 16.0 / 3, 16, 16.0 / 3));
            fill(square.intersected(strip), 0.75 - band * 0.3);
        }
        p.drawPath(square);
        break;
    }
    case Icon::Oval: {
        QPen dashed = pen;
        dashed.setDashPattern({1.6, 1.4});
        p.setPen(dashed);
        p.drawEllipse(QPointF(12, 12), 8.4, 7);
        break;
    }
    case Icon::Polygon: {
        const QPolygonF corners({QPointF(5, 8.5), QPointF(13.5, 4.5), QPointF(20, 10.5), QPointF(16.5, 19.5),
                                 QPointF(6.5, 17.5)});
        p.drawPolygon(corners);
        QPainterPath dots;
        for (const QPointF &c : corners)
            dots.addEllipse(c, 1.7, 1.7);
        fill(dots);
        break;
    }
    case Icon::More: {
        p.drawPath(circle(12, 12, 8.6));
        QPainterPath dots;
        for (qreal x : {8.0, 12.0, 16.0})
            dots.addEllipse(QPointF(x, 12), 1.25, 1.25);
        fill(dots);
        break;
    }
    case Icon::UpDown:
        p.drawPolyline(QPolygonF({QPointF(8, 9.6), QPointF(12, 5.6), QPointF(16, 9.6)}));
        p.drawPolyline(QPolygonF({QPointF(8, 14.4), QPointF(12, 18.4), QPointF(16, 14.4)}));
        break;
    case Icon::Chevron: {
        QPen chevron = pen;
        chevron.setWidthF(2.2);
        p.setPen(chevron);
        p.drawPolyline(QPolygonF({QPointF(6.5, 9.5), QPointF(12, 15), QPointF(17.5, 9.5)}));
        break;
    }
    case Icon::Export: {
        // out of the tray: the print, sent on
        QPainterPath tray;
        tray.moveTo(4, 14.2);
        tray.lineTo(4, 18.2);
        tray.quadTo(4, 20.2, 6, 20.2);
        tray.lineTo(18, 20.2);
        tray.quadTo(20, 20.2, 20, 18.2);
        tray.lineTo(20, 14.2);
        p.drawPath(tray);
        QPainterPath arrow;
        arrow.moveTo(12, 15.6);
        arrow.lineTo(12, 3.6);
        arrow.moveTo(7.8, 7.8);
        arrow.lineTo(12, 3.6);
        arrow.lineTo(16.2, 7.8);
        p.drawPath(arrow);
        break;
    }
    }
}

QColor inkFor(QIcon::Mode mode, QIcon::State state, bool brighterOn = true)
{
    if (mode == QIcon::Disabled)
        return QColor(104, 104, 104);
    if ((state == QIcon::On && brighterOn) || mode == QIcon::Selected || mode == QIcon::Active)
        return QColor(255, 255, 255);
    return QColor(214, 214, 214);
}

// Draws the icon at whatever size and scale it is asked for.
class Engine : public QIconEngine
{
public:
    Engine(Icon icon, bool brighterOn) : m_icon(icon), m_brighterOn(brighterOn) {}
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        icons::paint(*p, QRectF(rect), m_icon, inkFor(mode, state, m_brighterOn));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pixmap((QSizeF(size) * scale).toSize());
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(scale);
        QPainter p(&pixmap);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }
    QIconEngine *clone() const override { return new Engine(m_icon, m_brighterOn); }
    QString key() const override { return QStringLiteral("flx.icons"); }

private:
    Icon m_icon;
    bool m_brighterOn;
};

} // namespace

QIcon icon(Icon which, bool brighterOn)
{
    return QIcon(new Engine(which, brighterOn));
}

void paint(QPainter &p, const QRectF &box, Icon which, const QColor &ink)
{
    const qreal w = gridWidth(which), h = kGrid;
    const qreal scale = std::min(box.width() / w, box.height() / h);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(box.center().x() - w * scale / 2, box.center().y() - h * scale / 2);
    p.scale(scale, scale);
    draw(p, which, ink);
    p.restore();
}

QImage sheet(int size, qreal scale)
{
    const Icon all[] = {Icon::Sidebar,    Icon::Folder,      Icon::SurroundDark, Icon::SurroundGrey, Icon::SurroundWhite,
                        Icon::Stills,     Icon::Motion,      Icon::Copy,         Icon::Paste,        Icon::Presets,
                        Icon::Crop,       Icon::Original,    Icon::Loupe,        Icon::Export,       Icon::Pick,
                        Icon::Picked,     Icon::Reject,      Icon::RotateLeft,   Icon::RotateRight,  Icon::FlipAcross,
                        Icon::FlipUpDown, Icon::Level,       Icon::SwapAspect,   Icon::Gradient,     Icon::Oval,
                        Icon::Polygon,    Icon::More,        Icon::UpDown};
    const int pad = size / 2;
    int width = pad;
    for (Icon i : all)
        width += int(size * gridWidth(i) / kGrid) + pad;
    QImage image(QSize(width, size * 2 + pad * 3) * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(QColor(42, 42, 42));
    QPainter p(&image);
    // a row as it shows normally, and a row as it shows when on
    for (int row = 0; row < 2; ++row) {
        qreal x = pad;
        for (Icon i : all) {
            const qreal w = size * gridWidth(i) / kGrid;
            paint(p, QRectF(x, pad + row * (size + pad), w, size), i,
                  row == 0 ? inkFor(QIcon::Normal, QIcon::Off) : inkFor(QIcon::Normal, QIcon::On));
            x += w + pad;
        }
    }
    return image;
}

} // namespace icons
