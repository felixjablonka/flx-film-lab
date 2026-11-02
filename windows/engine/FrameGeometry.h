#pragma once

#include <QJsonObject>

#include <optional>
#include <string>
#include <vector>

// The print's shape (FrameGeometry.swift): how the frame is turned, levelled
// and cropped, and the map from the print's pixels back to the frame's.

struct PointD {
    double x = 0, y = 0;
    bool operator==(const PointD &) const = default;
};

struct RectD {
    double x = 0, y = 0, w = 0, h = 0;
    double minX() const { return x; }
    double minY() const { return y; }
    double maxX() const { return x + w; }
    double maxY() const { return y + h; }
    double midX() const { return x + w / 2; }
    double midY() const { return y + h / 2; }
    bool contains(PointD p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    RectD offset(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
    // CGRect.scaled(by:about:)
    RectD scaled(double factor, PointD c) const
    {
        return {c.x - (c.x - x) * factor, c.y - (c.y - y) * factor, w * factor, h * factor};
    }
    bool operator==(const RectD &) const = default;
};

// CGAffineTransform: (x, y) to (a x + c y + tx, b x + d y + ty).
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    PointD apply(PointD p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    // this, then `then` (CGAffineTransform.concatenating)
    Affine then(const Affine &t) const;
    Affine inverted() const;
};

// The proportions a crop keeps.
enum class CropAspect { Original, ThreeTwo, FourThree, FiveFour, Panoramic, Custom, Free };
const std::vector<CropAspect> &cropAspects();
std::string cropAspectName(CropAspect aspect);     // its raw value: "Original", "3:2", ...
CropAspect cropAspectNamed(const std::string &name);

struct FrameGeometry {
    int quarterTurns = 0;           // clockwise, after the mirror
    bool mirrored = false;          // left to right, before the turns
    double straighten = 0;          // degrees, positive clockwise on screen, within ±45
    // The crop as drawn, as fractions of the turned frame; none is the
    // largest crop of `aspect` that fits, centred.
    std::optional<RectD> crop;
    CropAspect aspect = CropAspect::Original;
    std::optional<bool> portrait;   // stood upright; none follows the frame
    double customWidth = 1, customHeight = 1;

    static constexpr double maxStraighten = 45;
    bool operator==(const FrameGeometry &) const = default;
    bool isIdentity() const { return *this == FrameGeometry(); }

    PointD turnedSize(double width, double height) const;
    std::optional<double> ratio(PointD frame) const;
    bool isPortrait(PointD frame) const;
    RectD cropRect(PointD frame) const;
    static RectD largestCentred(double ratio, PointD frame, double degrees);
    std::optional<RectD> drawnCrop(PointD frame) const;
    static double tolerance(PointD frame);
    bool contains(const RectD &rect, PointD frame) const;
    RectD fitted(const RectD &rect, PointD frame) const;
    void setCrop(const RectD &rect, PointD frame);
    void refit(PointD frame);
    void setAspect(CropAspect aspect, PointD frame);
    void swapOrientation(PointD frame);
    void setStraighten(double degrees);
    void turn(bool clockwise);
    void flip(bool horizontal);
    // The same crop with the turns and the mirror undone, so that turning or
    // flipping a print never changes how the meters read it.
    FrameGeometry upright() const;
};

// The Mac's Codable form: every key, the crop and portrait where set.
QJsonObject frameGeometryToJson(const FrameGeometry &g);
FrameGeometry frameGeometryFromJson(const QJsonObject &o);

// A geometry worked out for one size of source: where each pixel of the
// print comes from in the frame, and back (GeometryMap).
struct GeometryMap {
    double sourceWidth = 0, sourceHeight = 0;
    PointD frame;                   // after the turns and the mirror
    RectD crop;                     // in the turned frame's pixels, whole pixels
    Affine sourceToOutput, outputToSource;
    bool isIdentity = true;

    GeometryMap() = default;
    // `wholeFrame`: the whole turned, levelled frame, its corners empty where
    // levelling leaves them, for drawing the crop on.
    GeometryMap(const FrameGeometry &g, int sourceWidth, int sourceHeight, bool wholeFrame = false);

    int outputWidth() const { return int(crop.w); }
    int outputHeight() const { return int(crop.h); }
    // a point in normalised frame coordinates (as masks keep them) to the
    // print's pixels, and back
    PointD outputPoint(PointD normalised) const;
    PointD normalisedSource(PointD output) const;
    // The print cut out of `pixels` (RGBA float, the source's size),
    // bilinear, at most `maxLongEdge` on its long side: what the meters read.
    std::vector<float> resample(const std::vector<float> &pixels, int maxLongEdge, int &width, int &height) const;
};
