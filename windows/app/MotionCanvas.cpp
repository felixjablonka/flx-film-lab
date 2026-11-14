#include "MotionCanvas.h"
#include "DisplayColour.h"

#include "MotionDocument.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPadding = 20;

QColor surroundColour(Surround s)
{
    switch (s) {
    case Surround::Dark: return QColor::fromRgbF(0.07f, 0.07f, 0.07f);
    case Surround::MiddleGrey: return QColor::fromRgbF(0.4663f, 0.4663f, 0.4663f);
    case Surround::White: return QColor(255, 255, 255);
    }
    return Qt::black;
}

// A pill of text in parts, each in its own colour, as the Mac's status line.
QRectF capsule(QPainter &p, const QPointF &centreBottom, const std::vector<std::pair<QString, QColor>> &parts,
               const QFont &font, const QColor &fill)
{
    p.setFont(font);
    const QFontMetricsF m(font);
    const double gap = m.horizontalAdvance(QStringLiteral("   "));
    double width = 0;
    for (size_t i = 0; i < parts.size(); ++i)
        width += m.horizontalAdvance(parts[i].first) + (i ? gap : 0);
    const QRectF r(centreBottom.x() - (width + 24) / 2, centreBottom.y() - (m.height() + 10), width + 24,
                   m.height() + 10);
    QPainterPath path;
    path.addRoundedRect(r, r.height() / 2, r.height() / 2);
    p.fillPath(path, fill);
    double x = r.left() + 12;
    for (const auto &[text, ink] : parts) {
        p.setPen(ink);
        const double w = m.horizontalAdvance(text);
        p.drawText(QRectF(x, r.top(), w + 1, r.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
        x += w + gap;
    }
    return r;
}

// A strip of film with its sprocket holes, for the placeholder.
void drawFilm(QPainter &p, QPointF c, const QColor &ink)
{
    p.setPen(QPen(ink, 1.2));
    p.setBrush(Qt::NoBrush);
    const QRectF strip(c.x() - 24, c.y() - 18, 48, 36);
    p.drawRoundedRect(strip, 4, 4);
    p.drawLine(QPointF(strip.left() + 9, strip.top()), QPointF(strip.left() + 9, strip.bottom()));
    p.drawLine(QPointF(strip.right() - 9, strip.top()), QPointF(strip.right() - 9, strip.bottom()));
    for (int i = 0; i < 4; ++i) {
        const double y = strip.top() + 5 + i * 7.5;
        p.drawRect(QRectF(strip.left() + 3, y, 3, 3.5));
        p.drawRect(QRectF(strip.right() - 6, y, 3, 3.5));
    }
}

} // namespace

MotionCanvas::MotionCanvas(MotionDocument &document, QWidget *parent)
    : QWidget(parent), m_doc(document)
{
    setMinimumSize(600, 360);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    m_open = new QPushButton(QStringLiteral("Open…"), this);
    m_open->setCursor(Qt::PointingHandCursor);
    m_open->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: white; border: none; border-radius: 5px;"
                                         " padding: 4px 14px; }"
                                         "QPushButton:hover { background: %2; }")
                              .arg(theme::accent().name(), theme::accent().lighter(115).name()));
    connect(m_open, &QPushButton::clicked, this, &MotionCanvas::openRequested);

    // while exporting: what is being written, how far, and the way out
    m_exportBar = new QWidget(this);
    m_exportBar->setObjectName(QStringLiteral("exportBar"));
    m_exportBar->setAttribute(Qt::WA_StyledBackground);
    m_exportBar->setStyleSheet(QStringLiteral(
        "#exportBar { background: rgba(40, 40, 40, 225); border-radius: 14px; }"
        "QLabel { color: #a0a0a0; }"
        "QProgressBar { background: #2a2a2a; border: none; border-radius: 3px; max-height: 6px; }"
        "QProgressBar::chunk { background: %1; border-radius: 3px; }"
        "QPushButton { background: #3a3a3a; color: #e6e6e6; border: none; border-radius: 5px; padding: 3px 10px; }"
        "QPushButton:hover { background: #474747; }").arg(theme::accent().name()));
    auto *bar = new QHBoxLayout(m_exportBar);
    bar->setContentsMargins(12, 4, 6, 4);
    bar->setSpacing(10);
    m_exportText = new QLabel;
    m_exportText->setFont(theme::mono(10));
    m_exportProgress = new QProgressBar;
    m_exportProgress->setRange(0, 1000);
    m_exportProgress->setTextVisible(false);
    m_exportProgress->setFixedWidth(180);
    m_exportPercent = new QLabel;
    m_exportPercent->setFont(theme::mono(10));
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setFont(theme::small());
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setCursor(Qt::PointingHandCursor);
    bar->addWidget(m_exportText);
    bar->addWidget(m_exportProgress);
    bar->addWidget(m_exportPercent);
    bar->addWidget(cancel);
    m_exportBar->hide();
    connect(cancel, &QPushButton::clicked, this, [this] { m_doc.cancelExport(); });

    connect(&m_doc, &MotionDocument::frameChanged, this, [this] { update(); });
    connect(&m_doc, &MotionDocument::changed, this, [this] {
        m_open->setVisible(!m_doc.clip());
        updateViewSize();
        update();
    });
    connect(&m_doc, &MotionDocument::statusChanged, this, [this] {
        const auto &progress = m_doc.exportProgress();
        m_exportBar->setVisible(progress.has_value());
        if (progress) {
            m_exportText->setText(m_doc.status());
            m_exportProgress->setValue(int(std::lround(*progress * 1000)));
            m_exportPercent->setText(QStringLiteral("%1%").arg(int(*progress * 100)));
            layoutChildren();
        }
        update();
    });
}

