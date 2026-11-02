#include "FrameGeometry.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Swift's rounded(): to the nearest, halves away from zero.
double nearest(double v)
{
    return std::round(v);
}

} // namespace

// MARK: - Affine

Affine Affine::then(const Affine &t) const
{
    return {a * t.a + b * t.c, a * t.b + b * t.d, c * t.a + d * t.c, c * t.b + d * t.d, tx * t.a + ty * t.c + t.tx,
            tx * t.b + ty * t.d + t.ty};
}

Affine Affine::inverted() const
{
    const double det = a * d - b * c;
    if (det == 0)
        return *this;
    return {d / det, -b / det, -c / det, a / det, (c * ty - d * tx) / det, (b * tx - a * ty) / det};
}

// MARK: - CropAspect

const std::vector<CropAspect> &cropAspects()
{
    static const std::vector<CropAspect> all{CropAspect::Original, CropAspect::ThreeTwo, CropAspect::FourThree,
                                             CropAspect::FiveFour, CropAspect::Panoramic, CropAspect::Custom,
                                             CropAspect::Free};
    return all;
}

std::string cropAspectName(CropAspect aspect)
{
    switch (aspect) {
    case CropAspect::Original: return "Original";
    case CropAspect::ThreeTwo: return "3:2";
    case CropAspect::FourThree: return "4:3";
    case CropAspect::FiveFour: return "5:4";
    case CropAspect::Panoramic: return "65:24";
    case CropAspect::Custom: return "Custom";
    case CropAspect::Free: return "Free";
    }
    return "Original";
}

CropAspect cropAspectNamed(const std::string &name)
{
    for (CropAspect a : cropAspects())
        if (cropAspectName(a) == name)
            return a;
    return CropAspect::Original;
}

namespace {

// long side over short, where the preset fixes it
std::optional<double> longOverShort(CropAspect a)
{
    switch (a) {
    case CropAspect::ThreeTwo: return 3.0 / 2.0;
    case CropAspect::FourThree: return 4.0 / 3.0;
    case CropAspect::FiveFour: return 5.0 / 4.0;
    case CropAspect::Panoramic: return 65.0 / 24.0;
    default: return std::nullopt;
    }
}

} // namespace

// MARK: - FrameGeometry

PointD FrameGeometry::turnedSize(double width, double height) const
{
    return quarterTurns % 2 == 0 ? PointD{width, height} : PointD{height, width};
}

// Width over height of the crop, in pixels, or none for a free crop.
std::optional<double> FrameGeometry::ratio(PointD frame) const
{
    double longSide = 1;
    switch (aspect) {
    case CropAspect::Free: return std::nullopt;
    case CropAspect::Original:
        longSide = std::max(frame.x, frame.y) / std::max(std::min(frame.x, frame.y), 1.0);
        break;
    case CropAspect::Custom: return customWidth / std::max(customHeight, 1e-6);
    default: longSide = longOverShort(aspect).value_or(1);
    }
    const bool upright = portrait.value_or(frame.y > frame.x);
    return upright ? 1 / longSide : longSide;
}

bool FrameGeometry::isPortrait(PointD frame) const
{
    if (aspect == CropAspect::Custom)
        return customHeight > customWidth;
    if (portrait)
        return *portrait;
    if (crop)
        return crop->h * frame.y > crop->w * frame.x;
    return frame.y > frame.x;
}

RectD FrameGeometry::cropRect(PointD frame) const
{
    if (const auto drawn = drawnCrop(frame))
        return fitted(*drawn, frame);
    const auto r = ratio(frame);
    if (!r) {
        if (straighten == 0)
            return {0, 0, frame.x, frame.y};
        return largestCentred(frame.x / std::max(frame.y, 1.0), frame, straighten);
    }
    return largestCentred(*r, frame, straighten);
}

// The largest crop of width-over-height `ratio`, centred, inside a frame
// levelled by `degrees` and inside the frame.
RectD FrameGeometry::largestCentred(double r, PointD frame, double degrees)
{
    const double theta = std::abs(degrees) * kPi / 180;
    const double a = frame.x / 2, b = frame.y / 2;
    const double halfHeight = std::min({a / (r * std::cos(theta) + std::sin(theta)),
                                        b / (r * std::sin(theta) + std::cos(theta)), a / r, b});
    const double halfWidth = r * halfHeight;
    return {a - halfWidth, b - halfHeight, 2 * halfWidth, 2 * halfHeight};
}

