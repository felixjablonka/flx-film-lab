#include "EditNames.h"

#include <QStringList>

#include <algorithm>

namespace {

// A control's name as a menu item puts it: "Print exposure" is "Print Exposure".
std::string titled(const QString &name)
{
    static const QStringList minor{"and", "or", "of", "on", "the", "to", "in", "a"};
    QStringList words = name.split(QLatin1Char(' '));
    for (int i = 0; i < words.size(); ++i)
        if (!(i > 0 && minor.contains(words[i])) && !words[i].isEmpty())
            words[i][0] = words[i][0].toUpper();
    return words.join(QLatin1Char(' ')).toStdString();
}

template<typename T>
std::string list(const std::vector<T> &a, const std::vector<T> &b, const std::string &noun)
{
    return b.size() > a.size() ? "Add " + noun : (b.size() < a.size() ? "Delete " + noun : noun);
}

std::string shape(const FrameGeometry &a, const FrameGeometry &b)
{
    if (a.quarterTurns != b.quarterTurns)
        return "Turn";
    if (a.mirrored != b.mirrored)
        return "Mirror";
    if (a.straighten != b.straighten)
        return "Straighten";
    return "Crop";
}

} // namespace

std::vector<std::string> recipeChangeNames(const Recipe &a, const Recipe &b, Vocabulary v, bool silver)
{
    // a new film can bring its own paper and filter with it
    if (a.filmStock != b.filmStock)
        return {"Film"};
    std::vector<std::string> names;
    auto add = [&names](bool changed, const std::string &name) {
        if (changed)
            names.push_back(name);
    };
    auto control = [v](const ControlSpec &spec) { return titled(spec.name(v)); };
    add(a.printStock != b.printStock, "Paper");
    const bool ev = a.exposureEV != b.exposureEV;
    add(ev, "Exposure");
    // moving a slider whose auto is on turns the auto off
    add(a.autoExposure != b.autoExposure, ev ? "Exposure" : "Auto Exposure");
    add(a.lensFilter != b.lensFilter, "Filter");
    add(a.gradients != b.gradients, list(a.gradients, b.gradients, "Gradient"));
    add(!(a.geometry == b.geometry), shape(a.geometry, b.geometry));
    const bool brightness = a.brightness != b.brightness;
    add(brightness, control(controls::brightness));
    add(a.autoDensity != b.autoDensity, brightness ? control(controls::brightness) : "Auto Brightness");
    add(a.density != b.density, control(controls::printExposure));
    const std::string contrast = titled(v == Vocabulary::Photographic ? QStringLiteral("Contrast") : QStringLiteral("Grade"));
    add(a.grade != b.grade, contrast);
    add(a.autoGrade != b.autoGrade, a.grade != b.grade ? contrast : "Auto Contrast");
    add(a.shadowGrade != b.shadowGrade, control(controls::shadowContrast));
    add(a.highlightGrade != b.highlightGrade, control(controls::highlightContrast));
    add(a.shadows != b.shadows, control(controls::shadows));
    add(a.highlights != b.highlights, control(controls::highlights));
    add(a.toe != b.toe, control(controls::blacks));
    add(a.shoulder != b.shoulder, control(controls::whites));
    add(a.toeWidth != b.toeWidth, control(controls::blacksRange));
    add(a.shoulderWidth != b.shoulderWidth, control(controls::whitesRange));
    add(a.snap != b.snap, control(controls::midtoneContrast));
    add(!(a.gradeTrim == b.gradeTrim) || !(a.toeTrim == b.toeTrim) || !(a.shoulderTrim == b.shoulderTrim)
            || !(a.snapTrim == b.snapTrim),
        "Colour Crossover");
    add(a.filterShiftY != b.filterShiftY, control(controls::temperature));
    add(a.filterShiftM != b.filterShiftM, control(controls::tint));
    add(!(a.shadowCMY == b.shadowCMY), "Shadow Colour");
    add(!(a.highlightCMY == b.highlightCMY), "Highlight Colour");
    add(a.preflashExposure != b.preflashExposure || a.preflashY != b.preflashY || a.preflashM != b.preflashM,
        control(controls::highlightProtection));
    add(a.densityCurveGamma != b.densityCurveGamma, "Curve Gamma");
    add(a.couplersEnabled != b.couplersEnabled || a.couplersAmount != b.couplersAmount,
        silver ? "Edge Effects" : "Couplers");
    add(a.couplersDiffusion != b.couplersDiffusion, silver ? "Edge Width" : "Coupler Diffusion");
    add(a.halationEnabled != b.halationEnabled || a.halationAmount != b.halationAmount, "Halation");
    add(a.halationScale != b.halationScale, "Halation Size");
    add(a.scatterAmount != b.scatterAmount || a.scatterScale != b.scatterScale, "Scatter");
    add(a.cameraSharpness != b.cameraSharpness, "Sharpness");
    add(a.grainEnabled != b.grainEnabled || a.grainAmount != b.grainAmount, "Grain");
    add(a.grainScale != b.grainScale, "Grain Size");
    add(a.glareEnabled != b.glareEnabled || a.glareAmount != b.glareAmount, "Glare");
    add(a.masks != b.masks, list(a.masks, b.masks, "Mask"));
    add(a.effects != b.effects, "Bloom & Glow");
    add(a.curves != b.curves, "Curves");
    add(a.scanFilm != b.scanFilm, "Print on Paper");
    add(a.normalizeToBase != b.normalizeToBase, "Base White");
    add(a.outputColorSpace != b.outputColorSpace, "Colour Space");
    add(a.gamutCompress != b.gamutCompress, "Gamut Compression");
    add(a.filmFormat != b.filmFormat, "Film Format");
    return names;
}

std::string stepName(const std::vector<std::string> &names)
{
    std::vector<std::string> unique;
    for (const std::string &n : names)
        if (std::find(unique.begin(), unique.end(), n) == unique.end())
            unique.push_back(n);
    return unique.size() == 1 ? unique[0] : "Changes";
}

std::string lookChangeName(const Look &a, const Look &b, Vocabulary v, bool silver)
{
    std::vector<std::string> names = recipeChangeNames(a.recipe, b.recipe, v, silver);
    if (a.whiteBalance != b.whiteBalance || a.customTemperature != b.customTemperature
        || a.customTint != b.customTint)
        names.push_back("White Balance");
    return stepName(names);
}

std::string motionChangeName(const MotionLook &a, const MotionLook &b, bool silver)
{
    std::vector<std::string> names = recipeChangeNames(a.recipe, b.recipe, Vocabulary::Photographic, silver);
    if (!(a.effects == b.effects))
        names.push_back("Effects");
    if (!(a.input == b.input))
        names.push_back("Log Input");
    return stepName(names);
}
