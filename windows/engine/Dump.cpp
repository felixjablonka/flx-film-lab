#include "Dump.h"

#include <QFile>

#include <cstring>
#include <stdexcept>

namespace {

struct Header {
    char magic[4];
    uint32_t version, width, height, channels, type, reserved[2];
};
static_assert(sizeof(Header) == 32);

void write(const QString &path, int width, int height, int channels, uint32_t type,
           const void *values, size_t byteCount)
{
    Header h{{'F', 'L', 'X', 'D'}, 1, uint32_t(width), uint32_t(height), uint32_t(channels), type, {0, 0}};
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)
        || f.write(reinterpret_cast<const char *>(&h), sizeof h) != qint64(sizeof h)
        || f.write(static_cast<const char *>(values), qint64(byteCount)) != qint64(byteCount))
        throw std::runtime_error("cannot write " + path.toStdString());
}

} // namespace

Dump readDump(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot open " + path.toStdString());
    const QByteArray data = f.readAll();
    Header h;
    if (data.size() < qsizetype(sizeof h))
        throw std::runtime_error(path.toStdString() + ": too short");
    std::memcpy(&h, data.constData(), sizeof h);
    if (std::memcmp(h.magic, "FLXD", 4) != 0 || h.version != 1 || h.type > 1)
        throw std::runtime_error(path.toStdString() + ": not an FLXD v1 dump");
    Dump d;
    d.width = int(h.width);
    d.height = int(h.height);
    d.channels = int(h.channels);
    const size_t bytes = d.count() * (h.type == 0 ? 4 : 1);
    if (size_t(data.size()) != sizeof h + bytes)
        throw std::runtime_error(path.toStdString() + ": size does not match its header");
    const char *payload = data.constData() + sizeof h;
    if (h.type == 0) {
        d.floats.resize(d.count());
        std::memcpy(d.floats.data(), payload, bytes);
    } else {
        d.bytes.assign(payload, payload + bytes);
    }
    return d;
}

void writeDump(const QString &path, int width, int height, int channels, const float *values)
{
    write(path, width, height, channels, 0, values, size_t(width) * height * channels * 4);
}

void writeDump(const QString &path, int width, int height, int channels, const uint8_t *values)
{
    write(path, width, height, channels, 1, values, size_t(width) * height * channels);
}
