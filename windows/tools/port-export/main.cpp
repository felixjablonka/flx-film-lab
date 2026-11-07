// flxport-export: a clip graded and written as the app's Export does
// (engine/ClipExport): the look baked for the clip's format, every frame
// through the frame pipeline at full size, the sound copied.
//
//   flxport-export <movie> <out.mov> [--codec prores|hevc] [--display windows|apple|bt1886] [--seconds N]
//                  [--film STOCK] [--exposure EV] [--bloom] [--glow] [--packs DIR] [--backend NAME]

#include "ClipExport.h"
#include "Provenance.h"
#include "SetupBuilder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <rhi/qrhi.h>

#include <cstdio>
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
    auto flag = [&](const QString &name) { return args.removeAll(name) > 0; };
#if defined(Q_OS_WIN)
    const QString backend = option("--backend", "d3d12");
#else
    const QString backend = option("--backend", "metal");
#endif
    const QString packsDir = option("--packs", PackLibrary::defaultDirectory());
    const QString codec = option("--codec", "prores");
    const QString display = option("--display", "windows");
    const double seconds = option("--seconds", "0").toDouble();
    const QString film = option("--film", "kodak_portra_800");
    const QString exposure = option("--exposure", {});
    const bool bloom = flag("--bloom"), glow = flag("--glow");
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: flxport-export <movie> <out.mov> [--codec prores|hevc] [--display windows|apple|bt1886] "
                             "[--seconds N] [--film STOCK] [--exposure EV] [--bloom] [--glow]\n");
        return 2;
    }
    try {
        const std::string path = args[0].toStdString();
        const ClipInfo info = readClipInfo(path);
        const LogFormat format = info.detection().first;
        PackLibrary packs(packsDir);
        MotionLook look = MotionLook::initial();
        look.recipe = loadingFilm(look.recipe, film.toStdString(), packs);
        const ProfilePack &f = packs.pack(look.recipe.filmStock);
        look.effects.bloom.enabled = bloom;
        look.effects.glow.enabled = glow;
        auto rhi = makeRhi(backend);
        if (!rhi)
            throw std::runtime_error("cannot make the GPU device");
        SetupBuilder builder(packs);
        double ev = 0;
        if (!exposure.isEmpty())
            ev = exposure.toDouble();
        else if (const auto h = clipHighlight(path, look.input, format))
            ev = clipExposure(*h, builder.highlightHeadroomStops(f));
        Recipe recipe = look.recipe;
        recipe.exposureEV = ev;
        LookBaker baker(rhi.get(), packs);
        const LookLUT lut = baker.bake(recipe, 65, lutInputOf(format), LutOutput::BT1886);
        ClipExportOptions options;
        options.codec = codec == "hevc" ? ExportCodec::HEVC10 : ExportCodec::ProRes422HQ;
        options.display = display == "bt1886" ? LutOutput::BT1886
                          : display == "apple" ? LutOutput::AppleDisplay
                                               : LutOutput::SRGB;
        options.seconds = seconds;
        // as the app's export names what made the clip; a tool has no version
        const Provenance made = provenance(recipe, packs, "");
        options.software = made.software;
        options.description = made.description;
        QElapsedTimer clock;
        clock.start();
        int lastShown = -1;
        const std::string encoder = exportClip(
            rhi.get(), path, args[1].toStdString(), lut, effectsAsPrinted(look, packs), look.input, format, options,
            [&](double p) {
                const int percent = int(p * 100);
                if (percent / 10 != lastShown / 10) {
                    std::fprintf(stderr, "  %d%%\n", percent);
                    lastShown = percent;
                }
                return true;
            });
        const double elapsed = double(clock.nsecsElapsed()) / 1e9;
        const double span = seconds > 0 ? std::min(seconds, info.duration) : info.duration;
        std::printf("%s: %s read as %s, %+.2f EV; %.1f s of it written with %s in %.1f s (%.1f fps) on %s -> %s\n",
                    QFileInfo(args[0]).fileName().toUtf8().constData(), info.summary().c_str(),
                    logFormatName(format).c_str(), ev, span, encoder.c_str(), elapsed,
                    span * info.frameRate / std::max(elapsed, 1e-6), rhi->backendName(),
                    args[1].toUtf8().constData());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
