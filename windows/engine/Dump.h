#pragma once

#include <QString>

#include <cstdint>
#include <vector>

// The dump format shared with tools/mac-reference and tools/compare: a 32-byte
// header — "FLXD", version 1, width, height, channels, type (0 float32, 1
// uint8), two zeros — then the values, row-major, little-endian.
struct Dump {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> floats;      // type 0
    std::vector<uint8_t> bytes;     // type 1

    size_t count() const { return size_t(width) * height * channels; }
};

// Throws std::runtime_error naming the file when it is missing or malformed.
Dump readDump(const QString &path);
void writeDump(const QString &path, int width, int height, int channels, const float *values);
void writeDump(const QString &path, int width, int height, int channels, const uint8_t *values);
