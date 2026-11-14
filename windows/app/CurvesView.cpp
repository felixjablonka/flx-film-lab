#include "CurvesView.h"

#include "Theme.h"
#include "Widgets.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

QColor channelColour(int c, double alpha = 1)
{
    static const QColor colours[3] = {QColor::fromRgbF(1, 0.27f, 0.27f), QColor::fromRgbF(0.3f, 1, 0.38f),
                                      QColor::fromRgbF(0.35f, 0.52f, 1)};
    QColor colour = colours[c];
    colour.setAlphaF(float(alpha));
    return colour;
}

QColor colourOf(PrintCurves::Channel channel, double alpha = 1)
{
    if (channel == PrintCurves::RGB) {
        QColor white(255, 255, 255);
        white.setAlphaF(float(alpha));
        return white;
    }
    return channelColour(int(channel) - 1, alpha);
}

// One channel's bins as a filled outline across `rect`, scaled so `peak`
// reaches the top.
QPainterPath histogramPath(const std::vector<uint32_t> &bins, int c, uint32_t peak, const QRectF &r)
{
    QPainterPath path;
    path.moveTo(r.left(), r.bottom());
    for (int i = 0; i < 256; ++i) {
        const double v = std::min(double(bins[size_t(c) * 256 + i]) / double(std::max(peak, 1u)), 1.0);
        path.lineTo(r.left() + r.width() * i / 255.0, r.bottom() - r.height() * v);
    }
    path.lineTo(r.right(), r.bottom());
    path.closeSubpath();
    return path;
}

// The tallest bin away from the two ends: those can hold whole areas clipped
// to black or white and would flatten everything else.
uint32_t histogramPeak(const std::vector<uint32_t> &bins, std::initializer_list<int> channels)
{
    uint32_t peak = 1;
    for (int c : channels)
        for (int i = 1; i < 255; ++i)
            peak = std::max(peak, bins[size_t(c) * 256 + i]);
    return peak;
}

// Pixels at 0 or 255, as a share of the print, per channel.
std::array<double, 3> clipped(const std::vector<uint32_t> &bins, int bin)
{
    double total = 0;
    for (int i = 0; i < 256; ++i)
        total += bins[size_t(i)];
    std::array<double, 3> share{};
    for (int c = 0; c < 3; ++c)
        share[size_t(c)] = total > 0 ? bins[size_t(c) * 256 + bin] / total : 0;
    return share;
}

void roundedFrame(QPainter &p, const QRectF &r)
{
    QPainterPath frame;
    frame.addRoundedRect(r, 5, 5);
    p.fillPath(frame, QColor(0, 0, 0, 72));
    p.setClipPath(frame);
}

} // namespace

// MARK: - The histogram

HistogramView::HistogramView(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(theme::px(56));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("The print's histogram."));
}

void HistogramView::setBins(const std::vector<uint32_t> &bins)
{
    m_bins = bins.size() == 768 ? bins : std::vector<uint32_t>{};
    QString help = QStringLiteral(
        "The finished print's histogram, as its file will hold it: red, green and blue, white where they agree.");
    if (!m_bins.empty()) {
        for (const auto &[bin, end] : {std::pair(0, "black"), std::pair(255, "white")}) {
            const auto share = clipped(m_bins, bin);
            QStringList named;
            const char *names[3] = {"red", "green", "blue"};
            for (int c = 0; c < 3; ++c)
                if (share[size_t(c)] > 0.0005)
                    named << QStringLiteral("%1 %2%").arg(QLatin1StringView(names[c])).arg(share[size_t(c)] * 100, 0, 'f', 1);
            if (!named.isEmpty())
                help += QStringLiteral(" At %1: %2.").arg(QLatin1StringView(end), named.join(QStringLiteral(", ")));
        }
    }
    setToolTip(help);
    update();
}