std::optional<RectD> FrameGeometry::drawnCrop(PointD frame) const
{
    if (!crop)
        return std::nullopt;
    return RectD{crop->x * frame.x, crop->y * frame.y, crop->w * frame.x, crop->h * frame.y};
}

// 1/4096 of the long edge, about half a pixel of the preview the crop is
// drawn on, so a crop fits the same at every size.
double FrameGeometry::tolerance(PointD frame)
{
    return std::max(frame.x, frame.y) / 4096;
}

bool FrameGeometry::contains(const RectD &rect, PointD frame) const
{
    const double t = tolerance(frame);
    if (!(rect.w > 0 && rect.h > 0 && rect.minX() >= -t && rect.minY() >= -t && rect.maxX() <= frame.x + t
          && rect.maxY() <= frame.y + t))
        return false;
    const double theta = straighten * kPi / 180;
    const double c = std::cos(theta), s = std::sin(theta);
    const double cx = frame.x / 2, cy = frame.y / 2;
    const double a = cx + t, b = cy + t;
    for (const PointD corner : {PointD{rect.minX(), rect.minY()}, PointD{rect.maxX(), rect.minY()},
                                PointD{rect.minX(), rect.maxY()}, PointD{rect.maxX(), rect.maxY()}}) {
        const double x = corner.x - cx, y = corner.y - cy;
        // back into the unlevelled frame
        const double u = c * x + s * y, v = -s * x + c * y;
        if (std::abs(u) > a || std::abs(v) > b)
            return false;
    }
    return true;
}

// `rect` shrunk about its centre until it lies inside the picture, or the
// largest centred crop of its proportions if its centre does not.
RectD FrameGeometry::fitted(const RectD &rect, PointD frame) const
{
    if (contains(rect, frame))
        return rect;
    const PointD centre{rect.midX(), rect.midY()};
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        if (contains(rect.scaled(mid, centre), frame))
            lo = mid;
        else
            hi = mid;
    }
    if (lo > 0.05)
        return rect.scaled(lo, centre);
    return largestCentred(rect.w / std::max(rect.h, 1e-6), frame, straighten);
}

void FrameGeometry::setCrop(const RectD &rect, PointD frame)
{
    const double t = tolerance(frame);
    if (straighten == 0 && std::abs(rect.minX()) < t && std::abs(rect.minY()) < t
        && std::abs(rect.maxX() - frame.x) < t && std::abs(rect.maxY() - frame.y) < t) {
        crop.reset();
        return;
    }
    // on a grid of 2^-40 of the frame, where 1 - x is exact: turned and
    // mirrored any number of times, the crop comes back as it was
    auto snap = [](double v) { return nearest(v * 0x1p40) / 0x1p40; };
    const double x0 = snap(rect.minX() / frame.x), y0 = snap(rect.minY() / frame.y);
    const double x1 = snap(rect.maxX() / frame.x), y1 = snap(rect.maxY() / frame.y);
    crop = RectD{x0, y0, x1 - x0, y1 - y0};
}

// A drawn crop given the current proportions: as large as before and where
// it was, inside the picture.
void FrameGeometry::refit(PointD frame)
{
    const auto r = ratio(frame);
    if (!crop || !r)
        return;
    const RectD current = cropRect(frame);
    const double area = current.w * current.h;
    const double width = std::sqrt(area * *r), height = std::sqrt(area / *r);
    RectD rect{current.midX() - width / 2, current.midY() - height / 2, width, height};
    const RectD largest = largestCentred(*r, frame, straighten);
    if (rect.w > largest.w)
        rect = rect.scaled(largest.w / rect.w, {rect.midX(), rect.midY()});
    setCrop(fitted(rect, frame), frame);
}

void FrameGeometry::setAspect(CropAspect newAspect, PointD frame)
{
    aspect = newAspect;
    refit(frame);
}

void FrameGeometry::swapOrientation(PointD frame)
{
    if (aspect == CropAspect::Custom)
        std::swap(customWidth, customHeight);
    else
        portrait = !isPortrait(frame);
    if (aspect == CropAspect::Free && crop) {
        // a free crop keeps its shape, turned the other way about its centre
        const RectD rect = cropRect(frame);
        const RectD turned{rect.midX() - rect.h / 2, rect.midY() - rect.w / 2, rect.h, rect.w};
        setCrop(fitted(turned, frame), frame);
    } else {
        refit(frame);
    }
}

void FrameGeometry::setStraighten(double degrees)
{
    straighten = std::clamp(degrees, -maxStraighten, maxStraighten);
}

