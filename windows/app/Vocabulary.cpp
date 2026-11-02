#include "Vocabulary.h"

#include "Packs.h"

QString vocabularyName(Vocabulary v)
{
    return v == Vocabulary::Photographic ? QStringLiteral("Photographic") : QStringLiteral("Darkroom");
}

QString ControlSpec::help(Vocabulary v, double physical) const
{
    const bool photographic = v == Vocabulary::Photographic;
    return QStringLiteral("%1\n\n%2 name: %3 (currently %4).")
        .arg(explanation, photographic ? QStringLiteral("Darkroom") : QStringLiteral("Photographic"),
             photographic ? darkroomName : photographicName, QString::number(physical, 'f', 2));
}

namespace {

// A control that is the same quantity in both vocabularies, only rescaled and
// possibly flipped so that "more" means "brighter".
ControlSpec linear(const char *photographic, const char *darkroom, double physicalPerUnit, const char *explanation,
                   double low = -100, double high = 100, int decimals = 0, const char *unit = "")
{
    ControlSpec s;
    s.photographicName = QString::fromUtf8(photographic);
    s.darkroomName = QString::fromUtf8(darkroom);
    s.low = low;
    s.high = high;
    s.decimals = decimals;
    s.unit = QString::fromUtf8(unit);
    s.explanation = QString::fromUtf8(explanation);
    s.toPhysical = [physicalPerUnit](double v) { return v * physicalPerUnit; };
    s.fromPhysical = [physicalPerUnit](double v) { return physicalPerUnit == 0 ? 0 : v / physicalPerUnit; };
    return s;
}

} // namespace

namespace controls {

// Midtone brightness, in fractions of the paper's lightness range: ±100
// moves mid-grey by a fifth of the distance from paper black to white.
const ControlSpec brightness = linear(
    "Brightness", "Midtone density", 0.002,
    "How light the midtones are. Mid-grey moves most, and the move fades to nothing toward paper black and paper "
    "white, so neither end clips or goes milky. For a change that rides the whole curve, use Print exposure.");

// More light on the paper is a darker print, so the photographic slider runs
// the other way.
const ControlSpec printExposure = linear(
    "Print exposure", "Density", -0.03,
    "The enlarger timer: every tone moves, the blacks and whites included, rolling into the paper's toe and "
    "shoulder rather than clipping.");

ControlSpec contrast(float softest)
{
    const double reference = kGradeReference;
    const double soft = (double(softest) - reference) / 100;
    ControlSpec s;
    s.photographicName = QStringLiteral("Contrast");
    s.darkroomName = QStringLiteral("Grade");
    s.explanation = QStringLiteral("Overall contrast, set by the paper's contrast grade. R115 is neutral; harder "
                                   "grades compress the scale into fewer stops.");
    s.toPhysical = [=](double v) { return reference - v * (v < 0 ? soft : 0.65); };
    s.fromPhysical = [=](double v) { return reference - v < 0 ? (reference - v) / soft : (reference - v) / 0.65; };
    return s;
}

const ControlSpec shadowContrast = linear(
    "Shadow contrast", "Shadow grade", -0.4,
    "Contrast in the shadows alone: the hard-filter half of a split-grade print.");

const ControlSpec highlightContrast = linear(
    "Highlight contrast", "Highlight grade", -0.4,
    "Contrast in the highlights alone: the soft-filter half of a split-grade print.");

const ControlSpec shadows = linear(
    "Shadows", "Shadow density", 0.01,
    "Lightens or darkens the dark half of the print, most in the deep shadows just above black. It is gone by "
    "mid-grey and leaves paper black where it is; Blacks sets that. Lifting has more room than darkening: there is "
    "little space left between a deep shadow and black.");

const ControlSpec highlights = linear(
    "Highlights", "Highlight density", 0.01,
    "The same for the light half: most in the bright highlights just below white, gone by mid-grey, and paper "
    "white stays put; Whites sets that. Holding highlights back has more room than pushing them toward white.");

const ControlSpec blacks = linear(
    "Blacks", "Toe", 0.01,
    "How the deepest tones roll off. Positive opens the blacks and keeps shadow detail; negative sharpens the knee "
    "for a harder black.");

const ControlSpec whites = linear(
    "Whites", "Shoulder", -0.01,
    "How the brightest tones roll off. Negative holds highlights back from paper white; positive lets them run to "
    "white sooner.");

const ControlSpec blacksRange = linear(
    "Blacks range", "Toe reach", 0.01, "How far the black roll-off reaches up into the tonal scale.", 10, 300, 0, "%");

const ControlSpec whitesRange = linear(
    "Whites range", "Shoulder reach", 0.01, "How far the white roll-off reaches down into the tonal scale.", 10, 300,
    0, "%");

const ControlSpec midtoneContrast = linear(
    "Midtone contrast", "Snap", 0.005,
    "Contrast in the midtones only, anchored so mid-grey does not move and easing to nothing at both ends.");

// The enlarger's yellow filter runs along the blue-yellow axis, inverted:
// more yellow in the printing light makes the print cooler.
const ControlSpec temperature = linear(
    "Temperature", "Yellow filter", -0.4,
    "Warm/cool balance, applied by winding the enlarger's real yellow dichroic filter: a change to the printing "
    "light, not a tint on the output.");

const ControlSpec tint = linear(
    "Tint", "Magenta filter", -0.4, "Green/magenta balance, applied with the enlarger's magenta filter.");

const ControlSpec highlightProtection = linear(
    "Highlight protection", "Pre-flash", 0.003,
    "A uniform pre-exposure of the paper. The darkroom way to hold highlights: it lifts the paper off its toe so "
    "faint highlight detail starts to register.",
    0, 100, 0, "%");

const ControlSpec maskExposure = linear(
    "Brightness", "Exposure", -0.02, "Positive brightens this area (dodge), negative darkens it (burn).");

const ControlSpec maskContrast = linear(
    "Contrast", "Local grade", -0.3,
    "Prints this area through a different contrast filter: what a hard-filter burn is on variable-contrast paper. "
    "It changes the area's contrast without moving its brightness.");

} // namespace controls