void HistogramView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    roundedFrame(p, frame);
    if (m_bins.empty())
        return;
    const QRectF r = frame.adjusted(0, 3, 0, -3);
    const uint32_t peak = histogramPeak(m_bins, {0, 1, 2});
    p.setPen(Qt::NoPen);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int c = 0; c < 3; ++c)
        p.fillPath(histogramPath(m_bins, c, peak, r), channelColour(c, 0.72));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // over 0.05% of the print at an end is worth a mark
    for (const auto &[bin, x] : {std::pair(0, frame.left()), std::pair(255, frame.right())}) {
        const auto share = clipped(m_bins, bin);
        if (std::none_of(share.begin(), share.end(), [](double s) { return s > 0.0005; }))
            continue;
        const double d = bin == 0 ? 1 : -1;
        QPainterPath mark;
        mark.moveTo(x, frame.top());
        mark.lineTo(x + d * 9, frame.top());
        mark.lineTo(x, frame.top() + 9);
        mark.closeSubpath();
        p.fillPath(mark, QColor::fromRgbF(share[0] > 0.0005 ? 1 : 0.2f, share[1] > 0.0005 ? 1 : 0.2f,
                                          share[2] > 0.0005 ? 1 : 0.2f));
    }
}

std::vector<uint32_t> curveInput(const PrintHistogram &histogram, PrintCurves::Channel channel,
                                 const std::optional<PrintCurves> &curves)
{
    const std::vector<uint32_t> &given = histogram.input();
    if (channel == PrintCurves::RGB || !curves || PrintCurves::isStraight(curves->rgb) || given.size() != 768)
        return given;
    const CurveSpline all(curves->rgb);
    std::vector<uint32_t> out(768, 0);
    for (int i = 0; i < 256; ++i) {
        const int to = int(std::lround(all(i / 255.0) * 255));
        for (int c = 0; c < 3; ++c)
            out[size_t(c) * 256 + size_t(to)] += given[size_t(c) * 256 + size_t(i)];
    }
    return out;
}

// MARK: - The graph

class CurvesGraph : public QWidget
{
public:
    explicit CurvesGraph(CurvesEditor *editor)
        : QWidget(editor), m_editor(editor)
    {
        setFixedHeight(theme::px(176));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(false);
        setToolTip(QStringLiteral(
            "Curves on the finished print, as its file will hold it, after everything else. RGB moves all three "
            "channels; Red, Green and Blue each move one, after RGB. Click to add a point, drag it, drag it off the "
            "graph to take it away. Behind the curve is the histogram of the print it is given. The meters read the "
            "print before the curves."));
    }

    PrintHistogram histogram;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    static constexpr double kInset = 6, kGap = 0.01;   // points closer in x would make the curve all but vertical

    PrintCurves value() const { return m_editor->m_curves.value_or(PrintCurves()); }
    std::vector<CurvePoint> points() const { return PrintCurves::sanitized(value()[m_editor->m_channel]); }
    QRectF plotRect() const { return QRectF(rect()).adjusted(kInset, kInset, -kInset, -kInset); }
    QPointF location(const CurvePoint &p) const
    {
        const QRectF r = plotRect();
        return {r.left() + r.width() * p.x, r.bottom() - r.height() * p.y};
    }
    CurvePoint curvePoint(QPointF at) const
    {
        const QRectF r = plotRect();
        return {std::clamp((at.x() - r.left()) / std::max(r.width(), 1.0), 0.0, 1.0),
                std::clamp((r.bottom() - at.y()) / std::max(r.height(), 1.0), 0.0, 1.0)};
    }
    QPainterPath curvePath(const std::vector<CurvePoint> &points) const;
    void set(const std::vector<CurvePoint> &edited);
    std::optional<int> grabOrAdd(QPointF at);
    void move(int i, QPointF at);

    CurvesEditor *m_editor;
    std::optional<int> m_dragging;
    bool m_removing = false;
};

QPainterPath CurvesGraph::curvePath(const std::vector<CurvePoint> &points) const
{
    const CurveSpline spline(points);
    QPainterPath path;
    const int steps = 160;
    for (int i = 0; i <= steps; ++i) {
        const double x = double(i) / steps;
        const QPointF at = location({x, spline(x)});
        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }
    return path;
}

