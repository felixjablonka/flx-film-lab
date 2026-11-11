#include "Setup.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <stdexcept>

namespace {

[[noreturn]] void fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}

QJsonValue field(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(key);
    if (v.isUndefined())
        fail(QStringLiteral("setup.json: no \"%1\"").arg(key));
    return v;
}

float number(const QJsonObject &o, const char *key) { return float(field(o, key).toDouble()); }
int32_t integer(const QJsonObject &o, const char *key) { return int32_t(field(o, key).toInteger()); }

Vec4 vec3(const QJsonObject &o, const char *key)
{
    const QJsonArray a = field(o, key).toArray();
    if (a.size() != 3)
        fail(QStringLiteral("setup.json: \"%1\" is not three numbers").arg(key));
    return {float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()), 0};
}

template<size_t N>
void floats(const QJsonObject &o, const char *key, float (&out)[N])
{
    const QJsonArray a = field(o, key).toArray();
    if (size_t(a.size()) != N)
        fail(QStringLiteral("setup.json: \"%1\" is not %2 numbers").arg(key).arg(N));
    for (size_t i = 0; i < N; ++i)
        out[i] = float(a[qsizetype(i)].toDouble());
}

// A 3x3 matrix, written as its columns, nine numbers.
void columns(const QJsonObject &o, const char *key, Vec4 (&out)[3])
{
    const QJsonArray a = field(o, key).toArray();
    if (a.size() != 9)
        fail(QStringLiteral("setup.json: \"%1\" is not nine numbers").arg(key));
    for (int c = 0; c < 3; ++c)
        out[c] = {float(a[c * 3].toDouble()), float(a[c * 3 + 1].toDouble()), float(a[c * 3 + 2].toDouble()), 0};
}

} // namespace

