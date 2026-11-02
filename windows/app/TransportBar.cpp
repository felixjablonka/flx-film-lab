#include "TransportBar.h"

#include "MotionDocument.h"
#include "Theme.h"
#include "Widgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace {

enum class Glyph { Back, Play, Pause, Forward };

// The transport's symbols (backward.frame.fill, play.fill, pause.fill,
// forward.frame.fill), drawn in the text's colour.
QIcon icon(Glyph glyph, qreal dpr)
{
    const int side = 14;
    QPixmap pixmap(QSize(side, side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::secondary());
    auto triangle = [&](double left, double right, bool pointsRight) {
        QPainterPath path;
        if (pointsRight) {
            path.moveTo(left, 2);
            path.lineTo(right, 7);
            path.lineTo(left, 12);
        } else {
            path.moveTo(right, 2);
            path.lineTo(left, 7);
            path.lineTo(right, 12);
        }
        path.closeSubpath();
        p.drawPath(path);
    };
    switch (glyph) {
    case Glyph::Back:
        p.drawRect(QRectF(2, 2, 2, 10));
        triangle(5, 12, false);
        break;
    case Glyph::Play: triangle(3, 12, true); break;
    case Glyph::Pause:
        p.drawRect(QRectF(3, 2, 3, 10));
        p.drawRect(QRectF(8, 2, 3, 10));
        break;
    case Glyph::Forward:
        triangle(2, 9, true);
        p.drawRect(QRectF(10, 2, 2, 10));
        break;
    }
    return QIcon(pixmap);
}

} // namespace

TransportBar::TransportBar(MotionDocument &document, QWidget *parent)
    : QWidget(parent), m_doc(document)
{
    setAttribute(Qt::WA_StyledBackground);
    setObjectName(QStringLiteral("transport"));
    setStyleSheet(QStringLiteral("#transport { background: #262626; border-top: 1px solid black; }"
                                 "QToolButton { border: none; padding: 2px; border-radius: 4px; }"
                                 "QToolButton:hover { background: #3a3a3a; }"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 7, 14, 7);
    layout->setSpacing(10);
    auto button = [this](Glyph glyph, const QString &help) {
        auto *b = new QToolButton;
        b->setIcon(icon(glyph, devicePixelRatioF()));
        b->setIconSize(QSize(14, 14));
        b->setToolTip(help);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };
    m_back = button(Glyph::Back, QStringLiteral("Back one frame (←)"));
    m_play = button(Glyph::Play, QStringLiteral("Play or pause (Space)"));
    m_forward = button(Glyph::Forward, QStringLiteral("Forward one frame (→)"));
    connect(m_back, &QToolButton::clicked, this, [this] { m_doc.step(-1); });
    connect(m_play, &QToolButton::clicked, this, [this] { m_doc.togglePlay(); });
    connect(m_forward, &QToolButton::clicked, this, [this] { m_doc.step(1); });

    auto label = [](Qt::Alignment align, const QColor &ink) {
        auto *l = new QLabel;
        l->setFont(theme::mono(11));
        l->setFixedWidth(74);
        l->setAlignment(align | Qt::AlignVCenter);
        QPalette p = l->palette();
        p.setColor(QPalette::WindowText, ink);
        l->setPalette(p);
        return l;
    };
    m_time = label(Qt::AlignRight, theme::secondary());
    m_duration = label(Qt::AlignLeft, theme::tertiary());
    m_scrub = new ValueSlider;
    connect(m_scrub, &ValueSlider::moved, this, [this](double t) { m_doc.seek(t); });

    layout->addWidget(m_back);
    layout->addWidget(m_play);
    layout->addWidget(m_forward);
    layout->addWidget(m_time);
    layout->addWidget(m_scrub, 1);
    layout->addWidget(m_duration);

    connect(&m_doc, &MotionDocument::changed, this, &TransportBar::refresh);
    connect(&m_doc, &MotionDocument::statusChanged, this, &TransportBar::refresh);
    connect(&m_doc, &MotionDocument::timeChanged, this, &TransportBar::refresh);
    refresh();
}

void TransportBar::refresh()
{
    const double duration = m_doc.duration();
    const bool clip = duration > 0;
    for (QWidget *w : {static_cast<QWidget *>(m_back), static_cast<QWidget *>(m_play),
                       static_cast<QWidget *>(m_forward), static_cast<QWidget *>(m_scrub)})
        w->setEnabled(clip);
    if (m_doc.isPlaying() != m_showsPause) {
        m_showsPause = m_doc.isPlaying();
        m_play->setIcon(icon(m_showsPause ? Glyph::Pause : Glyph::Play, devicePixelRatioF()));
    }
    m_scrub->setRange(0, std::max(duration, 0.001));
    m_scrub->setValue(m_doc.time());
    m_time->setText(timecode(m_doc.time()));
    m_duration->setText(timecode(duration));
}

QString TransportBar::timecode(double seconds) const
{
    const double fps = m_doc.frameRate() > 0 ? m_doc.frameRate() : 25;
    const int total = int(std::floor(std::max(seconds, 0.0) * fps));
    const int perSecond = std::max(int(std::lround(fps)), 1);
    const int frames = total % perSecond, whole = total / perSecond;
    return QString::asprintf("%02d:%02d:%02d", whole / 60, whole % 60, frames);
}
