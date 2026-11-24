#include "HEVCLevels.h"

#include "Paths.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace hevclevels {

const std::vector<Limits> &table()
{
    static const std::vector<Limits> levels = {
        {"1", 30, 36'864, 552'960, 0.128, 0},
        {"2", 60, 122'880, 3'686'400, 1.5, 0},
        {"2.1", 63, 245'760, 7'372'800, 3, 0},
        {"3", 90, 552'960, 16'588'800, 6, 0},
        {"3.1", 93, 983'040, 33'177'600, 10, 0},
        {"4", 120, 2'228'224, 66'846'720, 12, 30},
        {"4.1", 123, 2'228'224, 133'693'440, 20, 50},
        {"5", 150, 8'912'896, 267'386'880, 25, 100},
        {"5.1", 153, 8'912'896, 534'773'760, 40, 160},
        {"5.2", 156, 8'912'896, 1'069'547'520, 60, 240},
        {"6", 180, 35'651'584, 1'069'547'520, 60, 240},
        {"6.1", 183, 35'651'584, 2'139'095'040, 120, 480},
        {"6.2", 186, 35'651'584, 4'278'190'080, 240, 800},
    };
    return levels;
}

size_t needed(int width, int height, double frameRate)
{
    const double pixels = double(width) * double(height);
    const double side = double(std::max(width, height));
    const double samples = pixels * std::max(frameRate, 1.0);
    const auto &t = table();
    for (size_t i = 0; i < t.size(); ++i)
        if (pixels <= double(t[i].picture) && side * side <= 8 * double(t[i].picture) && samples <= t[i].samples)
            return i;
    return t.size() - 1;
}

std::vector<double> steps(int width, int height, double frameRate)
{
    std::vector<double> steps;
    const auto &t = table();
    for (size_t i = needed(width, height, frameRate); i < t.size() && steps.size() < 3; ++i)
        if (steps.empty() || t[i].top() > steps.back())
            steps.push_back(t[i].top());
    while (steps.size() < 3)
        steps.push_back(steps.back());
    return steps;
}

static const Limits *limitsOf(uint8_t idc)
{
    for (const Limits &l : table())
        if (l.idc == idc)
            return &l;
    return nullptr;
}

std::string Label::level() const
{
    if (const Limits *l = limitsOf(idc))
        return l->name;
    char text[16];
    std::snprintf(text, sizeof text, "%.1f", double(idc) / 30);
    return text;
}

std::string Label::description() const
{
    return std::string(high ? "High" : "Main") + " tier, level " + level();
}

double Label::megabits() const
{
    const Limits *l = limitsOf(idc);
    return l ? (high && l->high > 0 ? l->high : l->main) : 0;
}

Label label(int width, int height, double frameRate, double megabits)
{
    const double rate = megabits - 1e-9;
    const auto &t = table();
    for (size_t i = needed(width, height, frameRate); i < t.size(); ++i) {
        if (rate <= t[i].main)
            return {false, t[i].idc};
        if (t[i].high > 0 && rate <= t[i].high)
            return {true, t[i].idc};
    }
    return {true, t.back().idc};
}

// MARK: - The label in a file

namespace {

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The payload of the box at the end of `path`, under the boxes from `from`
// to `to` in `data`. A sample description's entries start 8 bytes in, a
// visual sample entry's boxes 78; "hvc1" matches hev1 too.
std::optional<std::pair<size_t, size_t>> find(const std::vector<uint8_t> &data, size_t from, size_t to,
                                              const std::vector<std::string> &path, size_t depth = 0)
{
    size_t at = from;
    while (at + 8 <= to) {
        const size_t length = be32(&data[at]);
        if (length < 8 || at + length > to)
            return std::nullopt;
        const std::string type(reinterpret_cast<const char *>(&data[at + 4]), 4);
        const std::string &want = path[depth];
        if (type == want || (want == "hvc1" && type == "hev1")) {
            if (depth + 1 == path.size())
                return std::pair{at + 8, at + length};
            const size_t inner = at + 8 + (type == "stsd" ? 8 : type == "hvc1" || type == "hev1" ? 78 : 0);
            if (inner <= at + length)
                if (auto found = find(data, inner, at + length, path, depth + 1))
                    return found;
        }
        at += length;
    }
    return std::nullopt;
}

// The movie's hvcC record and where it lies in the file: moov, trak, mdia,
// minf, stbl, stsd, hvc1 or hev1, hvcC. The media data is skipped, never
// read.
struct Record {
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
};

std::optional<Record> record(const std::string &path)
{
    std::ifstream in(fsPath(path), std::ios::binary);
    if (!in)
        return std::nullopt;
    in.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(in.tellg());
    uint64_t at = 0;
    while (at + 8 <= size) {
        in.seekg(std::streamoff(at));
        uint8_t h[16] = {};
        in.read(reinterpret_cast<char *>(h), 16);
        const std::streamsize got = in.gcount();
        in.clear();
        if (got < 8)
            break;
        uint64_t length = be32(h);
        uint64_t header = 8;
        if (length == 1) {
            if (got < 16)
                break;
            length = uint64_t(be32(h + 8)) << 32 | be32(h + 12);
            header = 16;
        } else if (length == 0) {
            length = size - at;
        }
        if (length < header || at + length > size)
            return std::nullopt;
        if (std::string(reinterpret_cast<const char *>(h + 4), 4) == "moov") {
            std::vector<uint8_t> moov(size_t(length - header));
            in.seekg(std::streamoff(at + header));
            in.read(reinterpret_cast<char *>(moov.data()), std::streamsize(moov.size()));
            if (size_t(in.gcount()) != moov.size())
                return std::nullopt;
            const auto found = find(moov, 0, moov.size(), {"trak", "mdia", "minf", "stbl", "stsd", "hvc1", "hvcC"});
            if (!found)
                return std::nullopt;
            Record r;
            r.offset = at + header + found->first;
            r.bytes.assign(moov.begin() + std::ptrdiff_t(found->first), moov.begin() + std::ptrdiff_t(found->second));
            return r;
        }
        at += length;
    }
    return std::nullopt;
}

} // namespace

std::optional<Label> labelOf(const std::string &path)
{
    const auto r = record(path);
    if (!r || r->bytes.size() < 13)
        return std::nullopt;
    return Label{(r->bytes[1] & 0x20) != 0, r->bytes[12]};
}

bool relabel(const std::string &path, const Label &label)
{
    auto r = record(path);
    if (!r || r->bytes.size() <= 23)
        return false;
    std::vector<uint8_t> bytes = r->bytes;
    const uint8_t tierByte = bytes[1], level = bytes[12];
    std::vector<size_t> positions;     // each set's tier byte and level byte, in the record
    size_t at = 23;
    for (int a = 0; a < int(bytes[22]); ++a) {
        if (at + 3 > bytes.size())
            return false;
        const int type = bytes[at] & 0x3f;
        const size_t count = size_t(bytes[at + 1]) << 8 | bytes[at + 2];
        at += 3;
        for (size_t n = 0; n < count; ++n) {
            if (at + 2 > bytes.size())
                return false;
            const size_t length = size_t(bytes[at]) << 8 | bytes[at + 1];
            const size_t start = at + 2, end = start + length;
            if (end > bytes.size())
                return false;
            at = end;
            if (type != 32 && type != 33)
                continue;
            // the payload past the two-byte NAL header, its emulation
            // prevention bytes (00 00 03) left out, mapped to the record
            std::vector<uint8_t> payload;
            std::vector<size_t> place;
            int zeros = 0;
            for (size_t i = start + 2; i < end; ++i) {
                if (zeros >= 2 && bytes[i] == 3) {
                    zeros = 0;
                    continue;
                }
                payload.push_back(bytes[i]);
                place.push_back(i);
                zeros = bytes[i] == 0 ? zeros + 1 : 0;
            }
            // VPS: id, flags and layers in two bytes, 0xffff, then
            // profile_tier_level; SPS: id and sub-layers in one byte, then it.
            const size_t ptl = type == 32 ? 4 : 1;
            if (payload.size() <= ptl + 11)
                return false;
            const int subLayers = type == 32 ? (payload[1] >> 1) & 7 : (payload[0] >> 1) & 7;
            if (subLayers != 0 || payload[ptl] != tierByte || payload[ptl + 11] != level)
                return false;
            positions.push_back(place[ptl]);
            positions.push_back(place[ptl + 11]);
        }
    }
    if (positions.size() < 4)
        return false;
    const uint8_t newTier = label.high ? uint8_t(tierByte | 0x20) : uint8_t(tierByte & ~0x20);
    bytes[1] = newTier;
    bytes[12] = label.idc;
    for (size_t n = 0; n < positions.size(); ++n)
        bytes[positions[n]] = n % 2 == 0 ? newTier : label.idc;
    if (bytes == r->bytes)
        return true;
    std::fstream out(fsPath(path), std::ios::binary | std::ios::in | std::ios::out);
    if (!out)
        throw std::runtime_error("cannot open the export to label it");
    out.seekp(std::streamoff(r->offset));
    out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    out.flush();
    if (!out)
        throw std::runtime_error("cannot label the export");
    return true;
}

} // namespace hevclevels
