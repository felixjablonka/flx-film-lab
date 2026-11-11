#include "Recipe.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <QUuid>

namespace {

void read(const QJsonObject &o, const char *key, double &out)
{
    if (o.value(key).isDouble())
        out = o.value(key).toDouble();
}

void read(const QJsonObject &o, const char *key, bool &out)
{
    if (o.value(key).isBool())
        out = o.value(key).toBool();
}

void read(const QJsonObject &o, const char *key, std::string &out)
{
    if (o.value(key).isString())
        out = o.value(key).toString().toStdString();
}

void read(const QJsonObject &o, const char *key, RGBTrim &out)
{
    const QJsonObject t = o.value(key).toObject();
    read(t, "red", out.red);
    read(t, "green", out.green);
    read(t, "blue", out.blue);
}

void read(const QJsonObject &o, const char *key, CMYOffset &out)
{
    const QJsonObject t = o.value(key).toObject();
    read(t, "cyan", out.cyan);
    read(t, "magenta", out.magenta);
    read(t, "yellow", out.yellow);
}

// Glow's decoder: each value where it is present and valid, else its default.
GlowLook glowLookFromJson(const QJsonObject &o)
{
    GlowLook g;
    const QJsonObject b = o.value("bloom").toObject();
    read(b, "enabled", g.bloom.enabled);
    read(b, "amount", g.bloom.amount);
    read(b, "size", g.bloom.size);
    read(b, "threshold", g.bloom.threshold);
    read(b, "softness", g.bloom.softness);
    read(b, "blend", g.bloom.blend);
    if (g.bloom.blend != "lighten" && g.bloom.blend != "screen" && g.bloom.blend != "add")
        g.bloom.blend = "lighten";
    read(b, "haze", g.bloom.haze);
    const QJsonObject w = o.value("glow").toObject();
    read(w, "enabled", g.glow.enabled);
    read(w, "amount", g.glow.amount);
    read(w, "size", g.glow.size);
    read(w, "threshold", g.glow.threshold);
    const QJsonObject h = o.value("halation").toObject();
    read(h, "enabled", g.halation.enabled);
    read(h, "amount", g.halation.amount);
    read(h, "size", g.halation.size);
    read(h, "threshold", g.halation.threshold);
    if (h.value("tint").isObject()) {
        const QJsonObject t = h.value("tint").toObject();
        if (t.value("r").isDouble() && t.value("g").isDouble() && t.value("b").isDouble())
            g.halation.tint[0] = t.value("r").toDouble(), g.halation.tint[1] = t.value("g").toDouble(),
            g.halation.tint[2] = t.value("b").toDouble();
    }
    read(h, "haloOnly", g.halation.haloOnly);
    return g;
}

QJsonObject glowLookToJson(const GlowLook &g)
{
    return {{"bloom", QJsonObject{{"enabled", g.bloom.enabled},
                                  {"amount", g.bloom.amount},
                                  {"size", g.bloom.size},
                                  {"threshold", g.bloom.threshold},
                                  {"softness", g.bloom.softness},
                                  {"blend", QString::fromStdString(g.bloom.blend)},
                                  {"haze", g.bloom.haze}}},
            {"glow", QJsonObject{{"enabled", g.glow.enabled},
                                 {"amount", g.glow.amount},
                                 {"size", g.glow.size},
                                 {"threshold", g.glow.threshold}}},
            {"halation", QJsonObject{{"enabled", g.halation.enabled},
                                     {"amount", g.halation.amount},
                                     {"size", g.halation.size},
                                     {"threshold", g.halation.threshold},
                                     {"tint", QJsonObject{{"r", g.halation.tint[0]},
                                                          {"g", g.halation.tint[1]},
                                                          {"b", g.halation.tint[2]}}},
                                     {"haloOnly", g.halation.haloOnly}}}};
}

} // namespace

