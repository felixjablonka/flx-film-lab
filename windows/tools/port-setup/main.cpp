// flxport-setup: the port's own render setup, made from the film packs and
// the recipe a dump started from (recipe.json, in the Mac app's own format)
// on the dump's picture, meters and all; checked value by value against the
// setup the Mac engine made, and rendered, so its stages can be compared
// with the Mac's (tools/compare) like any port render.
//
//   flxport-setup <dumpdir> [--packs DIR] [--backend metal|d3d12|d3d11|vulkan] [--no-render] [--kept]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). The
// render goes into port-<backend>-ownsetup/.
//
// --kept checks the print's kept exposure instead (setKeptExposureBudget), as
// the Mac's tests/checks do: one renderer through a sequence of edits, each
// print compared with a fresh renderer's, and that it printed from the kept
// exposure exactly where it may. Exits 1 if a step differs.

#include "FilmRenderer.h"
#include "Filters.h"
#include "FrameGeometry.h"
#include "MaskRasterizer.h"
#include "Packs.h"
#include "Recipe.h"
#include "Setup.h"
#include "SetupBuilder.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {

#if defined(Q_OS_WIN)
const char *kDefaultBackend = "d3d12";
#else
const char *kDefaultBackend = "metal";
#endif

std::unique_ptr<QRhi> makeRhi(const QString &backend)
{
#if defined(Q_OS_MACOS)
    if (backend == "metal") {
        QRhiMetalInitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::Metal, &params));
    }
#endif
#if defined(Q_OS_WIN)
    if (backend == "d3d12") {
        QRhiD3D12InitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D12, &params));
    }
    if (backend == "d3d11") {
        QRhiD3D11InitParams params;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::D3D11, &params));
    }
#endif
    throw std::runtime_error("no backend \"" + backend.toStdString() + "\" in this build");
}

// The worst difference over everything compared, as a share of each item's
// own largest value.
double g_worst = 0;
QString g_worstItem;

void compare(const char *item, const float *mac, const float *port, size_t n)
{
    double scale = 0, maxAbs = 0;
    size_t at = 0;
    for (size_t i = 0; i < n; ++i)
        scale = std::max(scale, std::abs(double(mac[i])));
    for (size_t i = 0; i < n; ++i) {
        const double d = std::abs(double(mac[i]) - double(port[i]));
        if (d > maxAbs || std::isnan(d)) {
            maxAbs = std::isnan(d) ? INFINITY : d;
            at = i;
        }
    }
    const double rel = scale > 0 ? maxAbs / scale : maxAbs;
    if (rel > g_worst) {
        g_worst = rel;
        g_worstItem = item;
    }
    if (maxAbs == 0)
        std::printf("  %-26s %8zu values  identical\n", item, n);
    else
        std::printf("  %-26s %8zu values  max |d| %.3g = %.2g of its largest, at %zu: %.9g vs %.9g\n", item, n, maxAbs,
                    rel, at, double(mac[at]), double(port[at]));
}

void compare(const char *item, const std::vector<float> &mac, const std::vector<float> &port)
{
    if (mac.size() != port.size()) {
        std::printf("  %-26s SIZES DIFFER: %zu vs %zu\n", item, mac.size(), port.size());
        g_worst = INFINITY;
        g_worstItem = item;
        return;
    }
    compare(item, mac.data(), port.data(), mac.size());
}

template<typename Block>
void compareBlock(const char *item, const Block &mac, const Block &port)
{
    static_assert(sizeof(Block) % 4 == 0);
    // the integer fields compare as numbers too: both are whole vec4s of 4-byte values
    compare(item, reinterpret_cast<const float *>(&mac), reinterpret_cast<const float *>(&port), sizeof(Block) / 4);
}

void compareInts(const char *item, std::initializer_list<long long> mac, std::initializer_list<long long> port)
{
    const bool same = std::equal(mac.begin(), mac.end(), port.begin(), port.end());
    std::printf("  %-26s %s\n", item, same ? "identical" : "DIFFER");
    if (!same) {
        g_worst = INFINITY;
        g_worstItem = item;
    }
}

