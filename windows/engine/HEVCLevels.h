#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// HEVC's levels and tiers (ITU-T H.265, Annex A): the picture size, frame
// rate and data rate a player that declares a level promises to decode
// (HEVCLevels.swift).
//
// Export's HEVC rates come from here (HEVCLevel), and every HEVC export is
// labelled with the lowest level and tier that holds it, as cameras label
// theirs. The GPUs' encoders are told the level and tier; VideoToolbox, which
// the port's Mac build writes with, only ever labels Main tier and climbs
// levels with the data rate (a 4K 30 fps clip at 100 Mbit/s says level 6.1,
// an 8K level), so its files are relabelled once written.
namespace hevclevels {

// One level: the largest picture, in luma samples, the luma samples a second,
// and the data rate Main and Main 10 may have at each tier, in Mbit/s
// (Tables A.8 and A.9; below level 4 there is no High tier).
struct Limits {
    const char *name;
    uint8_t idc;            // general_level_idc, 30 times the level
    int64_t picture;
    double samples;
    double main;
    double high;            // 0 where there is no High tier
    double top() const { return high > 0 ? high : main; }
};
const std::vector<Limits> &table();

// The first level that holds pictures `width` x `height` at `frameRate`:
// their size, their longer side (no more than the square root of 8 times the
// level's size) and their luma samples a second. 4K needs level 5 up to 30
// fps, 5.1 up to 60 and 5.2 up to 120; HD 4, 4.1 and 5.
size_t needed(int width, int height, double frameRate);

// The limits from the clip's own level upward, each larger than the one
// before: the most its level allows, the next level's, the one after. 4K at
// 30 fps: 100, 160 and 240 Mbit/s; at 60, 160, 240 and 480.
std::vector<double> steps(int width, int height, double frameRate);

// A level and tier, as a file declares them.
struct Label {
    bool high = false;
    uint8_t idc = 0;
    bool operator==(const Label &) const = default;
    std::string description() const;    // "High tier, level 5.1"
    std::string level() const;          // "5.1", as the encoders take it
    // the data rate the label allows, Mbit/s: its level's at its tier
    double megabits() const;
};

// The lowest level, and in it the lower tier, that holds the clip at
// `megabits`: Main tier while the rate is within it, High tier above.
Label label(int width, int height, double frameRate, double megabits);

// The label an HEVC movie declares, from its hvcC record; none if it has no
// HEVC video or cannot be read.
std::optional<Label> labelOf(const std::string &path);

// Labels an HEVC movie `label` in place: the hvcC record's level and tier,
// and the same in its VPS and SPS, where players read them. Nothing else
// changes, so the pictures decode as they did. False, the file untouched, if
// the movie is not one it knows how to relabel: no hvcC, parameter sets that
// disagree with it, or sub-layers. Throws std::runtime_error when the file
// cannot be written.
bool relabel(const std::string &path, const Label &label);

} // namespace hevclevels
