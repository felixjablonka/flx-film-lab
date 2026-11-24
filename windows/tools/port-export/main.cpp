// flxport-export: a clip graded and written as the app's Export does
// (engine/ClipExport): the look baked for the clip's format, every frame
// through the frame pipeline at full size, the sound copied.
//
//   flxport-export <movie> <out.mov> [--codec prores|hevc] [--compression FLAVOUR|LEVEL|MBITS]
//                  [--display windows|apple|bt1886] [--seconds N] [--film STOCK] [--exposure EV] [--bloom] [--glow]
//                  [--grain 16|35|70] [--grain-amount A] [--grain-size S] [--packs DIR] [--backend NAME]
//
// --compression: ProRes's Proxy, LT, 422 or HQ; HEVC's Smaller, Standard, High or Maximum, or a rate in
// Mbit/s of its own. It says the rate the file came out at and, for HEVC, the level and tier it is labelled.

#include "ClipExport.h"
#include "HEVCLevels.h"
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
    const QString compression = option("--compression", {});
    const QString grainGauge = option("--grain", {});
    const double grainAmount = option("--grain-amount", "1").toDouble();
    const double grainSize = option("--grain-size", "1").toDouble();
    const QString display = option("--display", "windows");
    const double seconds = option("--seconds", "0").toDouble();
    const QString film = option("--film", "kodak_portra_800");
    const QString exposure = option("--exposure", {});
    const bool bloom = flag("--bloom"), glow = flag("--glow");
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: flxport-export <movie> <out.mov> [--codec prores|hevc] [--compression FLAVOUR|LEVEL|MBITS] "
                             "[--display windows|apple|bt1886] [--seconds N] [--film STOCK] [--exposure EV] [--bloom] "
                             "[--glow] [--grain 16|35|70] [--grain-amount A] [--grain-size S]\n");
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
        if (!compression.isEmpty()) {
            bool chosen = false;
            for (ProResFlavour f : proResFlavours())
                if (compression.compare(QString::fromStdString(proResFlavourName(f)), Qt::CaseInsensitive) == 0) {
                    options.proRes = f;
                    chosen = true;
                }
            for (HEVCLevel l : hevcLevels())
                if (compression.compare(QString::fromStdString(hevcLevelName(l)), Qt::CaseInsensitive) == 0) {
                    options.hevcLevel = l;
                    chosen = true;
                }
            bool number = false;
            const double megabits = compression.toDouble(&number);
            if (number) {
                options.hevcLevel = HEVCLevel::Custom;
                options.hevcCustomMegabits = megabits;
                chosen = true;
            }
            if (!chosen)
                throw std::runtime_error("no compression \"" + compression.toStdString() + "\"");
        }
        // the look's grain, made for the recipe it prints with
        std::optional<PrintGrainTable> grainTable;
        if (!grainGauge.isEmpty()) {
            MotionGrain grain;
            grain.enabled = true;
            grain.gauge = grainGauge == "16" ? FilmGauge::Sixteen : grainGauge == "70" ? FilmGauge::Seventy
                                                                                      : FilmGauge::ThirtyFive;
            grain.amount = grainAmount;
            grain.size = grainSize;
            grainTable = baker.printGrain(recipe);
            options.grainTable = grainTable ? &*grainTable : nullptr;
            options.grain = grain;
        }
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
        // what came out: the codec, its rate against the one asked for, HEVC's label
        const ClipInfo written = readClipInfo(args[1].toStdString());
        const double bytes = double(QFileInfo(args[1]).size());
        const ExportSettings settings = options.settings();
        std::printf("  %s, %s: asked %.1f Mbit/s, wrote %.1f (video %.1f)%s%s\n", written.codecName.c_str(),
                    settings.title().c_str(), settings.megabits(info.width, info.height, info.frameRate),
                    bytes * 8 / std::max(written.duration, 1e-6) / 1e6, written.dataRate / 1e6,
                    options.codec == ExportCodec::HEVC10 ? ", " : "",
                    options.codec == ExportCodec::HEVC10
                        ? hevclevels::labelOf(args[1].toStdString()).value_or(hevclevels::Label{}).description().c_str()
                        : "");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
