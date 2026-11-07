// flxport-render: the port's engine rendering a setup that flxref
// (tools/mac-reference) wrote, on one GPU backend, and writing the same taps
// into port-<backend>/ beside it for tools/compare.
//
//   flxport-render <dumpdir> [--backend metal|d3d12|d3d11|vulkan] [--blur own|measured] [--repeat N]
//
// --repeat times N more renders without the readbacks and reports the
// fastest; the default backend is Metal on a Mac and Direct3D 12 on Windows.
// --blur measured blurs with the kernels flxref measured from Metal
// Performance Shaders instead of the port's own Gaussian, and writes
// port-<backend>-measured/: the difference between the two runs is the
// port's blur's alone.

#include "FilmRenderer.h"
#include "Setup.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define WITH_VULKAN 1
#include <QVulkanInstance>
#endif

#include <algorithm>
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
#if defined(WITH_VULKAN)
    if (backend == "vulkan") {
        static QVulkanInstance instance;
        instance.setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        if (!instance.isValid() && !instance.create())
            return nullptr;
        QRhiVulkanInitParams params;
        params.inst = &instance;
        return std::unique_ptr<QRhi>(QRhi::create(QRhi::Vulkan, &params));
    }
#endif
    throw std::runtime_error("no backend \"" + backend.toStdString() + "\" in this build");
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
    const QString backend = option("--backend", kDefaultBackend);
    const QString blur = option("--blur", "own");
    const int repeats = option("--repeat", "3").toInt();
    if (args.size() != 1 || (blur != "own" && blur != "measured")) {
        std::fprintf(stderr, "usage: flxport-render <dumpdir> [--backend metal|d3d12|d3d11|vulkan] "
                             "[--blur own|measured] [--repeat N]\n");
        return 2;
    }

    try {
        const RenderSetup setup = loadSetup(args[0]);
        if (setup.stages.shaped || setup.stages.effects || setup.stages.dodgeBurn)
            throw std::runtime_error("the port has no shaped print, Glow effects or dodge & burn yet");

        std::unique_ptr<QRhi> rhi = makeRhi(backend);
        if (!rhi)
            throw std::runtime_error("cannot start " + backend.toStdString());
        const QString device = QString::fromUtf8(rhi->driverInfo().deviceName);

        FilmRenderer renderer(rhi.get(), blur == "measured" ? FilmRenderer::Blur::Measured : FilmRenderer::Blur::Own);
        renderer.load(setup);
        FilmRenderer::Taps taps;
        const double first = renderer.render(&taps);
        double fastest = first;
        for (int i = 0; i < repeats; ++i)
            fastest = std::min(fastest, renderer.render(nullptr));

        // The histogram the preview measures on the GPU (histogram_rgb) has
        // to count the 8-bit values the screen's image holds.
        renderer.setMeasuresHistogram(true);
        renderer.load(setup);
        std::vector<uint8_t> screen;
        FilmRenderer::Histogram histogram;
        renderer.render(&screen, nullptr, &histogram);
        std::vector<uint32_t> counted(768, 0);
        for (size_t i = 0; i + 3 < screen.size(); i += 4)
            for (int c = 0; c < 3; ++c)
                counted[size_t(c) * 256 + screen[i + size_t(c)]]++;
        size_t differ = 0;
        for (size_t i = 0; i < 768; ++i)
            differ += histogram.after.size() == 768 && histogram.after[i] == counted[i] ? 0 : 1;
        std::printf("histogram: %s%s\n",
                    differ == 0 ? "the screen image's own counts" : "differs in bins: ",
                    differ == 0 ? "" : std::to_string(differ).c_str());
        if (setup.printCurves.empty() != histogram.before.empty())
            throw std::runtime_error("the histogram before the curves is missing, or there without curves");

        const QDir out(QDir(setup.directory).filePath("port-" + backend + (blur == "measured" ? "-measured" : "")));
        if (!out.mkpath("."))
            throw std::runtime_error("cannot make " + out.path().toStdString());
        writeDump(out.filePath("log_e_film.f32"), setup.width, setup.height, 4, taps.logE.data());
        writeDump(out.filePath("cmy_film.f32"), setup.width, setup.height, 4, taps.cmy.data());
        writeDump(out.filePath("rgb_out.f32"), setup.width, setup.height, 4, taps.rgbOut.data());
        writeDump(out.filePath("display.u8"), setup.width, setup.height, 4, taps.display.data());
        QJsonArray blurs;
        for (const FilmRenderer::BlurUse &b : renderer.blurs())
            blurs.append(QJsonObject{{"sigma", b.sigma}, {"taps", b.taps}});
        const QJsonObject report{
            {"backend", backend}, {"device", device}, {"qt", qVersion()}, {"blur", blur},
            {"passes", renderer.passCount()}, {"blurs", blurs},
            {"firstFrameSeconds", first}, {"frameSeconds", fastest}, {"repeats", repeats},
        };
        QFile f(out.filePath("report.json"));
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(report).toJson()) < 0)
            throw std::runtime_error("cannot write report.json");
        std::printf("%s on %s: %d x %d, first frame %.3f s (with readback), fastest %.4f s -> %s\n",
                    qPrintable(backend), qPrintable(device), setup.width, setup.height, first, fastest,
                    qPrintable(QDir::toNativeSeparators(out.path())));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