// The print's shape, as RenderEngine::shape lays it on a setup.
void shapeSetup(RenderSetup &s, const GeometryMap &map)
{
    const Affine &t = map.outputToSource;
    s.shape.columns = {float(t.a), float(t.b), float(t.c), float(t.d)};
    s.shape.offsetSize = {float(t.tx), float(t.ty), float(map.sourceWidth), float(map.sourceHeight)};
    s.shape.active = {map.isIdentity ? 0 : 1, 0, 0, 0};
    s.encode.columns = s.shape.columns;
    s.encode.offsetSize = s.shape.offsetSize;
    s.encode.active = s.shape.active;
    s.encode.modes.y = 0;
    s.outputWidth = map.outputWidth();
    s.outputHeight = map.outputHeight();
    s.stages.shaped = !map.isIdentity;
}

// --kept: the Mac's "the print's exposure, kept between renders, prints what
// a fresh render does" (tests/checks).
int keptChecks(SetupBuilder &builder, const BuiltSetup &start, const RenderSetup &mac, QRhi *rhi)
{
    const int w = mac.width, h = mac.height;
    LocalMask burn;
    burn.id = "00000000-0000-0000-0000-000000000001";
    burn.name = "Burn";
    burn.shape = MaskShape::Oval;
    burn.points = {{0.5, 0.45}, {0.75, 0.45}, {0.5, 0.7}};
    burn.stops = 0.5;
    struct Step {
        const char *name;
        std::function<void(Recipe &, float &)> edit;
        bool fromKept;
    };
    const std::vector<Step> steps = {
        {"as it is", [](Recipe &, float &) {}, false},
        {"Brightness", [](Recipe &r, float &) { r.brightness += 0.1; }, true},
        {"Density", [](Recipe &r, float &) { r.density += 0.15; }, true},
        {"Contrast", [](Recipe &r, float &) { r.grade -= 15; }, true},
        {"Shadows", [](Recipe &r, float &) { r.shadows = 0.3; }, true},
        {"Highlights", [](Recipe &r, float &) { r.highlights = -0.3; }, true},
        {"a burn", [&](Recipe &r, float &) { r.masks = {burn}; }, true},
        {"the burn's own grade", [](Recipe &r, float &) { r.masks[0].grade = -10; }, true},
        {"Exposure: the film again", [](Recipe &, float &ev) { ev += 0.4; }, false},
        {"Brightness after it", [](Recipe &r, float &) { r.brightness -= 0.05; }, true},
        {"a crop", [](Recipe &r, float &) { r.geometry.crop = RectD{0.1, 0.1, 0.7, 0.75}; }, false},
        {"Brightness on the crop", [](Recipe &r, float &) { r.brightness += 0.05; }, true},
        {"another crop", [](Recipe &r, float &) { r.geometry.crop = RectD{0.2, 0.15, 0.6, 0.7}; }, false},
        {"Shadows on it", [](Recipe &r, float &) { r.shadows = 0.1; }, true},
        {"the crop taken off", [](Recipe &r, float &) { r.geometry = FrameGeometry(); }, false},
        {"another paper", [](Recipe &r, float &) { r.printStock = "kodak_supra_endura"; }, false},
        {"Grain: the film again", [](Recipe &r, float &) { r.grainAmount = 0.5; }, false},
        {"Brightness last", [](Recipe &r, float &) { r.brightness += 0.02; }, true},
    };
    std::printf("the print's exposure, kept between renders, prints what a fresh render does\n");
    FilmRenderer keeping(rhi, FilmRenderer::Blur::Own);
    keeping.setSkipsUnchangedFilm(true);
    keeping.setKeptExposureBudget(qint64(1) << 30);
    keeping.setPicture(mac.input.floats.data(), w, h);
    Recipe recipe = start.resolved;
    float ev = start.exposureEV;
    int failures = 0;
    for (const Step &step : steps) {
        step.edit(recipe, ev);
        BuiltSetup built = builder.build(recipe, ev, w, h);
        RenderSetup &s = built.setup;
        s.input = Dump();                   // the picture set, not set again
        s.fileSeed = start.setup.fileSeed;  // as RenderEngine::setupFor seeds the grain
        s.grainSeed = fnv1a(recipe.filmStock) ^ (s.fileSeed * 0x9E3779B9u);
        shapeSetup(s, GeometryMap(recipe.geometry, w, h));
        std::vector<float> map;
        const maskraster::Size size = maskraster::mapSize(w, h);
        if (!recipe.masks.empty()) {
            const std::vector<float> cover = maskraster::coverage(recipe.masks[0], size.width, size.height);
            map = maskraster::combine({{&recipe.masks[0], &cover}}, size.width, size.height);
        }
        keeping.setDodgeBurn(map, size.width, size.height);
        keeping.load(s);
        std::vector<uint8_t> displayKept, displayFresh;
        std::vector<float> linearKept, linearFresh;
        keeping.render(&displayKept, &linearKept);
        const bool fromKept = keeping.printedFromKeptExposure();
        FilmRenderer fresh(rhi, FilmRenderer::Blur::Own);
        fresh.setPicture(mac.input.floats.data(), w, h);
        fresh.setDodgeBurn(map, size.width, size.height);
        fresh.load(s);
        fresh.render(&displayFresh, &linearFresh);
        float worst = 0;
        for (size_t i = 0; i < std::min(linearKept.size(), linearFresh.size()); ++i)
            worst = std::max(worst, std::abs(linearKept[i] - linearFresh[i]));
        const bool same = displayKept == displayFresh && linearKept == linearFresh;
        const bool ok = same && fromKept == step.fromKept;
        failures += ok ? 0 : 1;
        std::printf("   %s %-26s %s, %s\n", ok ? "ok  " : "FAIL", step.name,
                    same ? "identical" : qPrintable(QStringLiteral("differs by %1").arg(double(worst))),
                    fromKept ? "printed from the kept exposure" : "kept anew");
    }
    std::printf(failures ? "\n%d step(s) failed\n" : "\nall steps pass\n", failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    auto option = [&](const QString &name, const QString &fallback) {
        const qsizetype i = args.indexOf(name);
        if (i < 0 || i + 1 >= args.size())
            return fallback;
        const QString value = args[i + 1];
        args.remove(i, 2);
        return value;
    };
    const QString packs = option("--packs", PackLibrary::defaultDirectory());
    const QString backend = option("--backend", kDefaultBackend);
    const bool render = !args.removeAll("--no-render");
    const bool kept = args.removeAll("--kept") > 0;
    if (args.size() != 1) {
        std::fprintf(stderr, "usage: flxport-setup <dumpdir> [--packs DIR] [--backend metal|d3d12|d3d11|vulkan] [--no-render]\n");
        return 2;
    }

    try {
        const RenderSetup mac = loadSetup(args[0]);
        QFile f(QDir(args[0]).filePath("recipe.json"));
        if (!f.open(QIODevice::ReadOnly))
            throw std::runtime_error("no recipe.json in the dump: make it with a newer flxref");
        const Recipe recipe = recipeFromJson(QJsonDocument::fromJson(f.readAll()).object());

        PackLibrary library(packs);
        SetupBuilder builder(library);
        const std::string fileName = mac.json.value("input").toString().toStdString();
        builder.prepare(recipe, mac.input, fileName);       // the packs read once
        QElapsedTimer clock;
        clock.start();
        BuiltSetup built = builder.prepare(recipe, mac.input, fileName);
        const double setupSeconds = double(clock.nsecsElapsed()) / 1e9;
        clock.restart();
        builder.build(built.resolved, built.exposureEV, mac.width, mac.height);
        const double buildSeconds = double(clock.nsecsElapsed()) / 1e9;
        RenderSetup &port = built.setup;
        port.input = mac.input;
        port.directory = mac.directory;
        if (kept) {
            std::unique_ptr<QRhi> rhi = makeRhi(backend);
            if (!rhi)
                throw std::runtime_error("cannot start " + backend.toStdString());
            return keptChecks(builder, built, mac, rhi.get());
        }

        std::printf("%s: %s on %s — the port's setup against the Mac engine's\n", fileName.c_str(),
                    recipe.filmStock.c_str(), recipe.printStock.c_str());
        const QJsonObject resolved = mac.json.value("resolved").toObject();
        auto number = [&](const char *key) { return float(resolved.value(key).toDouble()); };
        const float macResolved[3] = {number("exposureEV"), number("brightness"), number("grade")};
        const float portResolved[3] = {built.exposureEV, float(built.resolved.brightness), float(built.resolved.grade)};
        compare("meters: EV, brightness, grade", macResolved, portResolved, 3);
        const QJsonArray nf = resolved.value("neutralFilters").toArray();
        const float macNeutral[3] = {float(nf[0].toDouble()), float(nf[1].toDouble()), float(nf[2].toDouble())};
        const float portNeutral[3] = {built.neutralFilters.x, built.neutralFilters.y, built.neutralFilters.z};
        compare("neutral filtration", macNeutral, portNeutral, 3);
        const QJsonObject tg = resolved.value("toneGeometry").toObject();
        const tonecurve::Geometry &g = built.toneGeometry;
        const float macTone[6] = {float(tg.value("densityMin").toDouble()), float(tg.value("densityMax").toDouble()),
                                  float(tg.value("range").toDouble()), float(tg.value("scale").toDouble()),
                                  float(tg.value("anchor").toDouble()), float(tg.value("midGrey").toDouble())};
        const float portTone[6] = {g.densityMin, g.densityMax, g.range, g.scale, g.anchor, g.midGrey()};
        compare("tone geometry", macTone, portTone, 6);
        compareBlock("film_expose uniforms", mac.film, port.film);
        compare("exposure gradients", reinterpret_cast<const float *>(mac.gradients.data()),
                reinterpret_cast<const float *>(port.gradients.data()),
                std::min(mac.gradients.size(), port.gradients.size()) * 4);
        compareBlock("develop uniforms", mac.develop, port.develop);
        compareBlock("print uniforms", mac.print, port.print);
        compareBlock("coupler matrix", mac.couplerMatrix, port.couplerMatrix);
        compare("spectral tables", mac.spectral.floats, port.spectral.floats);
        compare("density curves", mac.curves.floats, port.curves.floats);
        compare("pre-coupler curves", mac.curves0.floats, port.curves0.floats);
        compare("edge-gain scale", mac.edgeScale.floats, port.edgeScale.floats);
        compare("tone LUT", mac.toneLut.floats, port.toneLut.floats);
        compare("neutral shift", mac.toneShift.floats, port.toneShift.floats);
        compare("tcLut", mac.tcLut.floats, port.tcLut.floats);
        compare("Selwyn table", mac.selwyn.floats, port.selwyn.floats);
        const float macPitch[1] = {mac.pixelSizeUm}, portPitch[1] = {port.pixelSizeUm};
        compare("pixel pitch", macPitch, portPitch, 1);
        compareInts("stages",
                    {mac.stages.halation, mac.stages.couplers, mac.stages.grain, mac.stages.shaped, mac.stages.effects,
                     mac.stages.dodgeBurn},
                    {port.stages.halation, port.stages.couplers, port.stages.grain, port.stages.shaped,
                     port.stages.effects, port.stages.dodgeBurn});
        compareInts("grain seed", {mac.grainSeed}, {port.grainSeed});
        std::printf("worst: %.3g of its largest value, in %s\n", g_worst, qPrintable(g_worstItem));
        std::printf("the setup in %.1f ms with the meters, %.1f ms without\n", setupSeconds * 1000, buildSeconds * 1000);

        if (render) {
            std::unique_ptr<QRhi> rhi = makeRhi(backend);
            if (!rhi)
                throw std::runtime_error("cannot start " + backend.toStdString());
            FilmRenderer renderer(rhi.get(), FilmRenderer::Blur::Own);
            renderer.load(port);
            FilmRenderer::Taps taps;
            const double first = renderer.render(&taps);
            double fastest = first;
            for (int i = 0; i < 3; ++i)
                fastest = std::min(fastest, renderer.render(nullptr));
            const QDir out(QDir(mac.directory).filePath("port-" + backend + "-ownsetup"));
            if (!out.mkpath("."))
                throw std::runtime_error("cannot make " + out.path().toStdString());
            writeDump(out.filePath("log_e_film.f32"), port.width, port.height, 4, taps.logE.data());
            writeDump(out.filePath("cmy_film.f32"), port.width, port.height, 4, taps.cmy.data());
            writeDump(out.filePath("rgb_out.f32"), port.width, port.height, 4, taps.rgbOut.data());
            writeDump(out.filePath("display.u8"), port.width, port.height, 4, taps.display.data());
            const QJsonObject report{{"backend", backend}, {"device", QString::fromUtf8(rhi->driverInfo().deviceName)},
                                     {"qt", qVersion()}, {"blur", "own"}, {"setup", "port"},
                                     {"passes", renderer.passCount()}, {"firstFrameSeconds", first},
                                     {"frameSeconds", fastest}, {"setupSeconds", setupSeconds},
                                     {"buildSeconds", buildSeconds}};
            QFile r(out.filePath("report.json"));
            if (!r.open(QIODevice::WriteOnly) || r.write(QJsonDocument(report).toJson()) < 0)
                throw std::runtime_error("cannot write report.json");
            std::printf("rendered with the port's setup on %s -> %s\n", qPrintable(backend),
                        qPrintable(QDir::toNativeSeparators(out.path())));
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
