// flxport-motion: Motion's pipeline on one frame of a clip (engine/Motion):
// the clip read, its format taken from its tags, the look baked for it, the
// frame at a time graded (and Glow's effects, with --bloom, --glow) into a
// PNG for the screen and, with --linear, display light as a float dump for
// tools/compare.
//
//   flxport-motion <movie> <out.png> [--at SECONDS] [--width PX] [--film STOCK] [--exposure EV]
//                  [--linear OUT.f32] [--bloom] [--glow] [--log] [--packs DIR] [--backend NAME]

#include "Dump.h"
#include "Motion.h"
#include "SetupBuilder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
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
    const double at = option("--at", "1").toDouble();
    const int width = option("--width", "1920").toInt();
    const QString film = option("--film", "kodak_portra_800");
    const QString exposure = option("--exposure", {});
    const QString linearPath = option("--linear", {});
    const bool bloom = flag("--bloom"), glow = flag("--glow"), log = flag("--log");
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: flxport-motion <movie> <out.png> [--at SECONDS] [--width PX] [--film STOCK] "
                             "[--exposure EV] [--linear OUT.f32] [--bloom] [--glow] [--log]\n");
        return 2;
    }
    try {
        const std::string path = args[0].toStdString();
        const ClipInfo info = readClipInfo(path);
        const LogFormat format = info.detection().first;
        PackLibrary packs(packsDir);
        MotionLook look = MotionLook::initial();
        look.recipe.filmStock = film.toStdString();
        const ProfilePack &f = packs.pack(look.recipe.filmStock);
        if (!f.targetPrint.empty())
            look.recipe.printStock = f.targetPrint;
        look.recipe.grade = f.defaultGrade();
        look.effects.bloom.enabled = bloom;
        look.effects.glow.enabled = glow;
        auto rhi = makeRhi(backend);
        if (!rhi)
            throw std::runtime_error("cannot make the GPU device");
        // the meter's exposure, as Auto exposure has it, unless given
        SetupBuilder builder(packs);
        double ev = 0;
        if (!exposure.isEmpty()) {
            ev = exposure.toDouble();
        } else if (const auto h = clipHighlight(path, look.input, format)) {
            ev = clipExposure(*h, builder.highlightHeadroomStops(f));
        }
        Recipe recipe = look.recipe;
        recipe.exposureEV = ev;
        LookBaker baker(rhi.get(), packs);
        QElapsedTimer clock;
        clock.start();
        const LookLUT lut = baker.bake(recipe, 65, lutInputOf(format), LutOutput::BT1886);
        const double bakeMs = double(clock.nsecsElapsed()) / 1e6;
        ClipReader reader(path);
        const auto frame = reader.frameAt(at);
        if (!frame)
            throw std::runtime_error("no frame at that time");
        const FrameReading reading = readingOf(*frame, look.input, format);
        FramePipeline pipeline(rhi.get());
        pipeline.setLUT(lut);
        const QSize size(width, int(std::lround(double(width) * frame->height / frame->width)));
        // the effects' sizes follow the graded frame's long edge
        EffectsParams e;
        Recipe holder;
        holder.effects = look.effects;
        if (look.effects.bloom.isActive()) {
            const double sigma = blurSigmaFraction(look.effects.bloom.size) * std::max(size.width(), size.height());
            e.bloom = look.effects.bloom.amount > 0;
            e.bloomAmount = float(look.effects.bloom.amount / 100);
            e.bloomSigma = float(sigma);
            e.bloomT0 = float(look.effects.bloom.threshold / 100);
            e.bloomT1 = float(e.bloomT0 + (1 - e.bloomT0) * look.effects.bloom.softness / 100);
        }
        if (look.effects.glow.isActive()) {
            e.glow = true;
            e.glowAmount = float(look.effects.glow.amount / 100);
            e.glowSigma = float(blurSigmaFraction(look.effects.glow.size) * std::max(size.width(), size.height()));
            e.glowT0 = float(look.effects.glow.threshold / 100);
        }
        const FramePipeline::Mode mode = log ? FramePipeline::Mode::Log : FramePipeline::Mode::Graded;
        double seconds = 0;
        const std::vector<uint8_t> rgba = pipeline.renderScreen(*frame, reading, size, mode, e, &seconds);
        seconds = 0;
        pipeline.renderScreen(*frame, reading, size, mode, e, &seconds);     // the second, kernels built
        QImage image(rgba.data(), size.width(), size.height(), QImage::Format_RGBA8888);
        if (!image.save(args[1]))
            throw std::runtime_error("cannot write " + args[1].toStdString());
        if (!linearPath.isEmpty()) {
            const std::vector<float> linear = pipeline.renderLinear(*frame, reading, size, mode, e);
            writeDump(linearPath, size.width(), size.height(), 4, linear.data());
        }
        std::printf("%s: %s read as %s, %+.2f EV (%s), the look baked in %.0f ms; frame %.3f s graded at %dx%d in "
                    "%.1f ms on %s\n",
                    QFileInfo(args[0]).fileName().toUtf8().constData(), info.summary().c_str(),
                    logFormatName(format).c_str(), ev, exposure.isEmpty() ? "metered" : "given", bakeMs, frame->time,
                    size.width(), size.height(), seconds * 1e3, rhi->backendName());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
