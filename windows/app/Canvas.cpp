#include "Canvas.h"
#include "DisplayColour.h"

#include "Document.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kPadding = 28;
constexpr double kPi = 3.14159265358979323846;

QColor surroundColour(Surround s)
{
    switch (s) {
    // not pure black: an absolute black surround makes everything look milky
    case Surround::Dark: return QColor::fromRgbF(0.07f, 0.07f, 0.07f);
    // L* 50, the neutral a print is normally assessed against
    case Surround::MiddleGrey: return QColor::fromRgbF(0.4663f, 0.4663f, 0.4663f);
    case Surround::White: return QColor(255, 255, 255);
    }
    return Qt::black;
}

// A pill of text, as the Mac's capsules.
// Text on a rounded capsule, centred above `centreBottom`; wrapped onto more
// lines where it is wider than `maxWidth` (0: one line however long). Returns
// its height.
qreal capsule(QPainter &p, const QPointF &centreBottom, const QString &text, const QFont &font, const QColor &fill,
              const QColor &ink, qreal maxWidth = 0)
{
    p.setFont(font);
    const QFontMetricsF m(font);
    QRectF r(0, 0, m.horizontalAdvance(text) + 24, m.height() + 10);
    int flags = Qt::AlignCenter;
    if (maxWidth > 0 && r.width() > maxWidth) {
        flags |= Qt::TextWordWrap;
        const QRectF wrapped = m.boundingRect(QRectF(0, 0, maxWidth - 24, 1e6), flags, text);
        r = QRectF(0, 0, wrapped.width() + 24, wrapped.height() + 10);
    }
    const QRectF placed = r.translated(centreBottom.x() - r.width() / 2, centreBottom.y() - r.height());
    const qreal radius = std::min(placed.height(), m.height() + 10) / 2;
    QPainterPath path;
    path.addRoundedRect(placed, radius, radius);
    p.fillPath(path, fill);
    p.setPen(ink);
    p.drawText(placed.adjusted(12, 0, -12, 0), flags, text);
    return placed.height();
}

} // namespace

Canvas::Canvas(Document &document, QWidget *parent)
    : QWidget(parent), m_doc(document)
{
    setMinimumSize(600, 400);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_open = new QPushButton(QStringLiteral("Open…"), this);
    m_open->setCursor(Qt::PointingHandCursor);
    m_open->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: white; border: none; border-radius: 5px;"
                                         " padding: 4px 14px; }"
                                         "QPushButton:hover { background: %2; }")
                              .arg(theme::accent().name(), theme::accent().lighter(115).name()));
    connect(m_open, &QPushButton::clicked, this, &Canvas::openRequested);
    setMouseTracking(true);
    connect(&m_doc, &Document::loupeChanged, this, qOverload<>(&QWidget::update));

    // while cropping: what the drags do, and the way out
    m_cropBar = new QWidget(this);
    m_cropBar->setObjectName(QStringLiteral("cropBar"));
    m_cropBar->setAttribute(Qt::WA_StyledBackground);
    m_cropBar->setStyleSheet(QStringLiteral(
        "#cropBar { background: rgba(40, 40, 40, 225); border-radius: 14px; }"
        "QLabel { color: #a0a0a0; }"
        "QPushButton { background: #3a3a3a; color: #e6e6e6; border: none; border-radius: 5px; padding: 3px 10px; }"
        "QPushButton:checked { background: %1; color: white; }"
        "QPushButton:hover:!checked { background: #474747; }").arg(theme::accent().name()));
    auto *bar = new QHBoxLayout(m_cropBar);
    bar->setContentsMargins(12, 4, 6, 4);
    bar->setSpacing(8);
    m_cropHint = new QLabel;
    m_cropHint->setFont(theme::small());
    m_levelButton = new QPushButton(QStringLiteral("Level"));
    m_levelButton->setCheckable(true);
    m_levelButton->setToolTip(QStringLiteral("Draw a line on the picture along a horizon, or along an upright, and "
                                             "the picture is levelled to it."));
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    auto *done = new QPushButton(QStringLiteral("Done"));
    for (QPushButton *b : {m_levelButton, cancel, done}) {
        b->setFont(theme::small());
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
    }
    bar->addWidget(m_cropHint);
    bar->addWidget(m_levelButton);
    bar->addWidget(cancel);
    bar->addWidget(done);
    m_cropBar->hide();
    connect(m_levelButton, &QPushButton::clicked, this, [this](bool on) { m_doc.setLevelling(on); });
    connect(cancel, &QPushButton::clicked, this, [this] { m_doc.endCropping(false); });
    connect(done, &QPushButton::clicked, this, [this] { m_doc.endCropping(true); });
    connect(&m_doc, &Document::imageChanged, this, [this] {
        m_cropBar->setVisible(m_doc.isCropping() && !originalShowing());
        update();
    });
    connect(&m_doc, &Document::changed, this, [this] {
        m_open->setVisible(m_doc.displayImage().isNull());
        m_cropBar->setVisible(m_doc.isCropping() && !originalShowing());
        m_levelButton->setChecked(m_doc.isLevelling());
        m_cropHint->setText(m_doc.isLevelling()
                                ? QStringLiteral("Draw a line along something that should be level or upright")
                                : QStringLiteral("Drag a corner or an edge · drag inside to move"));
        layoutCropBar();
        update();
    });
}

