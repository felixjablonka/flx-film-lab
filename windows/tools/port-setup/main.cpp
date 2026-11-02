// flxport-setup: the port's own render setup, made from the film packs and
// the recipe a dump started from (recipe.json, in the Mac app's own format)
// on the dump's picture, meters and all; checked value by value against the
// setup the Mac engine made, and rendered, so its stages can be compared
// with the Mac's (tools/compare) like any port render.
//
//   flxport-setup <dumpdir> [--packs DIR] [--backend metal|d3d12|d3d11|vulkan] [--no-render]
//
// The packs default to the Mac app's (PackLibrary::defaultDirectory). The
// render goes into port-<backend>-ownsetup/.

#include "FilmRenderer.h"
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