void FrameGeometry::turn(bool clockwise)
{
    quarterTurns = (quarterTurns + (clockwise ? 1 : 3)) % 4;
    if (crop) {
        const RectD c = *crop;
        crop = clockwise ? RectD{1 - c.maxY(), c.minX(), c.h, c.w} : RectD{c.minY(), 1 - c.maxX(), c.h, c.w};
    }
    if (portrait)
        portrait = !*portrait;
    std::swap(customWidth, customHeight);
}

void FrameGeometry::flip(bool horizontal)
{
    // the turns come after the mirror: a mirror of the turned frame is the
    // source's mirror with the turns reversed; top to bottom, and a half turn
    quarterTurns = horizontal ? (4 - quarterTurns) % 4 : (6 - quarterTurns) % 4;
    mirrored = !mirrored;
    straighten = -straighten;
    if (crop) {
        const RectD c = *crop;
        crop = horizontal ? RectD{1 - c.maxX(), c.minY(), c.w, c.h} : RectD{c.minX(), 1 - c.maxY(), c.w, c.h};
    }
}

FrameGeometry FrameGeometry::upright() const
{
    FrameGeometry g = *this;
    while (g.quarterTurns % 4 != 0)
        g.turn(false);
    if (g.mirrored)
        g.flip(true);
    return g;
}

// MARK: - JSON

QJsonObject frameGeometryToJson(const FrameGeometry &g)
{
    QJsonObject o{{"quarterTurns", g.quarterTurns},
                  {"mirrored", g.mirrored},
                  {"straighten", g.straighten},
                  {"aspect", QString::fromStdString(cropAspectName(g.aspect))},
                  {"customWidth", g.customWidth},
                  {"customHeight", g.customHeight}};
    // CGRect's Codable form: [[x, y], [width, height]]
    if (g.crop)
        o.insert("crop", QJsonArray{QJsonArray{g.crop->x, g.crop->y}, QJsonArray{g.crop->w, g.crop->h}});
    if (g.portrait)
        o.insert("portrait", *g.portrait);
    return o;
}

FrameGeometry frameGeometryFromJson(const QJsonObject &o)
{
    FrameGeometry g;
    g.quarterTurns = ((o.value("quarterTurns").toInt() % 4) + 4) % 4;
    g.mirrored = o.value("mirrored").toBool();
    g.straighten = o.value("straighten").toDouble();
    g.aspect = cropAspectNamed(o.value("aspect").toString().toStdString());
    g.customWidth = o.value("customWidth").toDouble(1);
    g.customHeight = o.value("customHeight").toDouble(1);
    const QJsonArray crop = o.value("crop").toArray();
    if (crop.size() == 2)
        g.crop = RectD{crop[0].toArray()[0].toDouble(), crop[0].toArray()[1].toDouble(),
                       crop[1].toArray()[0].toDouble(), crop[1].toArray()[1].toDouble()};
    if (o.value("portrait").isBool())
        g.portrait = o.value("portrait").toBool();
    return g;
}

// MARK: - GeometryMap

GeometryMap::GeometryMap(const FrameGeometry &g, int width, int height, bool wholeFrame)
{
    const double w = width, h = height;
    sourceWidth = w;
    sourceHeight = h;
    Affine orient;
    double fw = w, fh = h;
    if (g.mirrored)
        orient = orient.then({-1, 0, 0, 1, fw, 0});
    for (int i = 0; i < ((g.quarterTurns % 4 + 4) % 4); ++i) {
        // a clockwise quarter turn: (x, y) in a w x h frame to (h - y, x)
        orient = orient.then({0, 1, -1, 0, fh, 0});
        std::swap(fw, fh);
    }
    frame = {fw, fh};
    const double theta = g.straighten * kPi / 180;
    const double c = std::cos(theta), s = std::sin(theta);
    const double cx = fw / 2, cy = fh / 2;
    // clockwise on screen, y down, about the frame's centre
    const Affine level{c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy};
    const RectD rect = wholeFrame ? RectD{0, 0, fw, fh} : g.cropRect(frame);
    // to whole pixels: inwards when levelled, so none reach past the picture
    // into its empty corners
    const bool inwards = !wholeFrame && g.straighten != 0;
    double x0 = std::max(inwards ? std::ceil(rect.minX() - 1e-6) : nearest(rect.minX()), 0.0);
    double y0 = std::max(inwards ? std::ceil(rect.minY() - 1e-6) : nearest(rect.minY()), 0.0);
    double x1 = std::min(inwards ? std::floor(rect.maxX() + 1e-6) : nearest(rect.maxX()), fw);
    double y1 = std::min(inwards ? std::floor(rect.maxY() + 1e-6) : nearest(rect.maxY()), fh);
    const bool whole = x0 == 0 && y0 == 0 && x1 == fw && y1 == fh;
    if (!wholeFrame && !whole) {
        if (const auto r = g.ratio(frame)) {
            // the crop is drawn on the preview, whose sides are rounded: at
            // this size its proportions are worked out again in whole pixels
            const double cw = x1 - x0, ch = y1 - y0;
            if (cw > nearest(ch * *r)) {
                const double narrower = std::max(nearest(ch * *r), 1.0);
                x0 += std::floor((cw - narrower) / 2);
                x1 = x0 + narrower;
            } else if (ch > nearest(cw / *r)) {
                const double shorter = std::max(nearest(cw / *r), 1.0);
                y0 += std::floor((ch - shorter) / 2);
                y1 = y0 + shorter;
            }
        }
    }
    crop = {x0, y0, std::max(x1 - x0, 1.0), std::max(y1 - y0, 1.0)};
    sourceToOutput = orient.then(level).then({1, 0, 0, 1, -x0, -y0});
    outputToSource = sourceToOutput.inverted();
    isIdentity = g.quarterTurns % 4 == 0 && !g.mirrored && g.straighten == 0 && crop == RectD{0, 0, w, h};
}

