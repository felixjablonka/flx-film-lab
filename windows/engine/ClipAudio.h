#pragma once

#include <cstddef>
#include <memory>
#include <string>

// A clip's sound, as the Mac's player plays it with the picture: its first
// audio track decoded with FFmpeg and resampled to interleaved 32-bit float
// at the rate and channel count the speakers take.
class ClipAudio
{
public:
    // Throws std::runtime_error when the clip has no sound this build decodes.
    ClipAudio(const std::string &path, int sampleRate, int channels);
    ~ClipAudio();
    ClipAudio(const ClipAudio &) = delete;
    ClipAudio &operator=(const ClipAudio &) = delete;

    // The sound from `seconds` into the clip on, counted as the picture's
    // frame times are, from the video track's first frame.
    void seek(double seconds);
    // Up to `frames` frames into `out`, `channels` floats each; fewer only at
    // the end.
    size_t read(float *out, size_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
