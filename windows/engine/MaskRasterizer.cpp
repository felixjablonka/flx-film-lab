#include "MaskRasterizer.h"

#include <algorithm>
#include <cmath>

namespace maskraster {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Catmull-Rom through the control points: the outline bows out past its own
// vertices, as a cut card has no sharp corners either.
std::vector<PointD> smoothClosedPolyline(const std::vector<PointD> &points, int samplesPerSegment = 12)
{
    const size_t n = points.size();
    if (n < 3)
        return points;
    std::vector<PointD> out;
    out.reserve(n * size_t(samplesPerSegment));
    for (size_t i = 0; i < n; ++i) {
        const PointD p0 = points[(i + n - 1) % n], p1 = points[i], p2 = points[(i + 1) % n], p3 = points[(i + 2) % n];
        for (int s = 0; s < samplesPerSegment; ++s) {
            const double t = double(s) / samplesPerSegment, t2 = t * t, t3 = t2 * t;
            const double x = 0.5 * ((2 * p1.x) + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2
                                    + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
            const double y = 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2
                                    + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
            out.push_back({x, y});
        }
    }
    return out;
}

// Three points, the centre and two axis ends, make an affine frame, so a
// tilted or sheared oval needs nothing more.
std::vector<PointD> ovalOutline(const std::vector<PointD> &points, int samples = 64)
{
    const PointD c = points[0];
    const PointD u{points[1].x - c.x, points[1].y - c.y}, v{points[2].x - c.x, points[2].y - c.y};
    std::vector<PointD> out;
    for (int i = 0; i < samples; ++i) {
        const double theta = 2 * kPi * i / samples;
        out.push_back({c.x + u.x * std::cos(theta) + v.x * std::sin(theta),
                       c.y + u.y * std::cos(theta) + v.y * std::sin(theta)});
    }
    return out;
}

// The graduated card edge: full effect at and behind `a`, none at and past `b`.
void fillGradient(std::vector<float> &alpha, PointD a, PointD b, int w, int h)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1e-9)
        return;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double px = x + 0.5 - a.x, py = y + 0.5 - a.y;
            const double t = std::clamp((px * dx + py * dy) / lengthSquared, 0.0, 1.0);
            alpha[size_t(y) * w + x] = float(1 - t * t * (3 - 2 * t));
        }
    }
}

// Even-odd scanline fill.
void fillPolygon(std::vector<float> &alpha, const std::vector<PointD> &outline, int w, int h)
{
    const size_t n = outline.size();
    if (n < 3)
        return;
    std::vector<double> crossings;
    for (int y = 0; y < h; ++y) {
        const double scan = y + 0.5;
        crossings.clear();
        size_t j = n - 1;
        for (size_t i = 0; i < n; ++i) {
            const PointD a = outline[j], b = outline[i];
            if ((a.y > scan) != (b.y > scan))
                crossings.push_back(a.x + (scan - a.y) / (b.y - a.y) * (b.x - a.x));
            j = i;
        }
        if (crossings.size() < 2)
            continue;
        std::sort(crossings.begin(), crossings.end());
        for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
            const int x0 = std::max(int(std::round(crossings[k])), 0);
            const int x1 = std::min(int(std::round(crossings[k + 1])), w);
            for (int x = x0; x < x1; ++x)
                alpha[size_t(y) * w + x] = 1;
        }
    }
}

void boxPassHorizontal(const std::vector<float> &src, std::vector<float> &dst, int w, int h, int radius)
{
    const float norm = 1.0f / float(2 * radius + 1);
    for (int y = 0; y < h; ++y) {
        const size_t row = size_t(y) * w;
        float sum = 0;
        for (int i = -radius; i <= radius; ++i)
            sum += src[row + size_t(std::clamp(i, 0, w - 1))];
        for (int x = 0; x < w; ++x) {
            dst[row + x] = sum * norm;
            sum -= src[row + size_t(std::clamp(x - radius, 0, w - 1))];
            sum += src[row + size_t(std::clamp(x + radius + 1, 0, w - 1))];
        }
    }
}