void CurvesGraph::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    roundedFrame(p, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    const QRectF r = plotRect();
    const PrintCurves::Channel channel = m_editor->m_channel;

    // what the curve is given
    const std::vector<uint32_t> bins = curveInput(histogram, channel, m_editor->m_curves);
    if (bins.size() == 768) {
        const bool all = channel == PrintCurves::RGB;
        const int own = int(channel) - 1;
        const uint32_t peak = all ? histogramPeak(bins, {0, 1, 2}) : histogramPeak(bins, {own});
        p.setPen(Qt::NoPen);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        for (int c = 0; c < 3; ++c)
            if (all || c == own)
                p.fillPath(histogramPath(bins, c, peak, r), channelColour(c, all ? 0.16 : 0.22));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    p.setPen(QPen(QColor(255, 255, 255, 18), 0.5));
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4, y = r.top() + r.height() * i / 4;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    QPen dashed(QColor(255, 255, 255, 46), 0.75);
    dashed.setDashPattern({3, 3});
    p.setPen(dashed);
    p.drawLine(r.bottomLeft(), r.topRight());

    // the other curves, faint: the channels' under RGB, RGB's under a channel
    const PrintCurves v = value();
    std::vector<PrintCurves::Channel> others;
    if (channel == PrintCurves::RGB)
        others = {PrintCurves::Red, PrintCurves::Green, PrintCurves::Blue};
    else
        others = {PrintCurves::RGB};
    p.setBrush(Qt::NoBrush);
    for (PrintCurves::Channel other : others)
        if (!PrintCurves::isStraight(v[other])) {
            p.setPen(QPen(colourOf(other, 0.35), 1));
            p.drawPath(curvePath(v[other]));
        }
    const std::vector<CurvePoint> pts = points();
    p.setPen(QPen(colourOf(channel, 0.95), 1.5));
    p.drawPath(curvePath(pts));

    for (size_t i = 0; i < pts.size(); ++i) {
        const QPointF c = location(pts[i]);
        const bool held = m_dragging && size_t(*m_dragging) == i;
        const QColor orange(255, 149, 0);
        p.setBrush(held ? (m_removing ? orange : colourOf(channel)) : QColor(0, 0, 0, 178));
        p.setPen(QPen(m_removing && held ? orange : colourOf(channel), 1.2));
        p.drawEllipse(c, 4, 4);
    }

    // in and out on the 0...255 scale of an 8-bit file, while a point is dragged
    if (m_dragging && size_t(*m_dragging) < pts.size()) {
        const CurvePoint &q = pts[size_t(*m_dragging)];
        p.setFont(theme::panelMono(9));
        p.setPen(m_removing ? QColor(255, 149, 0) : theme::secondary());
        p.drawText(QRectF(r.left() + 2, r.top() + 2, r.width(), 14), Qt::AlignLeft | Qt::AlignTop,
                   m_removing ? QStringLiteral("Let go to remove")
                              : QStringLiteral("%1 → %2").arg(q.x * 255, 0, 'f', 0).arg(q.y * 255, 0, 'f', 0));
    }
}

void CurvesGraph::set(const std::vector<CurvePoint> &edited)
{
    PrintCurves c = value();
    c[m_editor->m_channel] = edited;
    m_editor->m_curves = c;
    emit m_editor->curvesChanged(m_editor->m_curves);
    m_editor->refresh();
}

// The point within reach of `at`, or a new one there: on the curve if the
// click was close to it, so the curve holds until it is dragged.
std::optional<int> CurvesGraph::grabOrAdd(QPointF at)
{
    const std::vector<CurvePoint> pts = points();
    int nearest = -1;
    double best = 1e9;
    for (size_t i = 0; i < pts.size(); ++i) {
        const QPointF d = location(pts[i]) - at;
        const double distance = std::hypot(d.x(), d.y());
        if (distance < best) {
            best = distance;
            nearest = int(i);
        }
    }
    if (nearest >= 0 && best <= 9)
        return nearest;
    CurvePoint added = curvePoint(at);
    const auto after = std::find_if(pts.begin(), pts.end(), [&](const CurvePoint &q) { return q.x > added.x; });
    if (after == pts.end() || after == pts.begin())
        return std::nullopt;
    const size_t index = size_t(after - pts.begin());
    if (added.x - pts[index - 1].x < kGap || pts[index].x - added.x < kGap)
        return std::nullopt;
    const double onCurve = CurveSpline(pts)(added.x);
    if (std::abs(location({added.x, onCurve}).y() - at.y()) <= 9)
        added.y = onCurve;
    std::vector<CurvePoint> edited = pts;
    edited.insert(edited.begin() + std::ptrdiff_t(index), added);
    set(edited);
    return int(index);
}

void CurvesGraph::move(int i, QPointF at)
{
    std::vector<CurvePoint> pts = points();
    if (i < 0 || size_t(i) >= pts.size())
        return;
    const CurvePoint target = curvePoint(at);
    const double low = i == 0 ? 0 : pts[size_t(i) - 1].x + kGap;
    const double high = size_t(i) == pts.size() - 1 ? 1 : pts[size_t(i) + 1].x - kGap;
    pts[size_t(i)] = {std::clamp(target.x, low, high), target.y};
    // an inner point dragged well off the graph is taken away on release
    m_removing = i > 0 && size_t(i) < pts.size() - 1 && !plotRect().adjusted(-18, -18, 18, 18).contains(at);
    set(pts);
}

void CurvesGraph::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_dragging = grabOrAdd(e->position());
    if (m_dragging)
        move(*m_dragging, e->position());
}