std::optional<double> ovalWeight(double px, double py, double ux, double uy, double vx, double vy, double feather,
                                 bool inverted)
{
    const double det = ux * vy - uy * vx;
    if (std::abs(det) <= 1e-6)
        return std::nullopt;
    // the point in the oval's own axes, where the oval is the unit circle
    const double a = (px * vy - py * vx) / det;
    const double b = (ux * py - uy * px) / det;
    const double r = std::sqrt(a * a + b * b);
    const double inner = 1 - std::clamp(feather, 0.0, 1.0);
    const double t = std::clamp((r - inner) / std::max(1 - inner, 1e-4), 0.0, 1.0);
    const double w = 1 - t * t * (3 - 2 * t);
    return inverted ? 1 - w : w;
}

double ExposureGradient::exposureAt(double x, double y, double width, double height) const
{
    if (!isActive())
        return 0;
    const double px = ax * width, py = ay * height;
    const double dx = bx * width - px, dy = by * height - py;
    if (isOval()) {
        const auto w = ovalWeight(x - px, y - py, dx, dy, cx * width - px, cy * height - py, featherAmount(),
                                  isInverted());
        return w ? exposureEV * *w : 0;
    }
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 1e-9)
        return 0;
    const double t = std::clamp(((x - px) * dx + (y - py) * dy) / lengthSquared, 0.0, 1.0);
    return exposureEV * (1 - t * t * (3 - 2 * t));
}

double blurSigmaFraction(double slider)
{
    return 0.001 * std::pow(50.0, std::clamp(slider, 0.0, 100.0) / 100);
}

Recipe recipeFromJson(const QJsonObject &o)
{
    Recipe r;
    read(o, "filmStock", r.filmStock);
    read(o, "printStock", r.printStock);
    read(o, "exposureEV", r.exposureEV);
    read(o, "autoExposure", r.autoExposure);
    if (o.value("lensFilter").isString())
        r.lensFilter = o.value("lensFilter").toString().toStdString();
    for (const QJsonValue &v : o.value("exposureGradients").toArray()) {
        const QJsonObject g = v.toObject();
        ExposureGradient e;
        const QJsonArray points = g.value("points").toArray();     // CGPoint: [x, y]
        e.points = int(points.size());
        if (points.size() >= 2) {
            e.ax = points[0].toArray()[0].toDouble();
            e.ay = points[0].toArray()[1].toDouble();
            e.bx = points[1].toArray()[0].toDouble();
            e.by = points[1].toArray()[1].toDouble();
        }
        if (points.size() >= 3) {
            e.cx = points[2].toArray()[0].toDouble();
            e.cy = points[2].toArray()[1].toDouble();
        }
        read(g, "id", e.id);
        read(g, "name", e.name);
        read(g, "exposureEV", e.exposureEV);
        read(g, "enabled", e.enabled);
        const QString shape = g.value("shape").toString();
        if (shape == QLatin1String("oval"))
            e.shape = GradientShape::Oval;
        else if (shape == QLatin1String("linear"))
            e.shape = GradientShape::Linear;
        if (g.value("feather").isDouble())
            e.feather = g.value("feather").toDouble();
        if (g.value("inverted").isBool())
            e.inverted = g.value("inverted").toBool();
        r.gradients.push_back(e);
    }
    // FrameGeometry is left out of the file at its identity.
    if (o.value("frameGeometry").isObject())
        r.geometry = frameGeometryFromJson(o.value("frameGeometry").toObject());
    r.shaped = !r.geometry.isIdentity();


    read(o, "brightness", r.brightness);
    read(o, "density", r.density);
    read(o, "autoDensity", r.autoDensity);
    read(o, "grade", r.grade);
    read(o, "autoGrade", r.autoGrade);
    read(o, "shadowGrade", r.shadowGrade);
    read(o, "highlightGrade", r.highlightGrade);
    read(o, "shadows", r.shadows);
    read(o, "highlights", r.highlights);
    read(o, "toe", r.toe);
    read(o, "shoulder", r.shoulder);
    read(o, "toeWidth", r.toeWidth);
    read(o, "shoulderWidth", r.shoulderWidth);
    read(o, "snap", r.snap);
    read(o, "gradeTrim", r.gradeTrim);
    read(o, "toeTrim", r.toeTrim);
    read(o, "shoulderTrim", r.shoulderTrim);
    read(o, "snapTrim", r.snapTrim);
    read(o, "filterShiftY", r.filterShiftY);
    read(o, "filterShiftM", r.filterShiftM);
    read(o, "shadowCMY", r.shadowCMY);
    read(o, "highlightCMY", r.highlightCMY);
    read(o, "preflashExposure", r.preflashExposure);
    read(o, "preflashY", r.preflashY);
    read(o, "preflashM", r.preflashM);

    read(o, "densityCurveGamma", r.densityCurveGamma);
    read(o, "couplersEnabled", r.couplersEnabled);
    read(o, "couplersAmount", r.couplersAmount);
    read(o, "couplersDiffusion", r.couplersDiffusion);
    read(o, "halationEnabled", r.halationEnabled);
    read(o, "halationAmount", r.halationAmount);
    read(o, "halationScale", r.halationScale);
    read(o, "scatterAmount", r.scatterAmount);
    read(o, "scatterScale", r.scatterScale);
    if (o.value("cameraSharpness").isBool())
        r.cameraSharpness = o.value("cameraSharpness").toBool();
    read(o, "grainEnabled", r.grainEnabled);
    read(o, "grainAmount", r.grainAmount);
    read(o, "grainScale", r.grainScale);
    read(o, "glareEnabled", r.glareEnabled);
    read(o, "glareAmount", r.glareAmount);
    for (const QJsonValue &v : o.value("masks").toArray()) {
        const QJsonObject j = v.toObject();
        LocalMask m;
        read(j, "id", m.id);
        read(j, "name", m.name);
        std::string shape;
        read(j, "shape", shape);
        m.shape = shape == "oval" ? MaskShape::Oval : shape == "gradient" ? MaskShape::Gradient : MaskShape::Polygon;
        for (const QJsonValue &p : j.value("points").toArray())     // CGPoint: [x, y]
            m.points.push_back({p.toArray()[0].toDouble(), p.toArray()[1].toDouble()});
        read(j, "stops", m.stops);
        read(j, "feather", m.feather);
        read(j, "grade", m.grade);
        read(j, "inverted", m.inverted);
        read(j, "enabled", m.enabled);
        r.masks.push_back(m);
    }
    if (o.value("effects").isObject())
        r.effects = glowLookFromJson(o.value("effects").toObject());
    if (o.value("curves").isObject())
        r.curves = printCurvesFromJson(o.value("curves").toObject());
    if (o.value("scanFilm").isBool())
        r.scanFilm = o.value("scanFilm").toBool();
    read(o, "normalizeToBase", r.normalizeToBase);
    read(o, "outputColorSpace", r.outputColorSpace);
    read(o, "gamutCompress", r.gamutCompress);
    read(o, "filmFormat", r.filmFormat);
    if (o.value("processingTemperature").isDouble())
        r.processingTemperature = o.value("processingTemperature").toDouble();
    return r;
}

