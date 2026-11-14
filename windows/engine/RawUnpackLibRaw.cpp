#include "RawUnpack.h"

#include <libraw/libraw.h>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &path, const std::string &why)
{
    throw std::runtime_error(path + ": " + why);
}

// dcraw's flip to EXIF's orientation
int exifOrientation(int flip)
{
    switch (flip) {
    case 3: return 3;               // 180
    case 5: return 8;               // 90 counter-clockwise
    case 6: return 6;               // 90 clockwise
    default: return 1;
    }
}

void readMetadata(const LibRaw &raw, RawMosaic &m)
{
    const libraw_data_t &d = raw.imgdata;
    m.orientation = exifOrientation(d.sizes.flip);
    m.exposure.iso = d.other.iso_speed;
    if (std::string(d.idata.make).rfind("Fuji", 0) == 0) {
        const auto &f = d.makernotes.fuji;
        if (std::abs(f.ExpoMidPointShift) < 100)
            m.exposure.fujiRawExposureShift = f.ExpoMidPointShift;
        if (f.DevelopmentDynamicRange != 0xffff)
            m.exposure.fujiDynamicRange = f.DevelopmentDynamicRange;
    }
    if (std::string(d.idata.make).rfind("Canon", 0) == 0)
        m.exposure.canonHighlightTonePriority = d.makernotes.canon.HighlightTonePriority;
    if (d.color.dng_levels.baseline_exposure > -999)
        m.exposure.dngBaselineExposure = d.color.dng_levels.baseline_exposure;
    auto trimmed = [](std::string s) {
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    };
    m.lensMaker = trimmed(d.lens.LensMake);
    m.lensModel = trimmed(d.lens.Lens);
    m.focalLength = d.other.focal_len;
    if (m.focalLength > 0 && d.lens.FocalLengthIn35mmFormat > 0)
        m.cropFactor = float(d.lens.FocalLengthIn35mmFormat) / m.focalLength;
    m.photo.shutter = d.other.shutter;
    m.photo.aperture = d.other.aperture;
    m.photo.focalLength35 = d.lens.FocalLengthIn35mmFormat;
    m.photo.minFocal = d.lens.MinFocal;
    m.photo.maxFocal = d.lens.MaxFocal;
    m.photo.maxApertureAtMin = d.lens.MaxAp4MinFocal;
    m.photo.maxApertureAtMax = d.lens.MaxAp4MaxFocal;
    m.photo.taken = static_cast<long long>(d.other.timestamp);
    m.photo.artist = trimmed(d.other.artist);
    m.photo.description = trimmed(d.other.desc);
    const libraw_gps_info_t &g = d.other.parsed_gps;
    if (g.gpsparsed) {
        m.photo.gps = true;
        for (int i = 0; i < 3; ++i) {
            m.photo.latitude[i] = g.latitude[i];
            m.photo.longitude[i] = g.longitude[i];
        }
        m.photo.altitude = g.altitude;
        m.photo.latitudeRef = g.latref;
        m.photo.longitudeRef = g.longref;
        m.photo.altitudeRef = g.altref;
        m.photo.gpsStatus = g.gpsstatus;
    }
}

} // namespace

void readMetadataWithLibRaw(const std::string &path, RawMosaic &m)
{
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_file(path.c_str()) == LIBRAW_SUCCESS)
        readMetadata(*raw, m);
}

