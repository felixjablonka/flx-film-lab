#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

// What the source file says about the photograph, for the exported file
// (Exporter.exportProperties): when it was taken, with which camera and lens,
// at what settings, where. Zero or empty where the source has none.
struct ExportMetadata {
    QString make, model, lensMake, lensModel, artist, description;
    // What made the print (Provenance): the software with its version, and
    // the film, its filter and the paper, for the user comment and, when the
    // camera left it blank, the description.
    QString software, provenance;
    double shutter = 0, aperture = 0, focalLength = 0;  // seconds, f-number, mm
    int iso = 0, focalLength35 = 0;
    long long taken = 0;            // seconds since 1970, local time as the camera wrote it
    bool gps = false;
    double latitude[3] = {}, longitude[3] = {}, altitude = 0;
    char latitudeRef = 0, longitudeRef = 0, altitudeRef = 0, gpsStatus = 0;
    // Whether the GPS block records where the picture was taken: a camera
    // without a fix still writes one, void, at 0 and 0 (Exporter.hasPosition).
    bool hasPosition() const;
};

// TIFF's directories, little-endian: an entry's tag, type, count and value;
// an entry with `directory` >= 0 holds that directory's offset (a LONG).
struct TiffEntry {
    uint16_t tag, type;
    uint32_t count;
    QByteArray value;
    int directory = -1;
};
using TiffDirectory = std::vector<TiffEntry>;

QByteArray tiffShorts(std::initializer_list<uint16_t> values);
QByteArray tiffLongs(const std::vector<uint32_t> &values);
QByteArray tiffText(const QString &text);              // ASCII, NUL-terminated
TiffEntry tiffEntry(uint16_t tag, uint16_t type, uint32_t count, QByteArray value);

// The directories one after another from `base`, the file's offset where they
// start, each followed by the values too long for its entries. The first
// directory names no next one. `offsets` gets where each landed.
QByteArray layoutDirectories(std::vector<TiffDirectory> directories, uint32_t base,
                             std::vector<uint32_t> *offsets = nullptr);

// The photograph's entries for IFD0 (Make, Model, the date, the credits, the
// orientation reset to upright, the software), and the EXIF and GPS
// directories, as ImageIO writes the Mac's. `srgb`: EXIF names one colour
// space, sRGB; for any other the embedded profile says which.
struct ExifDirectories {
    TiffDirectory ifd0, exif, gps;
};
ExifDirectories exifDirectories(const ExportMetadata &m, int width, int height, bool srgb);

// For a JPEG's APP1 or a PNG's eXIf: a whole little-endian TIFF structure of
// IFD0 and its EXIF and GPS directories. `jpeg`: with the two tags EXIF
// requires of a JPEG, how its colour components are stored.
QByteArray exifBlock(const ExportMetadata &m, int width, int height, bool srgb, bool jpeg);