void CurvesGraph::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton))
        move(*m_dragging, e->position());
}

void CurvesGraph::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    if (m_removing && m_dragging) {
        std::vector<CurvePoint> pts = points();
        if (size_t(*m_dragging) < pts.size()) {
            pts.erase(pts.begin() + *m_dragging);
            set(pts);
        }
    }
    m_dragging.reset();
    m_removing = false;
    update();
}

void CurvesGraph::contextMenuEvent(QContextMenuEvent *e)
{
    static const char *names[4] = {"RGB", "Red", "Green", "Blue"};
    QMenu menu(this);
    QAction *one = menu.addAction(QStringLiteral("Straighten %1 Curve").arg(QLatin1StringView(names[m_editor->m_channel])));
    one->setEnabled(!PrintCurves::isStraight(value()[m_editor->m_channel]));
    QAction *all = menu.addAction(QStringLiteral("Straighten All Curves"));
    all->setEnabled(!value().isIdentity());
    QAction *chosen = menu.exec(e->globalPos());
    if (chosen == one)
        m_editor->straighten(m_editor->m_channel);
    else if (chosen == all) {
        m_editor->m_curves.reset();
        emit m_editor->curvesChanged(std::nullopt);
        m_editor->refresh();
    }
}

// MARK: - The editor

CurvesEditor::CurvesEditor(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, theme::px(0), 0, 0);
    layout->setSpacing(theme::px(5));
    auto *row = new QHBoxLayout;
    row->setSpacing(theme::px(6));
    m_channels = new Segmented({QStringLiteral("RGB"), QStringLiteral("Red"), QStringLiteral("Green"),
                                QStringLiteral("Blue")});
    m_channels->setCurrent(0);
    connect(m_channels, &Segmented::chosen, this, [this](int i) {
        m_channel = PrintCurves::Channel(i);
        refresh();
    });
    row->addWidget(m_channels, 1);
    m_reset = new QToolButton;
    m_reset->setText(QStringLiteral("↺"));
    m_reset->setFont(theme::panelLabel());
    m_reset->setAutoRaise(true);
    m_reset->setToolTip(QStringLiteral("Straighten this curve again."));
    connect(m_reset, &QToolButton::clicked, this, [this] { straighten(m_channel); });
    row->addWidget(m_reset);
    layout->addLayout(row);
    m_graph = new CurvesGraph(this);
    layout->addWidget(m_graph);
    refresh();
}

void CurvesEditor::setCurves(const std::optional<PrintCurves> &curves)
{
    if (curves == m_curves)
        return;
    m_curves = curves;
    refresh();
}

void CurvesEditor::setHistogram(const PrintHistogram &histogram)
{
    m_graph->histogram = histogram;
    m_graph->update();
}

void CurvesEditor::straighten(PrintCurves::Channel channel)
{
    PrintCurves c = m_curves.value_or(PrintCurves());
    c[channel] = PrintCurves::straight();
    m_curves = c.isIdentity() ? std::nullopt : std::optional(c);
    emit curvesChanged(m_curves);
    refresh();
}

void CurvesEditor::refresh()
{
    m_reset->setEnabled(!PrintCurves::isStraight(m_curves.value_or(PrintCurves())[m_channel]));
    m_graph->update();
}