// row by row, one running sum per column: the same filter as a column walk,
// reading memory in order
void boxPassVertical(const std::vector<float> &src, std::vector<float> &dst, int w, int h, int radius)
{
    const float norm = 1.0f / float(2 * radius + 1);
    std::vector<float> sum(size_t(w), 0.0f);
    for (int i = -radius; i <= radius; ++i) {
        const size_t row = size_t(std::clamp(i, 0, h - 1)) * w;
        for (int x = 0; x < w; ++x)
            sum[size_t(x)] += src[row + x];
    }
    for (int y = 0; y < h; ++y) {
        const size_t out = size_t(y) * w;
        for (int x = 0; x < w; ++x)
            dst[out + x] = sum[size_t(x)] * norm;
        const size_t leaving = size_t(std::clamp(y - radius, 0, h - 1)) * w;
        const size_t entering = size_t(std::clamp(y + radius + 1, 0, h - 1)) * w;
        for (int x = 0; x < w; ++x)
            sum[size_t(x)] += src[entering + x] - src[leaving + x];
    }
}

// Three box passes, close enough to a Gaussian for a feather, in time
// independent of the radius.
void boxBlur(std::vector<float> &data, int w, int h, float sigma)
{
    if (sigma <= 0.5f)
        return;
    const int radius = std::max(int(std::round(sigma * 1.2f)), 1);
    std::vector<float> scratch(data.size(), 0.0f);
    for (int i = 0; i < 3; ++i) {
        boxPassHorizontal(data, scratch, w, h, radius);
        boxPassVertical(scratch, data, w, h, radius);
    }
}

} // namespace

Size mapSize(int width, int height)
{
    const int longEdge = std::max(width, height);
    if (longEdge <= kMapMaxDimension)
        return {std::max(width, 1), std::max(height, 1)};
    const double scale = double(kMapMaxDimension) / longEdge;
    return {std::max(int(width * scale), 1), std::max(int(height * scale), 1)};
}

bool isActive(const LocalMask &mask)
{
    return mask.enabled && (mask.stops != 0 || mask.grade != 0);
}

std::vector<float> coverage(const LocalMask &mask, int w, int h)
{
    std::vector<float> alpha(size_t(w) * h, 0.0f);
    std::vector<PointD> points;
    for (const PointD &p : mask.points)
        points.push_back({p.x * w, p.y * h});
    switch (mask.shape) {
    case MaskShape::Polygon:
        if (points.size() >= 3)
            fillPolygon(alpha, smoothClosedPolyline(points), w, h);
        break;
    case MaskShape::Oval:
        if (points.size() >= 3)
            fillPolygon(alpha, ovalOutline(points), w, h);
        break;
    case MaskShape::Gradient:
        if (points.size() >= 2)
            fillGradient(alpha, points[0], points[1], w, h);
        break;
    }
    // a gradient's handle spacing is its softness: feather does not apply
    if (mask.shape != MaskShape::Gradient && mask.feather > 0)
        boxBlur(alpha, w, h, float(mask.feather) * float(std::min(w, h)));
    if (mask.inverted)
        for (float &a : alpha)
            a = 1 - a;
    return alpha;
}

std::vector<float> combine(const std::vector<std::pair<const LocalMask *, const std::vector<float> *>> &coverages,
                           int w, int h)
{
    std::vector<float> out(size_t(w) * h * 4, 0.0f);
    for (const auto &[mask, alpha] : coverages) {
        if (alpha->size() != size_t(w) * h)
            continue;
        const float stops = float(mask->stops), grade = float(mask->grade);
        for (size_t i = 0; i < alpha->size(); ++i) {
            out[i * 4 + 0] += (*alpha)[i] * stops;
            out[i * 4 + 1] += (*alpha)[i] * grade;
        }
    }
    return out;
}

} // namespace maskraster
