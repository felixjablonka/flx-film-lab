#pragma once

#include "LookLUT.h"
#include "Motion.h"

#include <functional>
#include <stdexcept>
#include <string>

class QRhi;

// A graded copy of a clip, written with FFmpeg (ClipExporter.swift).
//
// Every frame goes through the same FramePipeline the preview uses, at full
// size; the audio is copied packet for packet, untouched. The file is a
// QuickTime movie tagged Rec.709, written with the curve the chosen displays
// decode (ExportSettings.display: Windows', Apple's, or BT.1886), and keeps
// the source's rotation and metadata. It is written beside the destination
// and put in its place only once it is complete, so a cancelled or failed
// export leaves nothing behind and replaces nothing.

struct ClipExportOptions {
    ExportCodec codec = ExportCodec::ProRes422HQ;
    LutOutput display = LutOutput::SRGB;
    // ProRes's flavour, HEVC's level and its rate under Custom, as a look's
    // ExportSettings keep them.
    ProResFlavour proRes = ProResFlavour::HQ;
    HEVCLevel hevcLevel = HEVCLevel::Standard;
    double hevcCustomMegabits = 100;
    double seconds = 0;     // only the clip's first `seconds`; 0 for all of it
    // What made the clip (Provenance), as its QuickTime software and
    // description, in place of the camera's; empty leaves the camera's.
    std::string software, description;
    // The look's grain as its print shows it, and the look's settings for it:
    // every frame drawn with grains of its own, numbered by its time as the
    // preview's are (FrameGrain). None, no grain.
    const PrintGrainTable *grainTable = nullptr;
    std::optional<MotionGrain> grain;

    ClipExportOptions() = default;
    // As a look's export settings say.
    explicit ClipExportOptions(const ExportSettings &settings);
    ExportSettings settings() const;
};

struct ExportCancelled : std::runtime_error {
    ExportCancelled() : std::runtime_error("Export cancelled.") {}
};

// `lut` is the print baked for the clip's format (LutOutput::BT1886, as the
// preview's); `effects` are Glow's, as printed. ProRes is written at its
// flavour; HEVC at its level's rate for the clip's size and frame rate, its
// peaks held to what the lowest HEVC level and tier that hold it allow, and
// labelled with them (HEVCLevels.h): the GPUs' encoders are told the level
// and tier, and a file labelled otherwise (VideoToolbox's, which knows no
// High tier) is relabelled once written. `progress` gets the share written,
// 0 to 1, and cancels the export by returning false. Returns the encoder
// that wrote the video ("prores_ks", "hevc_nvenc", ...). Throws
// std::runtime_error, ExportCancelled when cancelled.
std::string exportClip(QRhi *rhi, const std::string &source, const std::string &destination, const LookLUT &lut,
                       const GlowLook &effects, const InputSettings &input, LogFormat format,
                       const ClipExportOptions &options, const std::function<bool(double)> &progress = {});
