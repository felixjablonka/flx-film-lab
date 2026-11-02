#pragma once

#include "Dump.h"
#include "FrameGeometry.h"
#include "MaskRasterizer.h"
#include "Packs.h"
#include "RawDecoder.h"
#include "Recipe.h"
#include "SetupBuilder.h"

#include <QColorSpace>
#include <QImage>
#include <QString>

#include <map>
#include <memory>
#include <optional>
#include <string>

class QRhi;
class FilmRenderer;
class GpuDecoder;

// Where the app's data is: the film packs, Apple's colour per camera,
// Lensfun's lens database and RawSpeed's camera list.
struct AppData {
    QString packs, appleColour, lensfun, camerasXml;
};

// One decode: the file and every option that changes its pixels
// (DocumentModel.DecodeKey).
struct DecodeKey {
    QString path;
    std::string whiteBalance = "film";  // DecodeOptions': film, as-shot, daylight, tungsten, custom
    std::string reference = "D55";      // the film's reference illuminant
    double temperature = 5500, tint = 0;
    int maxDimension = 0;               // the long side; 0 keeps the full size
    bool operator==(const DecodeKey &) const = default;
};

// What the meters read, for the controls whose auto is on.
struct Metered {
    std::optional<double> exposureEV, brightness, grade;
};

enum class ExportFormat { Tiff16, Jpeg, Png };

// A GPU device for compute: Metal on a Mac; Direct3D 12 on Windows, else
// Direct3D 11, else Direct3D 11's software rasteriser. Nothing when there is
// none.
std::unique_ptr<QRhi> makeDevice();

// The colour space an output colour space's pixels are encoded in: the sRGB
// curve on each one's primaries, a pure 1.8 power on ProPhoto's
// (Renderer.cgColorSpace, Exporter.transferFunction).
QColorSpace outputColorSpace(const std::string &name);

// The preview's render loop and the export, off the main thread
// (RenderEngine.swift). Its own GPU device is made on the thread that first
// uses it, and every call after comes from that thread. Each stage is
// skipped when nothing it depends on has changed: the picture's upload, the
// auto exposure's reading and the print meters'.
class RenderEngine
{
public:
    explicit RenderEngine(AppData data);
    ~RenderEngine();

    struct Source {
        int id = 0;
        int width = 0, height = 0;
        QString camera;
    };
    // The picture `key` names, decoded at its size. Throws std::runtime_error.
    DecodedPicture decode(const DecodeKey &key);
    // Hold `picture`, decoded from `path`; every cached reading belonged to
    // the last one. Throws std::runtime_error.
    Source setPicture(DecodedPicture &&picture, const QString &path);

    struct Output {
        QImage image;                   // for the screen, in sRGB
        Recipe resolved;                // the recipe as printed, every metered value in
        Metered metered;
        GeometryMap map;                // where the image's pixels come from in the frame
        double milliseconds = 0;
        QString breakdown;              // where the time went
    };
    // `recipe` rendered for the picture `expected`, or nothing if that
    // picture has been replaced. `wholeFrame` shows the whole frame, turned
    // and levelled, for drawing the crop on, and `metering` is the shape the
    // meters read meanwhile. Throws std::runtime_error.
    std::optional<Output> render(const Recipe &recipe, int expected, bool wholeFrame = false,
                                 const std::optional<FrameGeometry> &metering = std::nullopt);

    // The recipe as it prints: each control whose auto is on carries the
    // meter's value in place of the stored one. The meters read only what
    // the print keeps of the frame: the recipe's crop, or `metering`'s.
    std::pair<Recipe, Metered> resolve(const Recipe &recipe,
                                       const std::optional<FrameGeometry> &metering = std::nullopt);

    // The picture at full size with `recipe`, metered on the preview as the
    // screen shows it, written to `path`. Throws std::runtime_error.
    void exportTo(const Recipe &recipe, const DecodeKey &key, const QString &path, ExportFormat format);

    // The backend's name, once there is a device.
    QString backendName() const;

private:
    void ensureDevice();
    // the part of the frame an upright crop keeps, cut once per crop
    const Dump &meteredArea(const FrameGeometry &upright);
    // the print's shape into the setup
    static void shape(RenderSetup &setup, const GeometryMap &map, bool coverage);
    // Dodge & burn's map made again only when the masks change, and of those
    // only the ones whose geometry moved: dragging a mask's exposure or
    // contrast reweights coverages already drawn.
    void updateMasks(const std::vector<LocalMask> &masks);
    BuiltSetup setupFor(const Recipe &resolved, int width, int height, const std::string &fileName);

    AppData m_data;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<GpuDecoder> m_decoder;
    std::unique_ptr<FilmRenderer> m_renderer;
    std::unique_ptr<PackLibrary> m_packs;
    std::unique_ptr<SetupBuilder> m_builder;

    Dump m_picture;                     // the preview's decode, RGBA float32, for the meters
    std::string m_fileName;             // the grain's seed
    int m_sourceID = 0;

    struct MeteredArea {
        FrameGeometry geometry;
        Dump pixels;
    };
    std::optional<MeteredArea> m_meteredArea;
    std::optional<std::vector<LocalMask>> m_lastMasks;
    struct Coverage {
        maskraster::Geometry geometry;
        std::vector<float> alpha;
    };
    std::map<std::string, Coverage> m_coverages;
    struct ExposureReading {
        std::string stock;
        FrameGeometry geometry;
        float ev = 0;
    };
    std::optional<ExposureReading> m_exposureReading;
    // Everything the print meters' reading depends on: it is taken again only
    // when one of these moves, not on every tone or colour slider tick.
    struct PrintReadingKey {
        std::string film, paper;
        std::optional<std::string> lensFilter;
        std::optional<bool> scanFilm;
        double exposureEV = 0, filterY = 0, filterM = 0, gamma = 0;
        FrameGeometry geometry;
        bool operator==(const PrintReadingKey &) const = default;
    };
    std::optional<std::pair<PrintReadingKey, std::optional<SetupBuilder::PrintReading>>> m_printReading;
};
