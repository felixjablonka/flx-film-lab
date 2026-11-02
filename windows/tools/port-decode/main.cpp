// flxport-decode: the port's own RAW decoder, for comparing with Apple's as
// the Mac app uses it (tools/mac-reference/flxdecode, tools/compare/decode_compare.py).
//
//   flxport-decode <raw> --info
//   flxport-decode <raw> --metadata              the metadata alone, as JSON
//   flxport-decode --lens MAKER MODEL LENS FOCAL  the lens correction Lensfun gives a camera and lens
//   flxport-decode <raw> <out.f32> [--size PX] [--reference D55] [--crop LEFT TOP WIDTH HEIGHT] [--ev EV]
//                  [--white-balance film|as-shot|daylight-d55|daylight|tungsten-3200|tungsten|custom] [--temperature K --tint T]
//                  [--camera-rgb] [--sensor-frame] [--decoder rawspeed|libraw] [--no-profile]
//                  [--no-lens-correction] [--lensfun-only] [--no-sharpening] [--no-colour-nr] [--no-moire]
//                  [--gpu [--backend metal|d3d12|d3d11|vulkan]]
//
// <out>.json beside the dump says what was decoded and how. --sensor-frame
// leaves the picture unturned, as the sensor has it. --gpu works the pixels
// on the GPU (GpuDecoder), by default with Metal on a Mac and Direct3D 12 on
// Windows; without it, on the CPU.
//
// RawSpeed decodes, with LibRaw where it can't, unless --decoder names one.
// Its camera list, cameras.xml, sits next to the program, and so does the
// table of Apple's camera profiles, apple-colour.json, unless --no-profile,
// and Lensfun's database, lensfun/, for the lens correction. --lensfun-only
// leaves the camera's own lens data aside.
//
// --camera-rgb writes the camera's own RGB, balanced for daylight but not
// converted, which Apple's colour is measured against (tools/calibrate).