// Formats LibRaw identifies that none of the decoders here read, refused
// before unpacking with what to do instead: Nikon's High Efficiency RAW (HE,
// HE*), coded in JPEG XS, which no open decoder reads yet (its compression,
// 13 or 14, rather than LibRaw's list of models, so newer cameras count
// too); and Sony's lossless compressed RAW at M and S size, which the camera
// stores as YCbCr colour, not as the sensor's mosaic.
void refuseUnreadable(LibRaw &raw)
{
    libraw_decoder_info_t info{};
    raw.get_decoder_info(&info);
    const std::string decoder = info.decoder_name ? info.decoder_name : "";
    const unsigned nef = raw.imgdata.makernotes.nikon.NEFCompression;
    if (decoder == "nikon_he_load_raw()" || nef == 13 || nef == 14)
        throw UnreadableFormat(
            "The Windows app cannot read Nikon's High Efficiency RAW (HE or HE*) yet: none of its decoders "
            "reads that format. Shoot Lossless compressed RAW, or convert the file to DNG with Adobe DNG Converter "
            "(free), which the app opens.");
    if (decoder == "sony_ycbcr_load_raw()")
        throw UnreadableFormat(
            "The Windows app cannot read Sony's lossless compressed RAW at M or S size: the camera stores those as "
            "colour pixels, not as the sensor's mosaic. The L size opens, as do uncompressed and compressed RAW.");
}

RawMosaic unpackWithLibRaw(const std::string &path)
{
    auto raw = std::make_unique<LibRaw>();
    if (const int e = raw->open_file(path.c_str()); e != LIBRAW_SUCCESS)
        fail(path, libraw_strerror(e));
    refuseUnreadable(*raw);
    if (const int e = raw->unpack(); e != LIBRAW_SUCCESS)
        fail(path, libraw_strerror(e));
    const libraw_data_t &d = raw->imgdata;
    if (!d.rawdata.raw_image)
        fail(path, "not a single-plane mosaic (LibRaw reads it, the port does not yet)");
    if (d.idata.filters == 0 || d.idata.colors != 3)
        fail(path, "not a Bayer or X-Trans mosaic (not handled yet)");

    RawMosaic m;
    m.decoder = "libraw";
    m.make = d.idata.make;
    m.model = d.idata.model;
    m.width = d.sizes.raw_width;
    m.height = d.sizes.raw_height;
    m.data.resize(size_t(m.width) * m.height);
    const int pitch = d.sizes.raw_pitch / 2;
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            m.data[size_t(y) * m.width + x] = d.rawdata.raw_image[size_t(y) * pitch + x];

    m.cropLeft = d.sizes.left_margin;
    m.cropTop = d.sizes.top_margin;
    m.cropWidth = d.sizes.width;
    m.cropHeight = d.sizes.height;
    // LibRaw's COLOR() is relative to the image area; the pattern here is
    // the mosaic's, from its origin. Its second green (3) is green. X-Trans
    // (filters 9) has its own, from the origin already.
    if (d.idata.filters == 9) {
        m.cfaSize = 6;
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x)
                m.cfa[y][x] = d.idata.xtrans_abs[y][x];
    } else {
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                const int c = raw->COLOR(y - m.cropTop, x - m.cropLeft);
                m.cfa[(y + 2) % 2][(x + 2) % 2] = c == 3 ? 1 : c;
            }
    }
    // black: the overall level, per channel, and per position of a pattern
    const auto &c = d.color;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            // X-Trans: one level for all, as Fujifilm's are
            const int ch = m.cfaSize == 6 ? 1 : raw->COLOR(y - m.cropTop, x - m.cropLeft);
            float b = float(c.black) + float(c.cblack[ch]);
            if (c.cblack[4] && c.cblack[5])
                b += float(c.cblack[6 + ((y % c.cblack[4]) * c.cblack[5]) + (x % c.cblack[5])]);
            m.black[y][x] = b;
        }
    m.white = float(c.maximum);
    // LibRaw opens some cameras it has no data for: no colour matrix, and a
    // pattern and levels that are guesses
    float matrix = 0;
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            m.xyzToCamera.at(r, k) = c.cam_xyz[r][k];
            matrix += std::abs(c.cam_xyz[r][k]);
        }
    if (matrix == 0)
        fail(path, "LibRaw does not know the camera (" + m.make + " " + m.model + ")");
    m.asShotNeutral = {c.cam_mul[1] / std::max(c.cam_mul[0], 1e-6f), 1.0f, c.cam_mul[1] / std::max(c.cam_mul[2], 1e-6f)};
    readMetadata(*raw, m);
    return m;
}