namespace {

QJsonObject trim(const RGBTrim &t)
{
    return {{"red", t.red}, {"green", t.green}, {"blue", t.blue}};
}

QJsonObject cmy(const CMYOffset &c)
{
    return {{"cyan", c.cyan}, {"magenta", c.magenta}, {"yellow", c.yellow}};
}

QJsonArray masks(const std::vector<LocalMask> &all)
{
    QJsonArray out;
    for (const LocalMask &m : all) {
        QJsonArray points;
        for (const PointD &p : m.points)
            points.append(QJsonArray{p.x, p.y});
        out.append(QJsonObject{{"id", QString::fromStdString(m.id.empty() ? newIdentifier() : m.id)},
                               {"name", QString::fromStdString(m.name)},
                               {"shape", QString::fromStdString(maskShapeName(m.shape))},
                               {"points", points},
                               {"stops", m.stops},
                               {"feather", m.feather},
                               {"grade", m.grade},
                               {"inverted", m.inverted},
                               {"enabled", m.enabled}});
    }
    return out;
}

} // namespace

std::string maskShapeName(MaskShape shape)
{
    return shape == MaskShape::Oval ? "oval" : shape == MaskShape::Gradient ? "gradient" : "polygon";
}

std::string newIdentifier()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper().toStdString();
}

