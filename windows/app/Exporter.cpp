#include "Exporter.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

QString formatName(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Tiff16: return QStringLiteral("TIFF (16-bit)");
    case ExportFormat::Jpeg: return QStringLiteral("JPEG");
    case ExportFormat::Png: return QStringLiteral("PNG");
    }
    return {};
}

QString formatExtension(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Tiff16: return QStringLiteral("tif");
    case ExportFormat::Jpeg: return QStringLiteral("jpg");
    case ExportFormat::Png: return QStringLiteral("png");
    }
    return {};
}

namespace {

[[noreturn]] void fail(const QString &what)
{
    throw std::runtime_error(what.toStdString());
}

// Rows split over the processor's threads.
template<typename F>
void parallelRows(int height, F &&rows)
{
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] { rows(height * t / threads, height * (t + 1) / threads); });
    for (std::thread &th : pool)
        th.join();
}

// The output space's curve: sRGB's piecewise one for sRGB, Display P3 and
// Rec.2020 (encoded as the screen's are), a pure 1.8 power for ProPhoto.
float encode(float v, bool proPhoto)
{
    v = std::max(v, 0.0f);
    if (proPhoto)
        return std::pow(v, 1 / 1.80078125f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
}

// A baseline TIFF, little-endian, uncompressed RGB at 16 bits, the profile in
// tag 34675 and the photograph's EXIF and GPS in directories of their own, as
// ImageIO writes the Mac's.
void writeTiff(const std::vector<uint16_t> &pixels, int width, int height, const QByteArray &icc,
               const ExportMetadata &metadata, bool srgb, const QString &path)
{
    const int rowsPerStrip = std::max(1, (1 << 20) / std::max(1, width * 6));
    const int strips = (height + rowsPerStrip - 1) / rowsPerStrip;
    std::vector<uint32_t> counts(static_cast<size_t>(strips)), offsets(static_cast<size_t>(strips), 0);
    for (int s = 0; s < strips; ++s)
        counts[size_t(s)] = uint32_t(std::min(rowsPerStrip, height - s * rowsPerStrip)) * uint32_t(width) * 6;

    ExifDirectories ex = exifDirectories(metadata, width, height, srgb);
    TiffDirectory ifd0 = ex.ifd0;
    for (TiffEntry e : {tiffEntry(256, 4, 1, tiffLongs({uint32_t(width)})),
                        tiffEntry(257, 4, 1, tiffLongs({uint32_t(height)})),
                        tiffEntry(258, 3, 3, tiffShorts({16, 16, 16})),
                        tiffEntry(259, 3, 1, tiffShorts({1})),
                        tiffEntry(262, 3, 1, tiffShorts({2})),
                        tiffEntry(277, 3, 1, tiffShorts({3})),
                        tiffEntry(278, 4, 1, tiffLongs({uint32_t(rowsPerStrip)})),
                        tiffEntry(279, 4, uint32_t(strips), tiffLongs(counts)),
                        tiffEntry(282, 5, 1, tiffLongs({72, 1})),
                        tiffEntry(283, 5, 1, tiffLongs({72, 1})),
                        tiffEntry(284, 3, 1, tiffShorts({1})),
                        tiffEntry(296, 3, 1, tiffShorts({2}))})
        ifd0.push_back(e);
    if (!icc.isEmpty())
        ifd0.push_back(tiffEntry(34675, 7, uint32_t(icc.size()), icc));
    TiffEntry exifPointer = tiffEntry(34665, 4, 1, {});
    exifPointer.directory = 1;
    ifd0.push_back(exifPointer);
    std::vector<TiffDirectory> directories{ifd0, ex.exif};
    if (!ex.gps.empty()) {
        TiffEntry gpsPointer = tiffEntry(34853, 4, 1, {});
        gpsPointer.directory = 2;
        directories[0].push_back(gpsPointer);
        directories.push_back(ex.gps);
    }
    // the strips follow the directories: laid out once to find where they
    // end, and again with the strips' offsets in
    auto withOffsets = [&](const std::vector<uint32_t> &o) {
        std::vector<TiffDirectory> d = directories;
        d[0].push_back(tiffEntry(273, 4, uint32_t(strips), tiffLongs(o)));
        return layoutDirectories(d, 8);
    };
    const uint32_t start = 8 + uint32_t(withOffsets(offsets).size());
    uint32_t next = start + (start % 2);
    for (int s = 0; s < strips; ++s) {
        offsets[size_t(s)] = next;
        next += counts[size_t(s)];
    }
    QByteArray head("II*\0", 4);
    const uint32_t first = 8;
    head.append(reinterpret_cast<const char *>(&first), 4);
    head.append(withOffsets(offsets));
    if (head.size() % 2)
        head.append('\0');

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    file.write(head);
    file.write(reinterpret_cast<const char *>(pixels.data()), qint64(pixels.size() * 2));
    if (!file.commit())
        fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
}

uint32_t crc32(const QByteArray &bytes)
{
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (const char b : bytes)
        c = table[(c ^ uint8_t(b)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

QByteArray bigEndian(uint32_t v)
{
    const char b[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    return QByteArray(b, 4);
}

// The EXIF into a JPEG Qt wrote: an APP1 segment right after its start.
QByteArray withExifJpeg(const QByteArray &jpeg, const QByteArray &exif)
{
    const QByteArray payload = QByteArray("Exif\0\0", 6) + exif;
    if (jpeg.size() < 2 || uint8_t(jpeg[0]) != 0xFF || uint8_t(jpeg[1]) != 0xD8 || payload.size() + 2 > 65535)
        return jpeg;
    const uint16_t length = uint16_t(payload.size() + 2);
    QByteArray segment("\xFF\xE1", 2);
    segment.append(char(length >> 8));
    segment.append(char(length & 0xFF));
    segment.append(payload);
    return jpeg.left(2) + segment + jpeg.mid(2);
}

// The EXIF into a PNG Qt wrote: an eXIf chunk before its first image data.
QByteArray withExifPng(const QByteArray &png, const QByteArray &exif)
{
    const qsizetype idat = png.indexOf("IDAT");
    if (idat < 12)
        return png;
    QByteArray chunk = bigEndian(uint32_t(exif.size()));
    const QByteArray typed = QByteArray("eXIf", 4) + exif;
    chunk.append(typed);
    chunk.append(bigEndian(crc32(typed)));
    return png.left(idat - 4) + chunk + png.mid(idat - 4);
}

} // namespace

void writeExport(const std::vector<float> &linear, int width, int height, const QString &path, ExportFormat format,
                 const std::string &colorSpace, const ExportMetadata &metadata)
{
    if (linear.size() != size_t(width) * height * 4)
        fail(QStringLiteral("the full-resolution render did not complete"));
    const bool proPhoto = colorSpace == "ProPhoto RGB";
    const QColorSpace space = outputColorSpace(colorSpace);
    if (format == ExportFormat::Tiff16) {
        std::vector<uint16_t> pixels(size_t(width) * height * 3);
        parallelRows(height, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * width; i < size_t(y1) * width; ++i)
                for (int c = 0; c < 3; ++c)
                    pixels[i * 3 + c] = uint16_t(std::clamp(encode(linear[i * 4 + c], proPhoto), 0.0f, 1.0f) * 65535
                                                 + 0.5f);
        });
        writeTiff(pixels, width, height, space.iccProfile(), metadata, colorSpace == "sRGB", path);
        return;
    }
    QImage image(width, height, QImage::Format_RGB888);
    parallelRows(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uchar *row = image.scanLine(y);
            const float *in = &linear[size_t(y) * width * 4];
            for (int x = 0; x < width; ++x)
                for (int c = 0; c < 3; ++c)
                    row[x * 3 + c] = uchar(std::clamp(encode(in[x * 4 + c], proPhoto), 0.0f, 1.0f) * 255 + 0.5f);
        }
    });
    image.setColorSpace(space);
    QBuffer encoded;
    encoded.open(QIODevice::WriteOnly);
    QImageWriter writer(&encoded, format == ExportFormat::Jpeg ? "jpeg" : "png");
    if (format == ExportFormat::Jpeg)
        writer.setQuality(95);
    if (!writer.write(image))
        fail(QStringLiteral("cannot write %1: %2").arg(path, writer.errorString()));
    const QByteArray exif = exifBlock(metadata, width, height, colorSpace == "sRGB", format == ExportFormat::Jpeg);
    const QByteArray bytes = format == ExportFormat::Jpeg ? withExifJpeg(encoded.data(), exif)
                                                          : withExifPng(encoded.data(), exif);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
}
