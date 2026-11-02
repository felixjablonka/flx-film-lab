#pragma once

#include "LogFormats.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A movie, read with FFmpeg (ClipInfo.swift, and AVFoundation's part of the
// Mac's Motion mode): what the clip is, and its frames as recorded.

enum class Chroma { C420, C422, C444 };
std::string chromaName(Chroma c);  // "4:2:0", ...

struct ClipInfo {
    std::string path;
    int width = 0, height = 0;      // encoded, before the track's rotation
    double frameRate = 0;
    double duration = 0;            // seconds
    int quarterTurns = 0;           // clockwise, as the track asks on playback
    std::string codecName;          // "HEVC", "H.264", "ProRes 422 HQ", ...
    int bitDepth = 0;               // 0 where the file does not say
    Chroma chroma = Chroma::C420;
    std::optional<bool> fullRange;  // none where the file does not say
    std::string matrixTag;          // "BT.2020", "BT.709", "BT.601", or none
    CameraTags camera;              // the log tags, the maker, the model
    bool hasAudio = false;
    double dataRate = 0;            // bits per second

    std::pair<LogFormat, DetectionBasis> detection() const { return detectLogFormat(camera); }
    int displayWidth() const { return quarterTurns % 2 ? height : width; }
    int displayHeight() const { return quarterTurns % 2 ? width : height; }
    std::string summary() const;
};

// Throws std::runtime_error naming the file.
ClipInfo readClipInfo(const std::string &path);

// A frame as recorded: its three planes, each sample the code value the file
// stores (8, 10 or 12 bits, in the low bits of a 16-bit word), no colour
// conversion and no change of range or subsampling.
struct VideoFrame {
    int width = 0, height = 0;
    int chromaWidth = 0, chromaHeight = 0;
    int bitDepth = 10;
    Chroma chroma = Chroma::C420;
    bool fullRange = false;         // the frame's own range
    std::string matrixTag;          // the frame's own matrix, or none
    int chromaLocation = 0;         // FFmpeg's AVChromaLocation
    double time = 0;                // seconds from the clip's start
    int64_t pts = 0;                // as the file stamps it, in the track's time base
    std::vector<uint16_t> planes[3];    // Y, Cb, Cr, rows packed
};

// Decodes a clip's frames, in order or at a time.
class ClipReader
{
public:
    explicit ClipReader(const std::string &path);   // throws std::runtime_error
    ~ClipReader();
    ClipReader(const ClipReader &) = delete;
    ClipReader &operator=(const ClipReader &) = delete;

    // The next frame in order; none at the end.
    std::optional<VideoFrame> next();
    // The frame showing at `seconds`: decoded on from the last one where it
    // is just ahead, else from the keyframe before it.
    std::optional<VideoFrame> frameAt(double seconds);
    double frameDuration() const { return m_frameDuration; }
    // the video track's time base, as a fraction of a second
    std::pair<int, int> timeBase() const;
    // what decodes it: "videotoolbox", "d3d11va", "cuda", "dxva2", or
    // "software"; a GPU's decoder that turns the clip's profile down hands
    // it to software frame by frame
    const std::string &decoder() const { return m_decoder; }

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    double m_frameDuration = 1.0 / 30;
    double m_last = -1;             // the last frame's time
    std::string m_decoder = "software";
};