void Canvas::setSurround(Surround surround)
{
    m_surround = surround;
    update();
}

bool Canvas::originalShowing() const
{
    return m_doc.showOriginal() && !m_doc.originalShown().isNull();
}

QRect Canvas::pictureRect() const
{
    const QImage &image = m_doc.displayImage();
    if (image.isNull())
        return {};
    const QRectF area = QRectF(rect()).adjusted(kPadding, kPadding, -kPadding, -kPadding);
    const double scale = std::min(area.width() / image.width(), area.height() / image.height());
    const QSizeF size(image.width() * scale, image.height() * scale);
    return QRectF(area.center().x() - size.width() / 2, area.center().y() - size.height() / 2, size.width(),
                  size.height())
        .toRect();
}

void Canvas::resizeEvent(QResizeEvent *)
{
    layoutPlaceholder();
    layoutCropBar();
}

void Canvas::layoutCropBar()
{
    m_cropBar->adjustSize();
    m_cropBar->move(width() / 2 - m_cropBar->width() / 2, 8);
}

void Canvas::layoutPlaceholder()
{
    m_open->adjustSize();
    m_open->move(width() / 2 - m_open->width() / 2, height() / 2 + 40);
}

void Canvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), displaycolour::toScreen(surroundColour(m_surround)));
    p.setRenderHint(QPainter::Antialiasing);
    const bool light = m_surround == Surround::White;
    // as the screen is given it (DisplayColour): the print, or its original
    // in its place while that shows
    const bool original = originalShowing();
    const QImage &image = original ? m_doc.originalShown() : m_doc.shownImage();

    if (!image.isNull()) {
        const QRect target = pictureRect();
        // a contact shadow, lighter on a light surround, so the frame reads as an edge
        const int radius = light ? 10 : 18;
        const double strength = light ? 0.22 : 0.5;
        p.setPen(Qt::NoPen);
        // none behind a frame whose levelled corners are empty
        for (int i = image.hasAlphaChannel() ? 0 : radius; i > 0; i -= 2) {
            const double a = strength * (1.0 - double(i) / radius) * 2.0 / radius * 2.5;
            p.setBrush(QColor::fromRgbF(0, 0, 0, std::min(a, 1.0)));
            p.drawRoundedRect(QRectF(target).adjusted(-i * 0.6, -i * 0.6 + 5, i * 0.6, i * 0.6 + 5), i * 0.6, i * 0.6);
        }
        // the frame scaled once to the pixels it fills, with a good filter
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = (QSizeF(target.size()) * dpr).toSize();
        if (m_scaledKey != image.cacheKey() || m_scaledSize != pixels) {
            m_scaled = QPixmap::fromImage(image.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            m_scaled.setDevicePixelRatio(dpr);
            m_scaledKey = image.cacheKey();
            m_scaledSize = pixels;
        }
        p.drawPixmap(target.topLeft(), m_scaled);
        if (original) {
            // what is shown, where the crop's bar is while cropping
            QFont font = theme::small();
            font.setWeight(QFont::DemiBold);
            capsule(p, QPointF(width() / 2.0, 8 + QFontMetricsF(font).height() + 10), QStringLiteral("Original"), font,
                    light ? QColor(235, 235, 235, 230) : QColor(40, 40, 40, 220),
                    light ? QColor(40, 40, 40) : QColor(230, 230, 230));
        } else if (m_doc.isCropping()) {
            paintCrop(p);
        } else {
            paintMasks(p);
        }
        if (m_doc.loupeOn() && !m_doc.isCropping() && !original)
            paintLoupe(p);
    } else {
        // the placeholder
        const QColor ink = light ? QColor(60, 60, 60) : theme::secondary();
        const QColor faint = light ? QColor(130, 130, 130) : theme::tertiary();
        const QPointF c(width() / 2.0, height() / 2.0 - 30);
        p.setPen(QPen(faint, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, 21, 21);
        for (int i = 0; i < 6; ++i) {
            const double a = i * kPi / 3;
            p.drawLine(QPointF(c.x() + 21 * std::cos(a), c.y() + 21 * std::sin(a)),
                       QPointF(c.x() + 9 * std::cos(a + 1.1), c.y() + 9 * std::sin(a + 1.1)));
        }
        QFont title = theme::label();
        title.setPixelSize(15);
        title.setWeight(QFont::Medium);
        p.setFont(title);
        p.setPen(ink);
        p.drawText(QRectF(0, c.y() + 30, width(), 22), Qt::AlignCenter, QStringLiteral("Drop a RAW file here"));
        p.setFont(theme::small());
        p.setPen(faint);
        p.drawText(QRectF(0, c.y() + 52, width(), 18), Qt::AlignCenter,
                   QStringLiteral("Nikon NEF, Canon CR3, Sony ARW, Fujifilm RAF, DNG and the other RAW formats"));
    }

    // an error above the status, then the status
    double bottom = height() - 10;
    QStringList parts{m_doc.status()};
    // only once they have taken a while, so stepping through pictures does
    // not make the line jump
    if (m_doc.isBusy() && m_doc.isDecoding())
        parts << QStringLiteral("decoding…");
    else if (m_doc.isRenderingSlowly())
        parts << QStringLiteral("rendering…");
    capsule(p, QPointF(width() / 2.0, bottom), parts.join(QStringLiteral("   ")), theme::mono(11),
            light ? QColor(235, 235, 235, 230) : QColor(40, 40, 40, 220), light ? QColor(70, 70, 70) : theme::secondary());
    bottom -= 34;
    if (!m_doc.errorMessage().isEmpty())
        capsule(p, QPointF(width() / 2.0, bottom), m_doc.errorMessage(), theme::small(), QColor(215, 45, 45, 217),
                Qt::white, std::min(width() - 80.0, 760.0));
}

// MARK: - The crop (CropOverlay)

PointD Canvas::frame() const
{
    if (const auto f = m_doc.turnedFrame())
        return *f;
    const QRect r = pictureRect();
    return {double(r.width()), double(r.height())};
}

double Canvas::scale() const
{
    return pictureRect().width() / std::max(frame().x, 1.0);
}

// After a turn, until the turned frame has rendered, the crop is not drawn.
bool Canvas::frameShowing() const
{
    const auto &map = m_doc.displayMap();
    if (!map)
        return false;
    const PointD f = frame();
    return std::abs(map->frame.x - f.x) < 1 && std::abs(map->frame.y - f.y) < 1;
}

QRectF Canvas::view(const RectD &r) const
{
    const QRect picture = pictureRect();
    const double s = scale();
    return QRectF(picture.left() + r.x * s, picture.top() + r.y * s, r.w * s, r.h * s);
}

Canvas::Hit Canvas::hit(QPointF v, const QRectF &r) const
{
    if (m_doc.isLevelling())
        return {Part::Level};
    const double reach = 10;
    const bool nearLeft = std::abs(v.x() - r.left()) < reach, nearRight = std::abs(v.x() - r.right()) < reach;
    const bool nearTop = std::abs(v.y() - r.top()) < reach, nearBottom = std::abs(v.y() - r.bottom()) < reach;
    const bool withinX = v.x() > r.left() - reach && v.x() < r.right() + reach;
    const bool withinY = v.y() > r.top() - reach && v.y() < r.bottom() + reach;
    const int x = nearLeft ? -1 : (nearRight ? 1 : 0), y = nearTop ? -1 : (nearBottom ? 1 : 0);
    if (x != 0 && y != 0 && withinX && withinY)
        return {Part::Corner, x, y};
    if (x != 0 && withinY)
        return {Part::Edge, x, 0};
    if (y != 0 && withinX)
        return {Part::Edge, 0, y};
    if (r.contains(v))
        return {Part::Move};
    return {};
}

void Canvas::paintCrop(QPainter &p)
{
    if (!frameShowing())
        return;
    const QRect picture = pictureRect();
    const QRectF rect = view(m_liveRect.value_or(m_doc.recipe().geometry.cropRect(frame())));
    p.save();
    // everything the crop leaves out, dimmed
    QPainterPath outside;
    outside.addRect(picture);
    outside.addRect(rect);
    outside.setFillRule(Qt::OddEvenFill);
    p.fillPath(outside, QColor(0, 0, 0, 140));
    // the thirds
    p.setPen(QPen(QColor(255, 255, 255, m_drag.part == Part::None ? 56 : 128), 0.5));
    for (int i = 1; i <= 2; ++i) {
        const double x = rect.left() + rect.width() * i / 3, y = rect.top() + rect.height() * i / 3;
        p.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        p.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
    p.setPen(QPen(QColor(255, 255, 255, 230), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect);
    // L-shaped corners and short bars at the middle of each edge, just
    // inside the crop, so they never hide what it leaves out
    const double longSide = std::min({18.0, rect.width() / 3, rect.height() / 3}), thick = 3;
    QPainterPath handles;
    for (const QPointF c : {rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()}) {
        const double sx = c.x() == rect.left() ? 1 : -1, sy = c.y() == rect.top() ? 1 : -1;
        handles.addRect(QRectF(std::min(c.x(), c.x() + sx * longSide), std::min(c.y(), c.y() + sy * thick),
                               longSide, thick));
        handles.addRect(QRectF(std::min(c.x(), c.x() + sx * thick), std::min(c.y(), c.y() + sy * longSide),
                               thick, longSide));
    }
    const double bar = std::min({16.0, rect.width() / 4, rect.height() / 4});
    handles.addRect(QRectF(rect.center().x() - bar / 2, rect.top(), bar, thick));
    handles.addRect(QRectF(rect.center().x() - bar / 2, rect.bottom() - thick, bar, thick));
    handles.addRect(QRectF(rect.left(), rect.center().y() - bar / 2, thick, bar));
    handles.addRect(QRectF(rect.right() - thick, rect.center().y() - bar / 2, thick, bar));
    handles.setFillRule(Qt::WindingFill);
    p.fillPath(handles.translated(0, 1), QColor(0, 0, 0, 120));
    p.fillPath(handles, Qt::white);
    if (m_drag.part == Part::Level) {
        p.setPen(QPen(theme::accent(), 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(m_dragStart, m_dragNow);
    }
    p.restore();
}

void Canvas::mousePressEvent(QMouseEvent *e)
{
    // the original is only looked at
    if (originalShowing())
        return QWidget::mousePressEvent(e);
    // the loupe, over everything else on the print: held where it was taken,
    // so it moves with the pointer instead of jumping to centre on it
    if (m_doc.loupeOn() && !m_doc.isCropping() && e->button() == Qt::LeftButton
        && loupeRect().contains(e->position())) {
        m_loupeGrab = e->position() - loupeRect().center();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (!m_doc.isCropping() && e->button() == Qt::LeftButton && !m_doc.displayImage().isNull()) {
        // the nearest of the selected item's handles, within reach
        const std::vector<PointD> handles = selectedHandles();
        double best = 13.5;
        for (size_t i = 0; i < handles.size(); ++i) {
            const QPointF h = toView(handles[i]);
            const double d = std::hypot(h.x() - e->position().x(), h.y() - e->position().y());
            if (d < best) {
                best = d;
                m_handle = int(i);
            }
        }
        if (m_handle >= 0) {
            m_handleStart = e->position();
            m_handleOrigin = handles;
            setCursor(Qt::ClosedHandCursor);
        }
        return;
    }
    if (!m_doc.isCropping() || !frameShowing() || e->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(e);
    const RectD start = m_doc.recipe().geometry.cropRect(frame());
    m_drag = hit(e->position(), view(start));
    m_startRect = start;
    m_dragStart = m_dragNow = e->position();
}

void Canvas::mouseMoveEvent(QMouseEvent *e)
{
    if (originalShowing() && !m_loupeGrab && m_handle < 0 && m_drag.part == Part::None) {
        unsetCursor();
        return;
    }
    if (m_loupeGrab) {
        const QRect target = pictureRect();
        const QPointF at = e->position() - *m_loupeGrab;
        m_doc.moveLoupe(QPointF((at.x() - target.left()) / std::max(target.width(), 1),
                                (at.y() - target.top()) / std::max(target.height(), 1)));
        return;
    }
    if (m_handle >= 0) {
        // the handle moves by the drag, not to the cursor
        const QPointF shift = e->position() - m_handleStart;
        auto dragged = [&](PointD p) { return fromView(toView(p) + shift); };
        Recipe r = m_doc.recipe();
        const std::string &id = m_doc.selected();
        for (LocalMask &m : r.masks) {
            if (m.id != id || m.points.size() != m_handleOrigin.size())
                continue;
            if (m.shape == MaskShape::Oval && m_handle == 0) {
                // an oval's axes go with its centre, so it moves rather than reshapes
                const PointD moved = dragged(m_handleOrigin[0]);
                for (size_t j = 0; j < m.points.size(); ++j)
                    m.points[j] = {m_handleOrigin[j].x + moved.x - m_handleOrigin[0].x,
                                   m_handleOrigin[j].y + moved.y - m_handleOrigin[0].y};
            } else {
                m.points[size_t(m_handle)] = dragged(m_handleOrigin[size_t(m_handle)]);
            }
        }
        for (ExposureGradient &g : r.gradients) {
            if (g.id != id || m_handleOrigin.size() != 3)
                continue;
            if (g.isOval()) {
                // the centre carries the oval; an axis end shapes it
                PointD c = m_handleOrigin[0], u = m_handleOrigin[1], v = m_handleOrigin[2];
                if (m_handle == 0) {
                    const PointD moved = dragged(c);
                    const double dx = moved.x - c.x, dy = moved.y - c.y;
                    c = moved;
                    u = {u.x + dx, u.y + dy};
                    v = {v.x + dx, v.y + dy};
                } else if (m_handle == 1) {
                    u = dragged(u);
                } else {
                    v = dragged(v);
                }
                g.ax = c.x;
                g.ay = c.y;
                g.bx = u.x;
                g.by = u.y;
                g.cx = v.x;
                g.cy = v.y;
                continue;
            }
            // the middle handle slides the whole transition
            const PointD a = m_handle == 1 ? m_handleOrigin[0] : dragged(m_handleOrigin[0]);
            const PointD b = m_handle == 0 ? m_handleOrigin[1] : dragged(m_handleOrigin[1]);
            g.ax = a.x;
            g.ay = a.y;
            g.bx = b.x;
            g.by = b.y;
        }
        m_doc.setRecipe(r);
        return;
    }
    if (!m_doc.isCropping()) {
        // over a handle, the cursor says it can be dragged
        bool over = false;
        for (const PointD &h : selectedHandles()) {
            const QPointF v = toView(h);
            over = over || std::hypot(v.x() - e->position().x(), v.y() - e->position().y()) < 13.5;
        }
        if (over)
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
        return;
    }
    if (!frameShowing()) {
        unsetCursor();
        return;
    }
    if (m_drag.part == Part::None) {
        // the cursor says what a drag from here does
        const Hit h = hit(e->position(), view(m_doc.recipe().geometry.cropRect(frame())));
        switch (h.part) {
        case Part::Edge: setCursor(h.x != 0 ? Qt::SizeHorCursor : Qt::SizeVerCursor); break;
        case Part::Corner: setCursor(h.x == h.y ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor); break;
        case Part::Move: setCursor(Qt::OpenHandCursor); break;
        case Part::Level: setCursor(Qt::CrossCursor); break;
        case Part::None: unsetCursor(); break;
        }
        return;
    }
    m_dragNow = e->position();
    if (m_drag.part != Part::Level && m_startRect) {
        const double s = scale();
        m_liveRect = dragged(m_drag, *m_startRect, (m_dragNow.x() - m_dragStart.x()) / s,
                             (m_dragNow.y() - m_dragStart.y()) / s);
    }
    update();
}

void Canvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_loupeGrab) {
        m_loupeGrab.reset();
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (m_handle >= 0) {
        m_handle = -1;
        m_handleOrigin.clear();
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (m_drag.part == Part::None)
        return QWidget::mouseReleaseEvent(e);
    const Hit drag = m_drag;
    const std::optional<RectD> rect = m_liveRect;
    m_drag = {};
    m_startRect.reset();
    m_liveRect.reset();
    if (drag.part == Part::Level) {
        const QRect picture = pictureRect();
        auto normalised = [&](QPointF v) {
            return PointD{(v.x() - picture.left()) / std::max(picture.width(), 1),
                          (v.y() - picture.top()) / std::max(picture.height(), 1)};
        };
        m_doc.level(normalised(m_dragStart), normalised(e->position()));
        m_doc.setLevelling(false);
    } else if (rect) {
        m_doc.editGeometry([&](FrameGeometry &g, PointD f) { g.setCrop(*rect, f); });
    }
    update();
}

// The crop moved or resized by a drag of (dx, dy) frame pixels, as far as it
// can go inside the picture.
RectD Canvas::dragged(const Hit &h, const RectD &s, double dx, double dy) const
{
    const auto ratio = m_doc.recipe().geometry.ratio(frame());
    const double minimum = std::max(24 / scale(), 8.0);
    auto anchored = [](PointD anchor, int x, int y, double w, double hh) {
        return RectD{x < 0 ? anchor.x - w : anchor.x, y < 0 ? anchor.y - hh : anchor.y, w, hh};
    };
    switch (h.part) {
    case Part::Move: {
        // one axis at a time, so a crop pushed against an edge slides along it
        const RectD across = furthest(s, s.offset(dx, 0));
        return furthest(across, across.offset(0, dy));
    }
    case Part::Corner: {
        // the opposite corner stays put
        const PointD anchor{h.x < 0 ? s.maxX() : s.minX(), h.y < 0 ? s.maxY() : s.minY()};
        double w = std::max(h.x < 0 ? s.maxX() - (s.minX() + dx) : s.maxX() + dx - s.minX(), minimum);
        double hh = std::max(h.y < 0 ? s.maxY() - (s.minY() + dy) : s.maxY() + dy - s.minY(), minimum);
        if (ratio) {
            const double r = *ratio;
            // whichever side the drag moved further sets the size
            if (w / r > hh)
                hh = w / r;
            else
                w = hh * r;
            if (hh < minimum) {
                hh = minimum;
                w = hh * r;
            }
            if (w < minimum) {
                w = minimum;
                hh = w / r;
            }
            return furthest(s, anchored(anchor, h.x, h.y, w, hh));
        }
        // a free corner one side at a time, so it slides along an edge it meets
        const RectD wide = furthest(s, anchored(anchor, h.x, h.y, w, s.h));
        return furthest(wide, anchored(anchor, h.x, h.y, wide.w, hh));
    }
    case Part::Edge: {
        const bool sideways = h.x != 0;     // the left or the right edge, which sets the width
        const double wanted = sideways ? std::max(h.x < 0 ? s.maxX() - (s.minX() + dx) : s.w + dx, minimum)
                                       : std::max(h.y < 0 ? s.maxY() - (s.minY() + dy) : s.h + dy, minimum);
        if (!ratio) {
            const RectD target = sideways ? RectD{h.x < 0 ? s.maxX() - wanted : s.minX(), s.minY(), wanted, s.h}
                                          : RectD{s.minX(), h.y < 0 ? s.maxY() - wanted : s.minY(), s.w, wanted};
            return furthest(s, target);
        }
        // with fixed proportions the other sides follow about the crop's
        // middle, moved as little as keeps it inside the picture
        const double r = *ratio;
        auto fit = [&](double side) -> std::optional<RectD> {
            const PointD size = sideways ? PointD{side, side / r} : PointD{side * r, side};
            return sideways ? place(size, h.x < 0 ? s.maxX() - size.x : s.minX(), true, s.midY() - size.y / 2)
                            : place(size, h.y < 0 ? s.maxY() - size.y : s.minY(), false, s.midX() - size.x / 2);
        };
        if (const auto whole = fit(wanted))
            return *whole;
        // as large as fits between the crop as it was and the size asked for
        double lo = sideways ? s.w : s.h, hi = wanted;
        RectD best = s;
        for (int i = 0; i < 24; ++i) {
            const double mid = (lo + hi) / 2;
            if (const auto rect = fit(mid)) {
                best = *rect;
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return best;
    }
    default:
        return s;
    }
}

// A crop of `size` at `fixed` along one axis, placed along the other as near
// `near` as it can be inside the picture, if it fits anywhere. The picture is
// convex, so the places that fit are one stretch.
std::optional<RectD> Canvas::place(PointD size, double fixed, bool fixedIsX, double near) const
{
    const FrameGeometry &g = m_doc.recipe().geometry;
    const PointD f = frame();
    auto rect = [&](double v) { return fixedIsX ? RectD{fixed, v, size.x, size.y} : RectD{v, fixed, size.x, size.y}; };
    const double span = fixedIsX ? f.y - size.y : f.x - size.x;
    if (span < 0)
        return std::nullopt;
    const double start = std::clamp(near, 0.0, span);
    if (g.contains(rect(start), f))
        return rect(start);
    std::optional<double> inside;
    for (int i = 0; i <= 32; ++i) {
        const double v = span * i / 32;
        if (g.contains(rect(v), f) && (!inside || std::abs(v - start) < std::abs(*inside - start)))
            inside = v;
    }
    if (!inside)
        return std::nullopt;
    double in = *inside, out = start;
    for (int i = 0; i < 24; ++i) {
        const double mid = (in + out) / 2;
        if (g.contains(rect(mid), f))
            in = mid;
        else
            out = mid;
    }
    return rect(in);
}

// As far from `a`, which lies inside the picture, towards `b` as stays inside it.
RectD Canvas::furthest(const RectD &a, const RectD &b) const
{
    const FrameGeometry &g = m_doc.recipe().geometry;
    const PointD f = frame();
    if (g.contains(b, f))
        return b;
    auto between = [&](double t) {
        return RectD{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2;
        if (g.contains(between(mid), f))
            lo = mid;
        else
            hi = mid;
    }
    return between(lo);
}

// MARK: - Masks and gradients (MaskOverlay)

// Points belong to the frame, not the print: on a turned, levelled or cropped
// print they are drawn through where the image on screen comes from.
QPointF Canvas::toView(PointD p) const
{
    const QRect picture = pictureRect();
    const auto &map = m_doc.displayMap();
    if (!map || map->isIdentity)
        return QPointF(picture.left() + p.x * picture.width(), picture.top() + p.y * picture.height());
    const PointD o = map->outputPoint(p);
    return QPointF(picture.left() + o.x / map->outputWidth() * picture.width(),
                   picture.top() + o.y / map->outputHeight() * picture.height());
}

PointD Canvas::fromView(QPointF v) const
{
    const QRect picture = pictureRect();
    PointD n{(v.x() - picture.left()) / std::max(picture.width(), 1),
             (v.y() - picture.top()) / std::max(picture.height(), 1)};
    const auto &map = m_doc.displayMap();
    if (map && !map->isIdentity)
        n = map->normalisedSource({n.x * map->outputWidth(), n.y * map->outputHeight()});
    return {std::clamp(n.x, -0.5, 1.5), std::clamp(n.y, -0.5, 1.5)};
}

// The selected mask's points, or the selected gradient's two and the one
// between them; `kinds` 1 for a handle that moves the whole (an oval's
// centre, a gradient's middle).
std::vector<PointD> Canvas::selectedHandles(std::vector<int> *kinds) const
{
    const std::string &id = m_doc.selected();
    if (id.empty() || m_doc.maskLinesHidden())
        return {};
    for (const LocalMask &m : m_doc.recipe().masks) {
        if (m.id == id && m.enabled) {
            if (kinds)
                for (size_t i = 0; i < m.points.size(); ++i)
                    kinds->push_back(m.shape == MaskShape::Oval && i == 0 ? 1 : 0);
            return m.points;
        }
    }
    for (const ExposureGradient &g : m_doc.recipe().gradients) {
        if (g.id == id && g.enabled && g.isOval() && g.points >= 3) {
            // the centre carries the oval; each axis end shapes it
            if (kinds)
                *kinds = {1, 0, 0};
            return {{g.ax, g.ay}, {g.bx, g.by}, {g.cx, g.cy}};
        }
        if (g.id == id && g.enabled && !g.isOval() && g.points >= 2) {
            if (kinds)
                *kinds = {0, 0, 1};
            return {{g.ax, g.ay}, {g.bx, g.by}, {(g.ax + g.bx) / 2, (g.ay + g.by) / 2}};
        }
    }
    return {};
}

void Canvas::paintMasks(QPainter &p)
{
    const Recipe &r = m_doc.recipe();
    // hidden, to see the print without them: they still act
    if ((r.masks.empty() && r.gradients.empty()) || m_doc.maskLinesHidden())
        return;
    const bool light = m_surround == Surround::White;
    const QRect picture = pictureRect();
    const double reach = std::max(picture.width(), picture.height()) * 2;
    const std::string &selected = m_doc.selected();
    auto colour = [&](bool isSelected) {
        return isSelected ? theme::accent() : light ? QColor(0, 0, 0, 115) : QColor(255, 255, 255, 115);
    };
    auto stroke = [&](const QPainterPath &path, bool isSelected, bool dashed) {
        // a soft dark (or light) edge first, so the line reads on any picture
        QPen halo(light ? QColor(255, 255, 255, 120) : QColor(0, 0, 0, 120), isSelected ? 3 : 2.5);
        if (dashed)
            halo.setDashPattern({4 / 2.5, 3 / 2.5});
        p.strokePath(path, halo);
        QPen pen(colour(isSelected), isSelected ? 1.5 : 1);
        if (dashed)
            pen.setDashPattern(isSelected ? QList<qreal>{4 / 1.5, 3 / 1.5} : QList<qreal>{4, 3});
        p.strokePath(path, pen);
    };
    // the two card edges of a gradient, perpendicular to its handles, right across
    auto across = [&](QPointF a, QPointF b, QPointF at) {
        const QPointF d = b - a;
        const double length = std::max(std::hypot(d.x(), d.y()), 1.0);
        const QPointF n(-d.y() / length, d.x() / length);
        QPainterPath path;
        path.moveTo(at - n * reach);
        path.lineTo(at + n * reach);
        return path;
    };
    p.save();
    p.setClipRect(picture);
    p.setRenderHint(QPainter::Antialiasing);
    for (const LocalMask &m : r.masks) {
        if (!m.enabled || m.points.empty())
            continue;
        std::vector<QPointF> pts;
        for (const PointD &q : m.points)
            pts.push_back(toView(q));
        QPainterPath path;
        if (m.shape == MaskShape::Polygon && pts.size() >= 3) {
            path.moveTo(pts[0]);
            for (size_t i = 1; i < pts.size(); ++i)
                path.lineTo(pts[i]);
            path.closeSubpath();
        } else if (m.shape == MaskShape::Oval && pts.size() >= 3) {
            const QPointF c = pts[0], u = pts[1] - c, v = pts[2] - c;
            for (int i = 0; i <= 64; ++i) {
                const double t = 2 * kPi * i / 64;
                const QPointF q = c + u * std::cos(t) + v * std::sin(t);
                if (i == 0)
                    path.moveTo(q);
                else
                    path.lineTo(q);
            }
        } else if (m.shape == MaskShape::Gradient && pts.size() >= 2) {
            path.addPath(across(pts[0], pts[1], pts[0]));
            path.addPath(across(pts[0], pts[1], pts[1]));
            path.moveTo(pts[0]);
            path.lineTo(pts[1]);
        }
        // a dodge dashed, a burn solid
        stroke(path, m.id == selected, m.stops < 0);
    }
    for (const ExposureGradient &g : r.gradients) {
        if (!g.enabled || g.points < (g.isOval() ? 3 : 2))
            continue;
        if (g.isOval()) {
            // the oval itself, where its compensation has faded out, and the
            // oval shrunk by its feather, inside which it is full; swapped when
            // it acts outside. Each sample through toView, so a turned or
            // levelled print draws it where the frame's pixels went.
            auto ellipse = [&](double scale) {
                QPainterPath path;
                for (int i = 0; i <= 96; ++i) {
                    const double t = 2 * kPi * i / 96;
                    const QPointF q = toView({g.ax + scale * ((g.bx - g.ax) * std::cos(t) + (g.cx - g.ax) * std::sin(t)),
                                              g.ay + scale * ((g.by - g.ay) * std::cos(t) + (g.cy - g.ay) * std::sin(t))});
                    if (i == 0)
                        path.moveTo(q);
                    else
                        path.lineTo(q);
                }
                return path;
            };
            const QPainterPath outer = ellipse(1), inner = ellipse(1 - std::clamp(g.featherAmount(), 0.0, 1.0));
            stroke(g.isInverted() ? outer : inner, g.id == selected, false);
            stroke(g.isInverted() ? inner : outer, g.id == selected, true);
            continue;
        }
        const QPointF a = toView({g.ax, g.ay}), b = toView({g.bx, g.by});
        // solid where the compensation is full, dashed where it has faded out
        QPainterPath full = across(a, b, a);
        full.moveTo(a);
        full.lineTo(b);
        stroke(full, g.id == selected, false);
        stroke(across(a, b, b), g.id == selected, true);
    }
    p.setClipping(false);
    std::vector<int> kinds;
    const std::vector<PointD> handles = selectedHandles(&kinds);
    for (size_t i = 0; i < handles.size(); ++i) {
        const bool whole = kinds[i] == 1;
        const double radius = whole ? 5.5 : 4.5;
        p.setPen(QPen(whole ? theme::accent() : Qt::white, 1.4));
        p.setBrush(whole ? QColor(Qt::white) : theme::accent());
        p.drawEllipse(toView(handles[i]), radius, radius);
    }
    p.restore();
}

// MARK: - The loupe

QRectF Canvas::loupeRect() const
{
    const QRect target = pictureRect();
    const QPointF c(target.left() + m_doc.loupeCentre().x() * target.width(),
                    target.top() + m_doc.loupeCentre().y() * target.height());
    return {c.x() - kLoupe / 2, c.y() - kLoupe / 2, kLoupe, kLoupe};
}

// The print at 100% under a square dragged over it: one pixel of the export
// to one pixel of the screen, as the export will have it.
void Canvas::paintLoupe(QPainter &p)
{
    const qreal dpr = devicePixelRatioF();
    const int side = int(std::lround(kLoupe * dpr));
    m_doc.setLoupeSize(QSize(side, side));
    const QRectF square = loupeRect();
    p.save();
    for (int i = 8; i > 0; i -= 2) {                // its shadow
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 18));
        p.drawRoundedRect(square.adjusted(-i * 0.5, -i * 0.5 + 3, i * 0.5, i * 0.5 + 3), 8 + i * 0.5, 8 + i * 0.5);
    }
    QPainterPath clip;
    clip.addRoundedRect(square, 8, 8);
    p.fillPath(clip, QColor(0, 0, 0, 153));
    p.setClipPath(clip);
    QImage image = m_doc.loupeImage();
    if (!image.isNull()) {
        image.setDevicePixelRatio(dpr);
        p.drawImage(square.topLeft(), image);
    }
    if (m_doc.loupeLoading()) {
        p.setPen(Qt::white);
        p.setFont(theme::small());
        p.drawText(square, Qt::AlignCenter, QStringLiteral("Reading the picture at full size…"));
    }
    p.setClipping(false);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 217), 1.5));
    p.drawRoundedRect(square.adjusted(0.75, 0.75, -0.75, -0.75), 8, 8);
    // "100%"
    p.setFont(theme::mono(10));
    const QString badge = QStringLiteral("100%");
    const QSizeF text = QFontMetricsF(p.font()).size(0, badge) + QSizeF(10, 4);
    const QRectF pill(square.right() - text.width() - 6, square.bottom() - text.height() - 6, text.width(), text.height());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 128));
    p.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    p.setPen(Qt::white);
    p.drawText(pill, Qt::AlignCenter, badge);
    p.restore();
}

