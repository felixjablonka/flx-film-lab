#pragma once

#include "FrameGeometry.h"
#include "PrintCurves.h"

#include <QJsonObject>

#include <optional>
#include <string>
#include <vector>

// A recipe as the Mac app saves it (Recipe.swift, its Codable form), the
// fields the render reads. A field the file leaves out keeps the Mac's
// default.

struct RGBTrim {
    double red = 0, green = 0, blue = 0;
    double operator[](int i) const { return i == 0 ? red : (i == 1 ? green : blue); }
    bool operator==(const RGBTrim &) const = default;
};

struct CMYOffset {
    double cyan = 0, magenta = 0, yellow = 0;
    bool operator==(const CMYOffset &) const = default;
};

// Graduated exposure: the handles in normalised image coordinates.
struct ExposureGradient {
    std::string id;                 // a UUID, as Foundation writes it
    std::string name = "Gradient";
    double ax = 0.5, ay = 0.3, bx = 0.5, by = 0.55;
    int points = 2;
    double exposureEV = -1;
    bool enabled = true;
    bool isActive() const { return enabled && exposureEV != 0 && points >= 2; }
    bool operator==(const ExposureGradient &) const = default;
};

// A darkroom mask (LocalMask): its points in normalised frame coordinates, so
// a mask drawn on the preview applies unchanged to the export.
enum class MaskShape { Polygon, Oval, Gradient };
std::string maskShapeName(MaskShape shape);    // "polygon", "oval", "gradient"

struct LocalMask {
    std::string id;                 // a UUID, as Foundation writes it
    std::string name = "Mask";
    MaskShape shape = MaskShape::Polygon;
    std::vector<PointD> points;     // polygon: N, Catmull-Rom smoothed; oval: centre and two axis ends; gradient: 2
    double stops = 0;               // +burn / -dodge
    double feather = 0.04;          // sigma as a fraction of the short side
    double grade = 0;               // ISO-R points off the frame's grade; negative is harder
    bool inverted = false;
    bool enabled = true;
    bool operator==(const LocalMask &) const = default;
};

// Glow's look (GlowLook.swift): bloom, glow and its halation, laid on the
// finished print. Spatial values are relative to the frame's long edge, so the
// preview blooms as the export does.
struct GlowLook {
    // the Photoshop Pro Mist recipe: highlights selected, blurred, blended back
    struct Bloom {
        bool enabled = true;
        double amount = 100;        // layer opacity in percent, 0 to 200
        double size = 45;           // 0 to 100 (blurSigmaFraction)
        double threshold = 80;      // display brightness where the selection starts, 0 to 100
        double softness = 100;      // the share above the threshold it fades in over
        std::string blend = "lighten";  // lighten, screen, add
        double haze = 0;            // a wide veil over the whole frame, 0 to 100
        bool isActive() const { return enabled && (amount > 0 || haze > 0); }
        bool operator==(const Bloom &) const = default;
    } bloom;
    // NegPy's lens glow: highlights blurred and added back in linear light
    struct Glow {
        bool enabled = false;
        double amount = 30, size = 40, threshold = 50;
        bool isActive() const { return enabled && amount > 0; }
        bool operator==(const Glow &) const = default;
    } glow;
    // NegPy's halation: red-dominant scatter off the film base
    struct Halation {
        bool enabled = false;
        double amount = 40, size = 52, threshold = 83;
        double tint[3] = {1.0, 0.3, 0.05};
        bool haloOnly = false;
        bool isActive() const { return enabled && amount > 0; }
        bool operator==(const Halation &o) const
        {
            return enabled == o.enabled && amount == o.amount && size == o.size && threshold == o.threshold
                && tint[0] == o.tint[0] && tint[1] == o.tint[1] && tint[2] == o.tint[2] && haloOnly == o.haloOnly;
        }
    } halation;
    bool isActive() const { return bloom.isActive() || glow.isActive() || halation.isActive(); }
    bool operator==(const GlowLook &) const = default;
    // everything off: FLX Film Lab's starting point (Glow's own has bloom on)
    static GlowLook off()
    {
        GlowLook look;
        look.bloom.enabled = false;
        return look;
    }
};

// Sigma as a fraction of the long edge for a size slider: 0.1 % at 0, 5 % at
// 100, on a log scale (BlurSize.sigmaFraction).
double blurSigmaFraction(double slider);

struct Recipe {
    std::string filmStock = "kodak_portra_400";
    std::string printStock = "kodak_portra_endura";

    double exposureEV = 0;
    bool autoExposure = true;
    std::optional<std::string> lensFilter;
    std::vector<ExposureGradient> gradients;
    // The print's shape: turned, mirrored, levelled, cropped. Like the masks
    // it belongs to its frame: pasted settings and a new image leave it out.
    FrameGeometry geometry;
    bool shaped = false;            // a geometry that turns, levels or crops (geometry is not identity)

    double brightness = 0;
    double density = 0;
    bool autoDensity = false;
    double grade = 115;
    bool autoGrade = false;
    double shadowGrade = 0, highlightGrade = 0;
    double shadows = 0, highlights = 0;
    double toe = 0, shoulder = 0, toeWidth = 1, shoulderWidth = 1, snap = 0;
    RGBTrim gradeTrim, toeTrim, shoulderTrim, snapTrim;
    double filterShiftY = 0, filterShiftM = 0;
    CMYOffset shadowCMY, highlightCMY;
    double preflashExposure = 0, preflashY = 0, preflashM = 0;

    double densityCurveGamma = 1;
    bool couplersEnabled = true;
    double couplersAmount = 1, couplersDiffusion = 1;
    bool halationEnabled = true;
    double halationAmount = 1, halationScale = 1, scatterAmount = 1, scatterScale = 1;
    std::optional<bool> cameraSharpness;
    bool grainEnabled = true;
    double grainAmount = 1, grainScale = 1;
    bool glareEnabled = true;
    double glareAmount = 0.03;
    std::vector<LocalMask> masks;
    // Glow's effects; none until first touched, which is all off
    std::optional<GlowLook> effects;
    // Point curves on the finished print; none, as in every recipe saved
    // before them, leaves the print as it is.
    std::optional<PrintCurves> curves;
    std::optional<bool> scanFilm;
    bool normalizeToBase = true;
    std::string outputColorSpace = "sRGB";
    bool gamutCompress = true;
    std::string filmFormat = "120"; // "35 mm" or "120"
    // What the file carries that the port does not model, kept as it was
    // read so it is written back the same.
    QJsonObject extra;

    bool operator==(const Recipe &) const = default;
    bool keepsCameraSharpness() const { return cameraSharpness.value_or(false); }
    // The format's frame, long side by short, mm (FilmFormat.frameMM).
    double frameLongMM() const { return filmFormat == "35 mm" ? 36 : 84; }
    double frameShortMM() const { return filmFormat == "35 mm" ? 24 : 56; }
};

Recipe recipeFromJson(const QJsonObject &json);
// The recipe in the Mac's Codable form: every key its decoder needs, and the
// optionals where they are set.
QJsonObject recipeToJson(const Recipe &recipe);
// A new UUID, upper case, as Foundation's UUID().uuidString.
std::string newIdentifier();
