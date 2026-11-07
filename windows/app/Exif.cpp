#include "Exif.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>

namespace {

enum : uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7 };

QByteArray rationals(std::initializer_list<std::pair<uint32_t, uint32_t>> values)
{
    QByteArray b;
    for (const auto &[n, d] : values) {
        b.append(reinterpret_cast<const char *>(&n), 4);
        b.append(reinterpret_cast<const char *>(&d), 4);
    }
    return b;
}

std::pair<uint32_t, uint32_t> fraction(double v, uint32_t denominator)
{
    return {uint32_t(std::max(0.0, std::round(v * denominator))), denominator};
}

// 1/250 as such, a long exposure in tenths
std::pair<uint32_t, uint32_t> exposureTime(double seconds)
{
    if (seconds > 0 && seconds < 1)
        return {1, uint32_t(std::round(1 / seconds))};
    return fraction(seconds, 10);
}

} // namespace

bool ExportMetadata::hasPosition() const
{
    if (!gps || gpsStatus == 'V')
        return false;
    const bool latitudeSet = latitude[0] != 0 || latitude[1] != 0 || latitude[2] != 0;
    const bool longitudeSet = longitude[0] != 0 || longitude[1] != 0 || longitude[2] != 0;
    return latitudeSet || longitudeSet;
}

QByteArray tiffShorts(std::initializer_list<uint16_t> values)
{
    QByteArray b;
    for (uint16_t v : values)
        b.append(reinterpret_cast<const char *>(&v), 2);
    return b;
}

QByteArray tiffLongs(const std::vector<uint32_t> &values)
{
    return QByteArray(reinterpret_cast<const char *>(values.data()), qsizetype(values.size() * 4));
}

QByteArray tiffText(const QString &text)
{
    QByteArray b = text.toUtf8();
    b.append('\0');
    return b;
}

TiffEntry tiffEntry(uint16_t tag, uint16_t type, uint32_t count, QByteArray value)
{
    return {tag, type, count, std::move(value), -1};
}

QByteArray layoutDirectories(std::vector<TiffDirectory> directories, uint32_t base, std::vector<uint32_t> *offsets)
{
    for (TiffDirectory &d : directories)
        std::sort(d.begin(), d.end(), [](const TiffEntry &a, const TiffEntry &b) { return a.tag < b.tag; });
    // where each directory and its values land
    std::vector<uint32_t> at(directories.size());
    uint32_t next = base;
    for (size_t i = 0; i < directories.size(); ++i) {
        at[i] = next;
        next += 2 + uint32_t(directories[i].size()) * 12 + 4;
        for (const TiffEntry &e : directories[i])
            if (e.directory < 0 && e.value.size() > 4)
                next += (uint32_t(e.value.size()) + 1) & ~1u;
    }
    if (offsets)
        *offsets = at;
    QByteArray out;
    for (size_t i = 0; i < directories.size(); ++i) {
        const TiffDirectory &d = directories[i];
        QByteArray values;
        uint32_t valueAt = at[i] + 2 + uint32_t(d.size()) * 12 + 4;
        const uint16_t n = uint16_t(d.size());
        out.append(reinterpret_cast<const char *>(&n), 2);
        for (const TiffEntry &e : d) {
            out.append(reinterpret_cast<const char *>(&e.tag), 2);
            out.append(reinterpret_cast<const char *>(&e.type), 2);
            out.append(reinterpret_cast<const char *>(&e.count), 4);
            QByteArray field;
            if (e.directory >= 0) {
                field = tiffLongs({at[size_t(e.directory)]});
            } else if (e.value.size() > 4) {
                const uint32_t offset = valueAt + uint32_t(values.size());
                field = QByteArray(reinterpret_cast<const char *>(&offset), 4);
                values.append(e.value);
                if (e.value.size() % 2)
                    values.append('\0');
            } else {
                field = e.value;
                field.resize(4, '\0');
            }
            out.append(field);
        }
        out.append(QByteArray(4, '\0'));                 // no next directory
        out.append(values);
    }
    return out;
}

