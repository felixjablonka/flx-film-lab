// flxport-lut: the port's LUT baker (engine/LookLUT), with the Mac's flxlut
// defaults, so the two can be compared value for value.
//
//   flxport-lut <output.cube> [--film STOCK] [--paper STOCK] [--size N] [--input MODE]
//               [--output bt1886|apple|srgb|linear] [--exposure EV] [--grade R] [--packs DIR]
//               [--backend metal|d3d12|d3d11]
//
// --input takes flxlut's names: code, video-levels, applelog, applelog2, vlog,
// flog, flog2, clog2, clog3, slog3 (S-Gamut3.Cine), slog3-sgamut3, ilog, and
// each but the Apple Logs with -video-levels.

#include "LookLUT.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <rhi/qrhi.h>

#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>

namespace {

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

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    auto option = [&](const QString &name, const QString &fallback) {
        const int i = int(args.indexOf(name));
        if (i < 0 || i + 1 >= args.size())
            return fallback;
        const QString v = args[i + 1];
        args.remove(i, 2);
        return v;
    };
#if defined(Q_OS_WIN)
    const QString backend = option("--backend", "d3d12");
#else
    const QString backend = option("--backend", "metal");
#endif
    const QString packsDir = option("--packs", PackLibrary::defaultDirectory());
    const QString film = option("--film", "kodak_portra_800");
    const QString paper = option("--paper", {});
    const int size = option("--size", "65").toInt();
    const QString inputName = option("--input", "code");
    const QString outputName = option("--output", "bt1886");
    const QString exposure = option("--exposure", {});
    const QString grade = option("--grade", {});
    if (args.size() != 1) {
        std::fprintf(stderr, "usage: flxport-lut <output.cube> [--film STOCK] [--paper STOCK] [--size N] [--input MODE] "
                             "[--output bt1886|apple|srgb|linear] [--exposure EV] [--grade R]\n");
        return 2;
    }
    static const std::map<QString, LutInput> inputs{
        {"code", LutInput::NLogCode}, {"video-levels", LutInput::NLogVideoLevels},
        {"applelog", LutInput::AppleLog}, {"applelog2", LutInput::AppleLog2}, {"vlog", LutInput::VLog},
        {"vlog-video-levels", LutInput::VLogVideoLevels}, {"flog", LutInput::FLog},
        {"flog-video-levels", LutInput::FLogVideoLevels}, {"flog2", LutInput::FLog2},
        {"flog2-video-levels", LutInput::FLog2VideoLevels}, {"clog2", LutInput::CLog2},
        {"clog2-video-levels", LutInput::CLog2VideoLevels}, {"clog3", LutInput::CLog3},
        {"clog3-video-levels", LutInput::CLog3VideoLevels}, {"slog3", LutInput::SLog3Cine},
        {"slog3-video-levels", LutInput::SLog3CineVideoLevels}, {"slog3-sgamut3", LutInput::SLog3},
        {"slog3-sgamut3-video-levels", LutInput::SLog3VideoLevels}, {"ilog", LutInput::ILog},
        {"ilog-video-levels", LutInput::ILogVideoLevels}};
    static const std::map<QString, LutOutput> outputs{{"bt1886", LutOutput::BT1886},
                                                      {"apple", LutOutput::AppleDisplay},
                                                      {"srgb", LutOutput::SRGB},
                                                      {"linear", LutOutput::Linear}};
    try {
        if (!inputs.count(inputName) || !outputs.count(outputName))
            throw std::runtime_error("unknown --input or --output");
        PackLibrary packs(packsDir);
        Recipe recipe;
        recipe.filmStock = film.toStdString();
        const ProfilePack &f = packs.pack(recipe.filmStock);
        recipe.printStock = !paper.isEmpty() ? paper.toStdString()
                          : !f.targetPrint.empty() ? f.targetPrint : "kodak_portra_endura";
        recipe.grade = f.defaultGrade();
        if (!exposure.isEmpty())
            recipe.exposureEV = exposure.toDouble();
        if (!grade.isEmpty())
            recipe.grade = grade.toDouble();
        auto rhi = makeRhi(backend);
        if (!rhi)
            throw std::runtime_error("cannot make the GPU device");
        LookBaker baker(rhi.get(), packs);
        QElapsedTimer clock;
        clock.start();
        const LookLUT lut = baker.bake(recipe, size, inputs.at(inputName), outputs.at(outputName));
        const double ms = double(clock.nsecsElapsed()) / 1e6;
        // a slide or an instant print is seen as it is: no paper in its title
        const std::string text = lut.cubeText(f.isViewedDirectly(recipe.scanFilm)
                                                  ? f.name
                                                  : f.name + " on " + packs.pack(recipe.printStock).name);
        QFile out(args[0]);
        if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), qint64(text.size())) != qint64(text.size()))
            throw std::runtime_error("cannot write " + args[0].toStdString());
        std::printf("%s on %s, %d^3, %s -> %s, baked in %.0f ms on %s\n", f.name.c_str(), recipe.printStock.c_str(),
                    size, lutInputName(lut.input).c_str(), lutOutputName(lut.output).c_str(), ms,
                    rhi->backendName());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