void MotionCanvas::setSurround(Surround surround)
{
    m_surround = surround;
    update();
}

std::optional<MotionCanvas::Placement> MotionCanvas::placement() const
{
    const auto &clip = m_doc.clip();
    if (!clip || clip->width <= 0 || clip->height <= 0)
        return std::nullopt;
    const double dpr = devicePixelRatioF();
    const QSizeF destination = QSizeF(size()) * dpr;
    const double padding = kPadding * dpr;
    const QSizeF source(clip->width, clip->height);
    const QSizeF turned = clip->quarterTurns % 2 ? source.transposed() : source;
    const double fit = std::max(std::min((destination.width() - 2 * padding) / turned.width(),
                                         (destination.height() - 2 * padding) / turned.height()),
                                1e-3);
    // never graded at more than the clip's own resolution; scaled up after,
    // if the window is bigger than the clip
    const double gradeScale = std::min(fit, 1.0);
    Placement p;
    p.graded = QSize(std::max(int(std::lround(source.width() * gradeScale)), 1),
                     std::max(int(std::lround(source.height() * gradeScale)), 1));
    const QSizeF shown = turned * fit;
    p.frame = QRectF(std::round((destination.width() - shown.width()) / 2) / dpr,
                     std::round((destination.height() - shown.height()) / 2) / dpr, shown.width() / dpr,
                     shown.height() / dpr);
    return p;
}

void MotionCanvas::updateViewSize()
{
    if (const auto p = placement())
        m_doc.setViewSize(p->graded);
}

void MotionCanvas::resizeEvent(QResizeEvent *)
{
    layoutChildren();
    updateViewSize();
}

void MotionCanvas::layoutChildren()
{
    m_open->adjustSize();
    m_open->move(width() / 2 - m_open->width() / 2, height() / 2 + 40);
    m_exportBar->adjustSize();
    m_exportBar->move(width() / 2 - m_exportBar->width() / 2, height() - 10 - m_exportBar->height());
}

void MotionCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), displaycolour::toScreen(surroundColour(m_surround)));
    p.setRenderHint(QPainter::Antialiasing);
    const bool light = m_surround == Surround::White;
    const QImage &image = m_doc.shownImage();
    const auto place = placement();

    if (place && !image.isNull()) {
        // the frame as graded, turned the way the track asks
        const QRectF &f = place->frame;
        const int turns = m_doc.quarterTurns();
        p.save();
        p.translate(f.center());
        p.rotate(90.0 * turns);
        const QSizeF drawn = turns % 2 ? f.size().transposed() : f.size();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(-drawn.width() / 2, -drawn.height() / 2, drawn.width(), drawn.height()), image);
        p.restore();
    } else if (!m_doc.clip()) {
        // the placeholder
        const QColor ink = light ? QColor(60, 60, 60) : theme::secondary();
        const QColor faint = light ? QColor(130, 130, 130) : theme::tertiary();
        const QPointF c(width() / 2.0, height() / 2.0 - 30);
        drawFilm(p, c, faint);
        QFont title = theme::label();
        title.setPixelSize(15);
        title.setWeight(QFont::Medium);
        p.setFont(title);
        p.setPen(ink);
        p.drawText(QRectF(0, c.y() + 30, width(), 22), Qt::AlignCenter, QStringLiteral("Drop a log clip here"));
        p.setFont(theme::small());
        p.setPen(faint);
        p.drawText(QRectF(0, c.y() + 52, width(), 18), Qt::AlignCenter,
                   QStringLiteral("N-Log, V-Log, Apple Log, F-Log, Canon Log, S-Log3 or I-Log, in any movie"));
    }

    // an error above the status, then the status (the export bar takes its
    // place while a clip is written)
    double bottom = height() - 10;
    if (!m_doc.isExporting()) {
        const QColor fill = light ? QColor(235, 235, 235, 230) : QColor(40, 40, 40, 220);
        const QColor ink = light ? QColor(70, 70, 70) : theme::secondary();
        const QColor faint = light ? QColor(140, 140, 140) : theme::tertiary();
        std::vector<std::pair<QString, QColor>> parts;
        const QString probe = m_doc.probeText();
        parts.emplace_back(probe.isEmpty() ? m_doc.status() : probe, ink);
        if (m_doc.drawMilliseconds() > 0 && m_doc.clip() && probe.isEmpty())
            parts.emplace_back(QStringLiteral("%1 ms").arg(m_doc.drawMilliseconds(), 0, 'f', 0), faint);
        if (m_doc.showLog())
            parts.emplace_back(QStringLiteral("log footage"), QColor(255, 159, 10, 230));
        if (m_doc.isOpening())
            parts.emplace_back(QStringLiteral("opening…"), faint);
        capsule(p, QPointF(width() / 2.0, bottom), parts, theme::mono(11), fill);
    }
    bottom -= 34;
    m_errorRect = QRectF();
    if (!m_doc.errorMessage().isEmpty())
        m_errorRect = capsule(p, QPointF(width() / 2.0, bottom), {{m_doc.errorMessage(), Qt::white}}, theme::small(),
                              QColor(215, 45, 45, 217));
}

// The pixel of the clip under a point of the canvas: the placement undone,
// the turn included.
std::optional<QPointF> MotionCanvas::sourcePixel(QPointF at) const
{
    const auto place = placement();
    const auto &clip = m_doc.clip();
    if (!place || !clip || !place->frame.contains(at))
        return std::nullopt;
    const QRectF &f = place->frame;
    const double u = (at.x() - f.left()) / f.width(), v = (at.y() - f.top()) / f.height();
    const double w = clip->width, h = clip->height;
    switch (clip->quarterTurns % 4) {
    case 1: return QPointF(v * w, (1 - u) * h);
    case 2: return QPointF((1 - u) * w, (1 - v) * h);
    case 3: return QPointF((1 - v) * w, u * h);
    default: return QPointF(u * w, v * h);
    }
}

void MotionCanvas::mouseMoveEvent(QMouseEvent *e)
{
    m_doc.probe(sourcePixel(e->position()));
}

void MotionCanvas::leaveEvent(QEvent *)
{
    m_doc.probe(std::nullopt);
}

// A click on the error puts it away.
void MotionCanvas::mousePressEvent(QMouseEvent *e)
{
    if (m_errorRect.contains(e->position()))
        m_doc.clearError();
}