QJsonObject recipeToJson(const Recipe &r)
{
    auto str = [](const std::string &s) { return QString::fromStdString(s); };
    QJsonObject o{
        {"filmStock", str(r.filmStock)},
        {"printStock", str(r.printStock)},
        {"exposureEV", r.exposureEV},
        {"autoExposure", r.autoExposure},
        {"brightness", r.brightness},
        {"density", r.density},
        {"autoDensity", r.autoDensity},
        {"grade", r.grade},
        {"autoGrade", r.autoGrade},
        {"shadowGrade", r.shadowGrade},
        {"highlightGrade", r.highlightGrade},
        {"shadows", r.shadows},
        {"highlights", r.highlights},
        {"toe", r.toe},
        {"shoulder", r.shoulder},
        {"toeWidth", r.toeWidth},
        {"shoulderWidth", r.shoulderWidth},
        {"snap", r.snap},
        {"gradeTrim", trim(r.gradeTrim)},
        {"toeTrim", trim(r.toeTrim)},
        {"shoulderTrim", trim(r.shoulderTrim)},
        {"snapTrim", trim(r.snapTrim)},
        {"filterShiftY", r.filterShiftY},
        {"filterShiftM", r.filterShiftM},
        {"shadowCMY", cmy(r.shadowCMY)},
        {"highlightCMY", cmy(r.highlightCMY)},
        {"preflashExposure", r.preflashExposure},
        {"preflashY", r.preflashY},
        {"preflashM", r.preflashM},
        {"densityCurveGamma", r.densityCurveGamma},
        {"couplersEnabled", r.couplersEnabled},
        {"couplersAmount", r.couplersAmount},
        {"couplersDiffusion", r.couplersDiffusion},
        {"halationEnabled", r.halationEnabled},
        {"halationAmount", r.halationAmount},
        {"halationScale", r.halationScale},
        {"scatterAmount", r.scatterAmount},
        {"scatterScale", r.scatterScale},
        {"grainEnabled", r.grainEnabled},
        {"grainAmount", r.grainAmount},
        {"grainScale", r.grainScale},
        {"glareEnabled", r.glareEnabled},
        {"glareAmount", r.glareAmount},
        {"masks", masks(r.masks)},
        {"normalizeToBase", r.normalizeToBase},
        {"outputColorSpace", str(r.outputColorSpace)},
        {"gamutCompress", r.gamutCompress},
        {"filmFormat", str(r.filmFormat)},
    };
    if (r.lensFilter)
        o.insert("lensFilter", str(*r.lensFilter));
    if (!r.gradients.empty()) {
        QJsonArray gradients;
        for (const ExposureGradient &g : r.gradients) {
            QJsonArray points;
            if (g.points >= 2)
                points = {QJsonArray{g.ax, g.ay}, QJsonArray{g.bx, g.by}};
            if (g.points >= 3)
                points.append(QJsonArray{g.cx, g.cy});
            QJsonObject o{{"id", str(g.id.empty() ? newIdentifier() : g.id)},
                          {"name", str(g.name)},
                          {"points", points},
                          {"exposureEV", g.exposureEV},
                          {"enabled", g.enabled}};
            // the optionals only where they are set, as the Mac's encoder
            if (g.shape)
                o.insert("shape", *g.shape == GradientShape::Oval ? "oval" : "linear");
            if (g.feather)
                o.insert("feather", *g.feather);
            if (g.inverted)
                o.insert("inverted", *g.inverted);
            gradients.append(o);
        }
        o.insert("exposureGradients", gradients);
    }
    if (r.cameraSharpness)
        o.insert("cameraSharpness", *r.cameraSharpness);
    if (r.scanFilm)
        o.insert("scanFilm", *r.scanFilm);
    if (r.processingTemperature)
        o.insert("processingTemperature", *r.processingTemperature);
    // left out at its identity, as the Mac's
    if (!r.geometry.isIdentity())
        o.insert("frameGeometry", frameGeometryToJson(r.geometry));
    if (r.effects)
        o.insert("effects", glowLookToJson(*r.effects));
    if (r.curves)
        o.insert("curves", printCurvesToJson(*r.curves));
    return o;
}