#include "GpuDecoder.h"
#include "RawDecoder.h"
#include "RawUnpack.h"

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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <string>
#include <vector>

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
    // a GUI application only for the GPU, which QRhi needs
    const bool gpu = std::any_of(argv + 1, argv + argc, [](const char *a) { return std::strcmp(a, "--gpu") == 0; });
    std::unique_ptr<QCoreApplication> app = gpu ? std::make_unique<QGuiApplication>(argc, argv)
                                                : std::make_unique<QCoreApplication>(argc, argv);
    const std::string camerasXml = (QCoreApplication::applicationDirPath() + "/cameras.xml").toStdString();
    const std::string appleColour = (QCoreApplication::applicationDirPath() + "/apple-colour.json").toStdString();
    const std::string lensfun = (QCoreApplication::applicationDirPath() + "/lensfun").toStdString();
    std::vector<std::string> args(argv + 1, argv + argc);
    auto take = [&](const std::string &name, int count) {
        std::vector<std::string> values;
        for (size_t i = 0; i + count < args.size(); ++i)
            if (args[i] == name) {
                values.assign(args.begin() + long(i) + 1, args.begin() + long(i) + 1 + count);
                args.erase(args.begin() + long(i), args.begin() + long(i) + 1 + count);
                break;
            }
        return values;
    };
    try {
        if (args.size() == 5 && args[0] == "--lens") {
            LensInfo lens;
            lens.cameraMaker = args[1];
            lens.cameraModel = args[2];
            lens.lensModel = args[3];
            lens.focalLength = std::stof(args[4]);
            const Distortion d = distortionFor("", lens, 6000, 4000, lensfun);
            std::printf("%s\n", d.model == Distortion::None ? "none"
                                 : (d.from + ": k " + std::to_string(d.k[0]) + " " + std::to_string(d.k[1]) + " " +
                                    std::to_string(d.k[2]) + ", radius " + std::to_string(d.radius) + " px of 6000 x 4000, zoom " +
                                    std::to_string(d.zoom)).c_str());
            return 0;
        }
        if (args.size() == 2 && args[1] == "--metadata") {
            RawMosaic m;
            readMetadataWithLibRaw(args[0], m);
            auto number = [](float v) { return std::isnan(v) ? QJsonValue() : QJsonValue(v); };
            const QJsonObject info{
                {"orientation", m.orientation}, {"iso", m.exposure.iso},
                {"fujiRawExposureShift", number(m.exposure.fujiRawExposureShift)},
                {"fujiDynamicRange", m.exposure.fujiDynamicRange},
                {"canonHighlightTonePriority", m.exposure.canonHighlightTonePriority},
                {"dngBaselineExposure", number(m.exposure.dngBaselineExposure)},
                {"lensMaker", QString::fromStdString(m.lensMaker)}, {"lensModel", QString::fromStdString(m.lensModel)},
                {"focalLength", m.focalLength}, {"cropFactor", m.cropFactor}};
            std::printf("%s\n", QJsonDocument(info).toJson(QJsonDocument::Compact).constData());
            return 0;
        }
        if (args.size() == 2 && args[1] == "--info") {
            // both decoders, for comparing what each reads
            for (const char *decoder : {"rawspeed", "libraw"}) {
                try {
                    const RawMosaic m = decoder == std::string("rawspeed") ? unpackWithRawSpeed(args[0], camerasXml)
                                                                          : unpackWithLibRaw(args[0]);
                    std::printf("%s %s, %s: mosaic %d x %d, image %d x %d at (%d, %d), orientation %d\n",
                                m.make.c_str(), m.model.c_str(), m.decoder.c_str(), m.width, m.height, m.cropWidth,
                                m.cropHeight, m.cropLeft, m.cropTop, m.orientation);
                    std::printf("  pattern %d%d%d%d, black %.0f %.0f %.0f %.0f, white %.0f, as shot R %.4f B %.4f\n",
                                m.cfa[0][0], m.cfa[0][1], m.cfa[1][0], m.cfa[1][1], m.black[0][0], m.black[0][1],
                                m.black[1][0], m.black[1][1], m.white, m.asShotNeutral.x, m.asShotNeutral.z);
                    std::printf("  ISO %.0f, Fujifilm raw exposure shift %.2f, dynamic range %d, Canon HTP %d, DNG "
                                "baseline exposure %.2f\n", m.exposure.iso, m.exposure.fujiRawExposureShift,
                                m.exposure.fujiDynamicRange, m.exposure.canonHighlightTonePriority,
                                m.exposure.dngBaselineExposure);
                    std::printf("  XYZ -> camera %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f\n",
                                m.xyzToCamera.at(0, 0), m.xyzToCamera.at(0, 1), m.xyzToCamera.at(0, 2),
                                m.xyzToCamera.at(1, 0), m.xyzToCamera.at(1, 1), m.xyzToCamera.at(1, 2),
                                m.xyzToCamera.at(2, 0), m.xyzToCamera.at(2, 1), m.xyzToCamera.at(2, 2));
                } catch (const std::exception &e) {
                    std::printf("%s: %s\n", decoder, e.what());
                }
            }
            return 0;
        }
        DecodeOptions options;
        options.camerasXml = camerasXml;
        options.appleColour = appleColour;
        options.lensfunDir = lensfun;
        if (auto it = std::find(args.begin(), args.end(), "--no-lens-correction"); it != args.end()) {
            options.lensCorrection = false;
            args.erase(it);
        }
        if (auto it = std::find(args.begin(), args.end(), "--lensfun-only"); it != args.end()) {
            options.cameraLensData = false;
            args.erase(it);
        }
        // the detail steps off, by flxdecode's names for Apple's
        for (const auto &[flag, setting] : {std::pair{"--no-sharpening", &options.sharpening},
                                            std::pair{"--no-colour-nr", &options.colourNoiseReduction},
                                            std::pair{"--no-moire", &options.falseColour}})
            if (auto it = std::find(args.begin(), args.end(), flag); it != args.end()) {
                *setting = false;
                args.erase(it);
            }
        if (auto it = std::find(args.begin(), args.end(), "--no-profile"); it != args.end()) {
            options.appleColour.clear();
            args.erase(it);
        }
        if (auto v = take("--white-balance", 1); !v.empty())
            options.whiteBalance = v[0];
        if (auto v = take("--temperature", 1); !v.empty())
            options.temperature = std::stof(v[0]);
        if (auto v = take("--tint", 1); !v.empty())
            options.tint = std::stof(v[0]);
        if (auto v = take("--decoder", 1); !v.empty())
            options.decoder = v[0];
        if (auto v = take("--size", 1); !v.empty())
            options.maxDimension = std::stoi(v[0]);
        if (auto v = take("--reference", 1); !v.empty())
            options.reference = v[0];
        if (auto v = take("--ev", 1); !v.empty())
            options.exposureEV = std::stof(v[0]);
        if (auto it = std::find(args.begin(), args.end(), "--camera-rgb"); it != args.end()) {
            options.cameraRGB = true;
            args.erase(it);
        }
        if (auto it = std::find(args.begin(), args.end(), "--sensor-frame"); it != args.end()) {
            options.upright = false;
            args.erase(it);
        }
        if (auto v = take("--crop", 4); !v.empty()) {
            options.cropLeft = std::stoi(v[0]);
            options.cropTop = std::stoi(v[1]);
            options.cropWidth = std::stoi(v[2]);
            options.cropHeight = std::stoi(v[3]);
        }
        std::string backend = kDefaultBackend;
        if (auto v = take("--backend", 1); !v.empty())
            backend = v[0];
        args.erase(std::remove(args.begin(), args.end(), "--gpu"), args.end());
        if (args.size() != 2) {
            std::fprintf(stderr, "usage: flxport-decode <raw> <out.f32> [--size PX] [--reference D55] "
                                 "[--crop LEFT TOP WIDTH HEIGHT] [--ev EV] [--camera-rgb] [--sensor-frame] "
                                 "[--decoder rawspeed|libraw] [--no-profile] [--no-lens-correction] [--lensfun-only] "
                                 "[--no-sharpening] [--no-colour-nr] [--no-moire] [--gpu [--backend NAME]]\n");
            return 2;
        }
        const auto start = std::chrono::steady_clock::now();
        double gpuSeconds = 0;
        DecodedPicture d;
        if (gpu) {
            std::unique_ptr<QRhi> rhi = makeRhi(QString::fromStdString(backend));
            if (!rhi)
                throw std::runtime_error("cannot start " + backend);
            GpuDecoder decoder(rhi.get());
            d = decoder.decode(planDecode(args[0], options), &gpuSeconds);
        } else {
            d = decodeRaw(args[0], options);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        writeDump(QString::fromStdString(args[1]), d.pixels.width, d.pixels.height, 4, d.pixels.floats.data());
        const RawMosaic &m = d.mosaic;
        QJsonObject info{
            {"make", QString::fromStdString(m.make)}, {"model", QString::fromStdString(m.model)},
            {"decoder", QString::fromStdString(m.decoder)}, {"width", d.pixels.width}, {"height", d.pixels.height},
            {"mosaicWidth", m.width}, {"mosaicHeight", m.height}, {"orientation", m.orientation},
            {"upright", options.upright}, {"cameraRGB", options.cameraRGB}, {"profiled", d.profiled},
            {"gpu", gpu ? QJsonValue(QString::fromStdString(backend)) : QJsonValue(false)},
            {"noise", QJsonArray{d.noise[0], d.noise[1], d.noise[2], d.noise[3]}},
            {"temperature", d.temperature}, {"tint", d.tint},
            {"decoderCrop", QJsonArray{m.cropLeft, m.cropTop, m.cropWidth, m.cropHeight}},
            {"crop", QJsonArray{d.cropLeft, d.cropTop, d.cropWidth, d.cropHeight}},
            {"black", QJsonArray{m.black[0][0], m.black[0][1], m.black[1][0], m.black[1][1]}},
            {"white", m.white},
            {"whiteBalance", QJsonArray{d.whiteBalance.x, d.whiteBalance.y, d.whiteBalance.z}},
            {"asShotNeutral", QJsonArray{m.asShotNeutral.x, m.asShotNeutral.y, m.asShotNeutral.z}},
        };
        QJsonArray matrix;
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                matrix.append(m.xyzToCamera.at(r, k));
        info["xyzToCamera"] = matrix;
        const Distortion &lens = d.distortion;
        static const char *models[] = {"none", "nikon", "poly3", "poly5", "ptlens", "profile", "dng"};
        info["lens"] = QJsonObject{
            {"maker", QString::fromStdString(m.lensMaker)}, {"model", QString::fromStdString(m.lensModel)},
            {"focalLength", m.focalLength}, {"cropFactor", m.cropFactor},
            {"correction", lens.model == Distortion::None ? QJsonValue()
                           : QJsonValue(QJsonObject{
                                 {"from", QString::fromStdString(lens.from)}, {"model", models[lens.model]},
                                 {"k", QJsonArray{lens.k[0], lens.k[1], lens.k[2], lens.k[3]}},
                                 {"radius", lens.radius}, {"zoom", lens.zoom}})}};
        auto number = [](float v) { return std::isnan(v) ? QJsonValue() : QJsonValue(v); };
        info["exposure"] = QJsonObject{
            {"iso", m.exposure.iso}, {"fujiRawExposureShift", number(m.exposure.fujiRawExposureShift)},
            {"fujiDynamicRange", m.exposure.fujiDynamicRange},
            {"canonHighlightTonePriority", m.exposure.canonHighlightTonePriority},
            {"dngBaselineExposure", number(m.exposure.dngBaselineExposure)}};
        QString jsonPath = QString::fromStdString(args[1]);
        jsonPath = jsonPath.left(jsonPath.lastIndexOf('.')) + ".json";
        QFile json(jsonPath);
        if (!json.open(QIODevice::WriteOnly) || json.write(QJsonDocument(info).toJson()) < 0)
            throw std::runtime_error("cannot write " + jsonPath.toStdString());
        if (gpu)
            std::printf("on the GPU (%s): %.2f s of %.2f\n", backend.c_str(), gpuSeconds, seconds);
        std::printf("%s %s (%s): %d x %d in %.2f s, white balance R %.4f B %.4f, lens %s\n", d.mosaic.make.c_str(),
                    d.mosaic.model.c_str(), d.mosaic.decoder.c_str(), d.pixels.width, d.pixels.height, seconds,
                    d.whiteBalance.x, d.whiteBalance.z,
                    lens.model == Distortion::None ? "not corrected" : ("corrected from " + lens.from).c_str());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
