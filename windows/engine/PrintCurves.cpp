#include "PrintCurves.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

std::vector<CurvePoint> PrintCurves::sanitized(const std::vector<CurvePoint> &points)
{
    std::vector<CurvePoint> sorted = points;
    std::stable_sort(sorted.begin(), sorted.end(), [](const CurvePoint &a, const CurvePoint &b) { return a.x < b.x; });
    std::vector<CurvePoint> out;
    for (const CurvePoint &p : sorted) {
        const CurvePoint q{std::min(std::max(p.x, 0.0), 1.0), std::min(std::max(p.y, 0.0), 1.0)};
        if (!std::isfinite(q.x) || !std::isfinite(q.y))
            continue;
        if (!out.empty() && q.x - out.back().x < 1e-6)
            out.back() = q;
        else
            out.push_back(q);
    }
    return out.size() >= 2 ? out : straight();
}

bool PrintCurves::isStraight(const std::vector<CurvePoint> &points)
{
    const std::vector<CurvePoint> p = sanitized(points);
    if (p.front().x != 0 || p.back().x != 1)
        return false;
    return std::all_of(p.begin(), p.end(), [](const CurvePoint &q) { return std::abs(q.y - q.x) < 1e-9; });
}

std::vector<CurvePoint> &PrintCurves::operator[](Channel c)
{
    return c == RGB ? rgb : (c == Red ? red : (c == Green ? green : blue));
}

const std::vector<CurvePoint> &PrintCurves::operator[](Channel c) const
{
    return c == RGB ? rgb : (c == Red ? red : (c == Green ? green : blue));
}

bool PrintCurves::isIdentity() const
{
    return isStraight(rgb) && isStraight(red) && isStraight(green) && isStraight(blue);
}

std::vector<float> PrintCurves::table() const
{
    const CurveSpline all(rgb);
    const CurveSpline own[3] = {CurveSpline(red), CurveSpline(green), CurveSpline(blue)};
    std::vector<float> out(3 * tableSize);
    for (int i = 0; i < tableSize; i++) {
        const double v = all(double(i) / double(tableSize - 1));
        for (int c = 0; c < 3; c++)
            out[size_t(c) * tableSize + i] = float(own[c](v));
    }
    return out;
}

// The one-sided three-point slope at an end, held to the shape of the points
// (SciPy's PCHIP end condition).
static double endSlope(double h0, double h1, double d0, double d1)
{
    const double m = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (m * d0 <= 0)
        return 0;
    if (d0 * d1 < 0 && std::abs(m) > std::abs(3 * d0))
        return 3 * d0;
    return m;
}

CurveSpline::CurveSpline(const std::vector<CurvePoint> &points)
{
    for (const CurvePoint &p : PrintCurves::sanitized(points)) {
        m_xs.push_back(p.x);
        m_ys.push_back(p.y);
    }
    const size_t n = m_xs.size();
    std::vector<double> h(n - 1), d(n - 1);
    for (size_t k = 0; k + 1 < n; k++) {
        h[k] = m_xs[k + 1] - m_xs[k];
        d[k] = (m_ys[k + 1] - m_ys[k]) / h[k];
    }
    m_slopes.assign(n, 0);
    if (n == 2) {
        m_slopes = {d[0], d[0]};
        return;
    }
    // Inside: the weighted harmonic mean of the secants on either side, or
    // flat where they disagree in sign (a peak or a dip).
    for (size_t k = 1; k + 1 < n; k++) {
        if (d[k - 1] * d[k] > 0) {
            const double w1 = 2 * h[k] + h[k - 1], w2 = h[k] + 2 * h[k - 1];
            m_slopes[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
        }
    }
    m_slopes[0] = endSlope(h[0], h[1], d[0], d[1]);
    m_slopes[n - 1] = endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
}

double CurveSpline::operator()(double x) const
{
    if (x <= m_xs.front())
        return m_ys.front();
    if (x >= m_xs.back())
        return m_ys.back();
    size_t k = 0;
    while (k < m_xs.size() - 2 && x >= m_xs[k + 1])
        k++;
    const double h = m_xs[k + 1] - m_xs[k], t = (x - m_xs[k]) / h;
    const double t2 = t * t, t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * m_ys[k] + (t3 - 2 * t2 + t) * h * m_slopes[k]
        + (-2 * t3 + 3 * t2) * m_ys[k + 1] + (t3 - t2) * h * m_slopes[k + 1];
    return std::min(std::max(y, 0.0), 1.0);
}

static std::vector<CurvePoint> pointsFromJson(const QJsonValue &v)
{
    if (!v.isArray())
        return PrintCurves::straight();
    std::vector<CurvePoint> points;
    for (const QJsonValue &p : v.toArray()) {
        const QJsonObject o = p.toObject();
        points.push_back({o.value("x").toDouble(), o.value("y").toDouble()});
    }
    return points;
}

static QJsonArray pointsToJson(const std::vector<CurvePoint> &points)
{
    QJsonArray a;
    for (const CurvePoint &p : points)
        a.append(QJsonObject{{"x", p.x}, {"y", p.y}});
    return a;
}

PrintCurves printCurvesFromJson(const QJsonObject &o)
{
    PrintCurves c;
    c.rgb = pointsFromJson(o.value("rgb"));
    c.red = pointsFromJson(o.value("red"));
    c.green = pointsFromJson(o.value("green"));
    c.blue = pointsFromJson(o.value("blue"));
    return c;
}

QJsonObject printCurvesToJson(const PrintCurves &c)
{
    return {{"rgb", pointsToJson(c.rgb)}, {"red", pointsToJson(c.red)},
            {"green", pointsToJson(c.green)}, {"blue", pointsToJson(c.blue)}};
}