PointD GeometryMap::outputPoint(PointD p) const
{
    return sourceToOutput.apply({p.x * sourceWidth, p.y * sourceHeight});
}

PointD GeometryMap::normalisedSource(PointD p) const
{
    const PointD s = outputToSource.apply(p);
    return {s.x / std::max(sourceWidth, 1.0), s.y / std::max(sourceHeight, 1.0)};
}

std::vector<float> GeometryMap::resample(const std::vector<float> &pixels, int maxLongEdge, int &ow, int &oh) const
{
    const int sw = int(sourceWidth), sh = int(sourceHeight);
    const double scale = std::min(1.0, double(maxLongEdge) / double(std::max({outputWidth(), outputHeight(), 1})));
    ow = std::max(int(nearest(outputWidth() * scale)), 1);
    oh = std::max(int(nearest(outputHeight() * scale)), 1);
    std::vector<float> out(size_t(ow) * oh * 4, 0.0f);
    if (pixels.size() < size_t(sw) * sh * 4)
        return out;
    const Affine &t = outputToSource;
    const int left = int(t.tx), top = int(t.ty);
    if (scale == 1 && t.a == 1 && t.b == 0 && t.c == 0 && t.d == 1 && double(left) == t.tx && double(top) == t.ty
        && left >= 0 && top >= 0 && left + ow <= sw && top + oh <= sh) {
        for (int y = 0; y < oh; ++y)
            std::copy_n(&pixels[(size_t(top + y) * sw + left) * 4], size_t(ow) * 4, &out[size_t(y) * ow * 4]);
        return out;
    }
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < ow; ++x) {
                const double ox = (x + 0.5) / scale, oy = (y + 0.5) / scale;
                const double sx = t.a * ox + t.c * oy + t.tx - 0.5, sy = t.b * ox + t.d * oy + t.ty - 0.5;
                const double fx = std::floor(sx), fy = std::floor(sy);
                const float ax = float(sx - fx), ay = float(sy - fy);
                const int x0 = std::clamp(int(fx), 0, sw - 1), yy0 = std::clamp(int(fy), 0, sh - 1);
                const int x1 = std::clamp(int(fx) + 1, 0, sw - 1), yy1 = std::clamp(int(fy) + 1, 0, sh - 1);
                const size_t o = (size_t(y) * ow + x) * 4;
                for (int ch = 0; ch < 4; ++ch) {
                    const float p00 = pixels[(size_t(yy0) * sw + x0) * 4 + ch];
                    const float p10 = pixels[(size_t(yy0) * sw + x1) * 4 + ch];
                    const float p01 = pixels[(size_t(yy1) * sw + x0) * 4 + ch];
                    const float p11 = pixels[(size_t(yy1) * sw + x1) * 4 + ch];
                    const float topRow = p00 + (p10 - p00) * ax, bottomRow = p01 + (p11 - p01) * ax;
                    out[o + ch] = topRow + (bottomRow - topRow) * ay;
                }
            }
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
        pool.emplace_back(rows, oh * i / threads, oh * (i + 1) / threads);
    for (std::thread &th : pool)
        th.join();
    return out;
}