ExifDirectories exifDirectories(const ExportMetadata &m, int width, int height, bool srgb)
{
    ExifDirectories d;
    auto text = [](uint16_t tag, const QString &s) { return tiffEntry(tag, kAscii, uint32_t(tiffText(s).size()), tiffText(s)); };
    // Cameras fill the description with spaces, or NULs, when there is
    // nothing to say.
    const QString said = QString(m.description).remove(QChar(0)).trimmed();
    if (!said.isEmpty())
        d.ifd0.push_back(text(270, said));
    else if (!m.provenance.isEmpty())
        d.ifd0.push_back(text(270, m.provenance));
    if (!m.make.isEmpty())
        d.ifd0.push_back(text(271, m.make));
    if (!m.model.isEmpty())
        d.ifd0.push_back(text(272, m.model));
    // the pixels were turned upright on decode
    d.ifd0.push_back(tiffEntry(274, kShort, 1, tiffShorts({1})));
    d.ifd0.push_back(text(305, m.software.isEmpty() ? QStringLiteral("FLX Film Lab") : m.software));
    QString when;
    if (m.taken > 0)
        when = QDateTime::fromSecsSinceEpoch(m.taken).toString(QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!when.isEmpty())
        d.ifd0.push_back(text(306, when));
    if (!m.artist.isEmpty())
        d.ifd0.push_back(text(315, m.artist));

    if (m.shutter > 0) {
        const auto [n, den] = exposureTime(m.shutter);
        d.exif.push_back(tiffEntry(33434, kRational, 1, rationals({{n, den}})));
    }
    if (m.aperture > 0)
        d.exif.push_back(tiffEntry(33437, kRational, 1, rationals({fraction(m.aperture, 10)})));
    if (m.iso > 0)
        d.exif.push_back(tiffEntry(34855, kShort, 1, tiffShorts({uint16_t(std::min(m.iso, 65535))})));
    d.exif.push_back(tiffEntry(36864, kUndefined, 4, QByteArray("0232", 4)));
    if (!when.isEmpty()) {
        d.exif.push_back(text(36867, when));
        d.exif.push_back(text(36868, when));
    }
    if (m.focalLength > 0)
        d.exif.push_back(tiffEntry(37386, kRational, 1, rationals({fraction(m.focalLength, 10)})));
    if (!m.provenance.isEmpty()) {
        // UserComment: its character code, then the text, unterminated
        const QByteArray comment = QByteArray("ASCII\0\0\0", 8) + m.provenance.toLatin1();
        d.exif.push_back(tiffEntry(37510, kUndefined, uint32_t(comment.size()), comment));
    }
    d.exif.push_back(tiffEntry(40960, kUndefined, 4, QByteArray("0100", 4)));     // FlashpixVersion, which EXIF requires
    // EXIF names one colour space, sRGB; for the rest the profile says which
    if (srgb)
        d.exif.push_back(tiffEntry(40961, kShort, 1, tiffShorts({1})));
    d.exif.push_back(tiffEntry(40962, kLong, 1, tiffLongs({uint32_t(width)})));
    d.exif.push_back(tiffEntry(40963, kLong, 1, tiffLongs({uint32_t(height)})));
    if (m.focalLength35 > 0)
        d.exif.push_back(tiffEntry(41989, kShort, 1, tiffShorts({uint16_t(m.focalLength35)})));
    if (!m.lensMake.isEmpty())
        d.exif.push_back(text(42035, m.lensMake));
    if (!m.lensModel.isEmpty())
        d.exif.push_back(text(42036, m.lensModel));

    if (m.hasPosition()) {
        d.gps.push_back(tiffEntry(0, kByte, 4, QByteArray("\x02\x03\x00\x00", 4)));
        const char lat = m.latitudeRef == 'S' ? 'S' : 'N', lon = m.longitudeRef == 'W' ? 'W' : 'E';
        d.gps.push_back(tiffEntry(1, kAscii, 2, QByteArray(&lat, 1) + QByteArray(1, '\0')));
        d.gps.push_back(tiffEntry(2, kRational, 3, rationals({fraction(m.latitude[0], 1), fraction(m.latitude[1], 1),
                                                             fraction(m.latitude[2], 10000)})));
        d.gps.push_back(tiffEntry(3, kAscii, 2, QByteArray(&lon, 1) + QByteArray(1, '\0')));
        d.gps.push_back(tiffEntry(4, kRational, 3, rationals({fraction(m.longitude[0], 1), fraction(m.longitude[1], 1),
                                                             fraction(m.longitude[2], 10000)})));
        if (m.altitude != 0) {
            const char ref = m.altitudeRef ? 1 : 0;
            d.gps.push_back(tiffEntry(5, kByte, 1, QByteArray(&ref, 1)));
            d.gps.push_back(tiffEntry(6, kRational, 1, rationals({fraction(std::abs(m.altitude), 100)})));
        }
    }
    return d;
}

QByteArray exifBlock(const ExportMetadata &m, int width, int height, bool srgb, bool jpeg)
{
    ExifDirectories d = exifDirectories(m, width, height, srgb);
    if (jpeg) {
        d.ifd0.push_back(tiffEntry(531, kShort, 1, tiffShorts({1})));                    // YCbCrPositioning: centred
        d.exif.push_back(tiffEntry(37121, kUndefined, 4, QByteArray("\x01\x02\x03\x00", 4)));  // Y Cb Cr
    }
    std::vector<TiffDirectory> directories{d.ifd0, d.exif};
    TiffEntry exifPointer = tiffEntry(34665, kLong, 1, {});
    exifPointer.directory = 1;
    directories[0].push_back(exifPointer);
    if (!d.gps.empty()) {
        directories.push_back(d.gps);
        TiffEntry gpsPointer = tiffEntry(34853, kLong, 1, {});
        gpsPointer.directory = 2;
        directories[0].push_back(gpsPointer);
    }
    QByteArray block("II*\0", 4);
    const uint32_t first = 8;
    block.append(reinterpret_cast<const char *>(&first), 4);
    block.append(layoutDirectories(directories, 8));
    return block;
}
