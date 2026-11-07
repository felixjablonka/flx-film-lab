#pragma once

#include "Recipe.h"

#include <QString>

#include <vector>

// How the RAW is balanced before the film sees it (RawLoader.WhiteBalance).
enum class WhiteBalance { FilmReference, AsShot, DaylightD55, Daylight, Tungsten3200, Tungsten, Custom };
// In the menu's order (RawLoader.WhiteBalance.allCases).
inline constexpr WhiteBalance kWhiteBalances[] = {
    WhiteBalance::FilmReference, WhiteBalance::AsShot, WhiteBalance::DaylightD55, WhiteBalance::Daylight,
    WhiteBalance::Tungsten3200, WhiteBalance::Tungsten, WhiteBalance::Custom};

QString whiteBalanceName(WhiteBalance wb);         // the Mac's raw value: "Film reference", ...
QString whiteBalanceLabel(WhiteBalance wb);        // what the menu shows: "Daylight D65", ...
WhiteBalance whiteBalanceNamed(const QString &name);
std::string decodeName(WhiteBalance wb);           // DecodeOptions': film, as-shot, ...

// What Copy Settings copies and a preset holds: the recipe without the masks,
// the gradients and the crop, and the white balance.
struct Look {
    Recipe recipe;
    WhiteBalance whiteBalance = WhiteBalance::FilmReference;
    double customTemperature = 5500, customTint = 0;
    bool operator==(const Look &) const = default;
};

struct Preset {
    QString name;
    Look look;
};

// The presets on disk, one JSON file each in the Mac's form (Presets.swift),
// so a preset file copied between the two apps works in both. A deleted one
// goes to the Recycle Bin.
class PresetStore
{
public:
    explicit PresetStore(QString directory);
    static QString standardDirectory();

    // Every preset that reads, by name; two files of one name give one preset.
    std::vector<Preset> load() const;
    // Over the file of a preset of the same name, if there is one. Throws.
    void save(const Preset &preset) const;
    void remove(const QString &name) const;
    QString ensuredDirectory() const;

private:
    struct File {
        QString path;
        std::optional<Preset> preset;
    };
    std::vector<File> files() const;
    QString m_directory;
};
