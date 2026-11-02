#pragma once

#include "ClipExport.h"
#include "Motion.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QRhi;

// The Motion mode's decoding and GPU work, off the main thread
// (PreviewRenderer, LookBakeEngine): the clip on screen read with FFmpeg, its
// look baked, the frame showing graded for the screen. Its own GPU device is
// made on the thread that first uses it, and every call after comes from
// that thread.
class MotionEngine
{
public:
    explicit MotionEngine(QString packsDir);
    ~MotionEngine();

    // Read `path`, which becomes clip `id`; the clip shown before stays
    // until then. Throws std::runtime_error.
    ClipInfo open(const QString &path, int id);

    // Everything the frame on screen is made from.
    struct Request {
        int clip = 0;
        double time = 0;            // the frame showing then
        Recipe recipe;              // as it prints, the meter's exposure in
        LogFormat format = LogFormat::NLog;
        InputSettings input;        // with the format resolved
        GlowLook effects;           // as this film and paper print them
        FramePipeline::Mode mode = FramePipeline::Mode::Graded;
        QSize size;                 // the graded frame's, the clip's way up
    };
    struct Output {
        int clip = 0;
        QImage image;               // sRGB, the clip's way up; null when nothing changed
        std::shared_ptr<const VideoFrame> frame;
        FrameReading reading;
        double milliseconds = 0;
        Recipe baked;               // the recipe the look on screen was baked from
        LogFormat bakedFormat = LogFormat::NLog;
    };
    // Nothing when `request` is for a clip no longer open. Throws
    // std::runtime_error.
    std::optional<Output> render(const Request &request);

    // The look as a .cube for other software. Throws std::runtime_error.
    std::string cube(const Recipe &recipe, LutInput input, const std::string &title,
                     const std::vector<std::string> &comments);

private:
    void ensureDevice();

    QString m_packsDir;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<PackLibrary> m_packs;
    std::unique_ptr<LookBaker> m_baker;
    std::unique_ptr<FramePipeline> m_pipeline;
    std::unique_ptr<ClipReader> m_reader;
    int m_clip = 0;
    std::shared_ptr<const VideoFrame> m_frame;
    std::optional<std::pair<Recipe, LogFormat>> m_baked;
    std::optional<Request> m_shown;     // what the last image was made from
};

// An export: the look baked once more from exactly the settings it was
// started with, then every frame written (ClipModel.export). Runs on the
// calling thread with a device of its own, so the preview carries on.
// Returns the encoder used. Throws std::runtime_error, ExportCancelled.
std::string runClipExport(const QString &packsDir, const QString &source, const QString &destination,
                          const MotionLook &look, LogFormat format, const std::function<bool(double)> &progress);