RenderSetup loadSetup(const QString &directory)
{
    RenderSetup s;
    s.directory = directory;
    const QDir dir(directory);
    QFile f(dir.filePath("setup.json"));
    if (!f.open(QIODevice::ReadOnly))
        fail("cannot open " + f.fileName());
    s.json = QJsonDocument::fromJson(f.readAll()).object();
    s.width = integer(s.json, "width");
    s.height = integer(s.json, "height");

    const QJsonObject film = field(s.json, "filmUniforms").toObject();
    columns(film, "rgbToXYZ", s.film.rgbToXYZ);
    s.film.scale.x = number(film, "exposureScale");
    s.film.counts.x = integer(film, "tcLutSize");
    s.film.counts.y = integer(film, "intoTriangle");
    // a, b, c, ev, kind, feather, inverted, as ExposureGradientUniform
    for (const QJsonValue &g : field(s.json, "gradients").toArray()) {
        const QJsonArray a = g.toArray();
        auto at = [&](int i) { return float(a[i].toDouble()); };
        s.gradients.push_back({at(0), at(1), at(2), at(3)});
        s.gradients.push_back({at(4), at(5), 0, 0});
        s.gradients.push_back({at(6), at(7), at(8), at(9)});
    }
    s.film.counts.z = int32_t(s.gradients.size() / kGradientVec4s);
    if (s.gradients.empty())
        s.gradients.resize(kGradientVec4s);     // a storage buffer must have a size

    const QJsonObject develop = field(s.json, "developUniforms").toObject();
    s.develop.axis = {number(develop, "logE0"), number(develop, "logEStep"),
                      number(develop, "edgeCap"), number(develop, "diffusionFraction")};
    s.develop.gammaFactor = vec3(develop, "gammaFactor");
    s.develop.densityMax = vec3(develop, "densityMax");
    s.develop.couplers = {number(develop, "couplersAmount"), float(integer(develop, "isPositive")), 0, 0};

    const QJsonObject print = field(s.json, "printUniforms").toObject();
    columns(print, "xyzToRGB", s.print.xyzToRGB);
    s.print.exposureFactorMidgray = vec3(print, "exposureFactorMidgray");
    s.print.preflashRaw = vec3(print, "preflashRaw");
    s.print.exposure = {number(print, "printExposure"), number(print, "cmfNormalization"),
                        number(print, "toneLogE0"), number(print, "toneLogEStep")};
    s.print.grades = {number(print, "gradeSpanMin"), number(print, "gradeSpanMax"),
                      number(print, "glareAmount"), number(print, "toneShiftMax")};
    s.print.illuminantXYZ = vec3(print, "illuminantXYZ");
    s.print.gamut = {number(print, "gamutThreshold"), number(print, "gamutLimit"), number(print, "gamutPower"), 0};
    s.print.flags = {integer(print, "toneLutWidth"), integer(print, "hasDodgeBurn"),
                     integer(print, "gamutCompress"), integer(print, "scanFilm")};
    s.print.flags2 = {integer(print, "toneShiftCount"), 0, 0, 0};
    columns(print, "gamutRGBToLMS", s.print.gamutRGBToLMS);
    columns(print, "gamutLMSToRGB", s.print.gamutLMSToRGB);

    // The frame as it is: GeometryUniforms() in Renderer.swift.
    s.shape.columns = {1, 0, 0, 1};
    s.shape.offsetSize = {0, 0, 0, 0};
    s.encode.modes = {integer(s.json, "encoding"), 0, 0, 0};
    s.encode.columns = s.shape.columns;

    s.input = readDump(dir.filePath("input.f32"));
    s.spectral = readDump(dir.filePath("spectral.f32"));
    s.curves = readDump(dir.filePath("curves.f32"));
    s.toneShift = readDump(dir.filePath("toneShift.f32"));
    if (field(s.json, "stages").toObject().value("curves").toBool())
        s.printCurves = readDump(dir.filePath("printCurves.f32")).floats;
    s.toneLut = readDump(dir.filePath("toneLut.f32"));
    s.tcLut = readDump(dir.filePath("tcLut.f32"));

    // The film stage's other inputs.
    s.stock = field(s.json, "film").toString();
    const QJsonObject recipe = field(s.json, "recipe").toObject();
    s.recipe.halationAmount = field(recipe, "halationAmount").toDouble();
    s.recipe.halationScale = field(recipe, "halationScale").toDouble();
    s.recipe.scatterAmount = field(recipe, "scatterAmount").toDouble();
    s.recipe.scatterScale = field(recipe, "scatterScale").toDouble();
    s.recipe.couplersDiffusion = field(recipe, "couplersDiffusion").toDouble();
    s.recipe.grainAmount = field(recipe, "grainAmount").toDouble();
    s.recipe.grainScale = field(recipe, "grainScale").toDouble();
    s.recipe.cameraSharpness = field(recipe, "cameraSharpness").toBool();
    const QJsonArray frame = field(recipe, "frameMM").toArray();
    s.recipe.frameLongMM = frame[0].toDouble();
    s.recipe.frameShortMM = frame[1].toDouble();

    const QJsonObject stock = field(s.json, "stock").toObject();
    s.monochrome = field(stock, "monochrome").toBool();
    if (stock.contains("halation")) {
        const QJsonObject h = stock.value("halation").toObject();
        s.halation.active = field(h, "active").toBool();
        floats(h, "scatterCoreUm", s.halation.scatterCoreUm);
        floats(h, "scatterTailUm", s.halation.scatterTailUm);
        floats(h, "scatterTailWeight", s.halation.scatterTailWeight);
        floats(h, "halationStrength", s.halation.halationStrength);
        floats(h, "halationFirstSigmaUm", s.halation.halationFirstSigmaUm);
        s.halation.bounces = integer(h, "halationNBounces");
        s.halation.bounceDecay = number(h, "halationBounceDecay");
        s.halation.renormalize = field(h, "halationRenormalize").toBool();
    }
    if (stock.contains("dirCouplers"))
        s.couplerDiffusionUm = number(stock.value("dirCouplers").toObject(), "diffusionSizeUm");
    columns(s.json, "couplerMatrix", s.couplerMatrix);
    s.curves0 = readDump(dir.filePath("curves0.f32"));
    s.edgeScale = readDump(dir.filePath("edgeScale.f32"));
    if (stock.contains("grain")) {
        const QJsonObject g = stock.value("grain").toObject();
        floats(g, "tableDensityMax", s.grain.tableDensityMax);
        floats(g, "cloudUm", s.grain.cloudUm);
        s.grain.samples = integer(g, "tableSamples");
        s.selwyn = readDump(dir.filePath("selwyn.f32"));
        if (s.selwyn.floats.size() != size_t(9 * s.grain.samples))
            fail("selwyn.f32 does not match tableSamples");
    }
    const QJsonObject seeds = field(s.json, "grainSeed").toObject();
    s.fileSeed = uint32_t(field(seeds, "file").toInteger());
    s.grainSeed = uint32_t(field(seeds, "seed").toInteger());
    s.pixelSizeUm = number(s.json, "pixelSizeUm");
    const QJsonObject stages = field(s.json, "stages").toObject();
    s.stages.halation = stages.value("halation").toBool();
    s.stages.couplers = stages.value("couplers").toBool();
    s.stages.grain = stages.value("grain").toBool();
    s.stages.shaped = stages.value("shaped").toBool();
    s.stages.effects = stages.value("effects").toBool();
    s.stages.dodgeBurn = stages.value("dodgeBurn").toBool();
    for (const QJsonValue &v : field(s.json, "mpsKernels").toArray()) {
        const QJsonObject k = v.toObject();
        MpsKernel kernel;
        kernel.sigma = number(k, "sigma");
        for (const QJsonValue &t : field(k, "taps").toArray())
            kernel.taps.push_back(float(t.toDouble()));
        s.mpsKernels.push_back(std::move(kernel));
    }
    if (s.input.width != s.width || s.input.height != s.height || s.input.channels != 4)
        fail("input.f32 does not match setup.json");
    if (s.tcLut.width != s.film.counts.x)
        fail("tcLut.f32 does not match the tcLutSize in setup.json");
    return s;
}
