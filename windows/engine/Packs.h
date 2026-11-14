#pragma once

#include "Mat3.h"

#include <QString>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The film and paper data the Mac app bundles (FLXFilmLab/Resources/packs in
// its tree, baked by tools/bake/bake_packs.py): ProfilePack.swift and
// PackLibrary.swift.

constexpr int kWavelengths = 81;                 // 380..780 nm at 5 nm
constexpr int kCurveSamples = 256;
constexpr int kCouplerOversampling = 16;
constexpr int kToneLutWidth = 512;
constexpr int kToneLutHeight = 17;
constexpr float kLocalGradeMin = -40;
constexpr float kLocalGradeMax = 40;
constexpr float kGradeReference = 115;
constexpr float kGradeMin = 50;
constexpr float kGradeMax = 180;
constexpr int kMaxExposureGradients = 8;

// Offsets into the packed spectral-table buffer print_and_scan reads.
constexpr int kOffFilmChannelDensity = 0;
constexpr int kOffFilmBaseDensity = 243;
constexpr int kOffFilmValid = 324;
constexpr int kOffPrintIlluminant = 405;
constexpr int kOffPaperSensitivity = 486;
constexpr int kOffPrintChannelDensity = 729;
constexpr int kOffPrintBaseDensity = 972;
constexpr int kOffPrintValid = 1053;
constexpr int kOffViewingIlluminant = 1134;
constexpr int kOffCMFs = 1215;
constexpr int kSpectralTableFloats = 1458;

struct CurveModel {
    int nChannels = 0, nLayers = 0;
    std::vector<float> centers, amplitudes, sigmas;     // (nChannels, nLayers) row-major
};

struct DirCouplerDefaults {
    bool active = false;
    float amount = 0, inhibitionSameLayer = 0, inhibitionInterLayer = 0;
    float gammaSameLayerRGB[3] = {}, gammaInterlayerRtoGB[2] = {}, gammaInterlayerGtoRB[2] = {},
          gammaInterlayerBtoRG[2] = {};
    float diffusionSizeUm = 0;
    std::optional<float> edgeGainReference, diffusionFraction;
};

struct HalationDefaults {
    bool active = false;
    float scatterCoreUm[3] = {}, scatterTailUm[3] = {}, scatterTailWeight[3] = {}, halationStrength[3] = {},
          halationFirstSigmaUm[3] = {};
    int bounces = 1;
    float bounceDecay = 0;
    bool renormalize = false;
};

struct GrainDefaults {
    float tableDensityMax[3] = {};
    std::vector<float> selwynUm2;   // [record][sample][layer]
    float cloudUm[9] = {};
    int tableSamples() const { return int(selwynUm2.size() / 9); }
};

struct LensFilterRef {
    std::string id, name, file;
    float factor = 1;
    std::string wratten;            // the number, for a black-and-white film's filters, named by their colour
    // The light the filter is made for, where its factor and the print time
    // are set, if not the film's own: daylight ("D55") for the 85 and 85B on
    // a tungsten film.
    std::string light;
    // "Red, Wratten 25", or a colour film's filter's own name, "Wratten 85B"
    std::string designation() const { return wratten.empty() ? name : name + ", Wratten " + wratten; }
};

// An instant print's curves at each processing temperature its sheet plots
// (ProcessingCurves): Polaroid's plot the print at 55, 70 and 95 F, and those
// three are all there is.
struct ProcessingCurves {
    float normalC = 0;                              // the sheets' normal one, 70 F: the pack's own curves
    std::vector<float> temperaturesC;               // ascending
    std::vector<std::vector<float>> densityCurves;  // one (256, 3) set per temperature, on the normal print's base
};

struct ProfilePack {
    std::string stock, name, type, support, stage, channelModel, targetPrint;
    std::string referenceIlluminant, viewingIlluminant;
    std::string use;                // agx's "still" or "cine": a motion picture film, or the print film one prints on
    std::vector<float> logExposure;                     // 256
    std::vector<float> densityCurves;                   // (256, 3)
    std::vector<float> densityMax, densityMin;          // 3
    std::vector<float> sensitivity, channelDensity;     // (81, 3)
    std::vector<float> baseDensity;                     // 81 or empty
    std::vector<float> spectralValid;                   // 81
    CurveModel curveModel;
    std::optional<DirCouplerDefaults> dirCouplers;
    std::optional<HalationDefaults> halation;
    std::optional<GrainDefaults> grain;
    std::optional<Mat3> rgbToXYZ;                       // filming stocks only
    std::string tcLutFile;
    int tcLutSize = 0;
    std::vector<LensFilterRef> lensFilters;
    std::optional<float> gradeSoftest, gradeDefault;
    std::optional<ProcessingCurves> processing;         // an instant print's, at each temperature
    std::vector<float> frameMM;                         // an instant print's own frame, mm, or empty

    bool isPositive() const { return type == "positive"; }
    // A film that develops straight into its own print (Polaroid's peel-apart
    // films): a positive on a paper support, seen by reflection, which the
    // engine shows as it shows a slide, white-pointed on its base, the paper.
    bool isInstantPrint() const { return isFilmStage() && support == "paper"; }
    // Seen as it is, not printed: a slide off the light table unless scanFilm
    // says otherwise, an instant print always.
    bool isViewedDirectly(const std::optional<bool> &scanFilm) const
    {
        return isInstantPrint() || scanFilm.value_or(isPositive());
    }
    // The processing temperature its sheet plots nearest temperatureC (none:
    // the normal one), in whole degrees Fahrenheit; none without processing curves.
    std::optional<int> processingFahrenheit(const std::optional<double> &temperatureC) const;
    // The print processed at temperatureC (none: the sheets' normal one), the
    // nearest of the temperatures its sheet plots: that print's own curves,
    // nothing between them. D-min stays the normal print's, so a cold or hot
    // print's whites keep what processing gave them.
    ProfilePack processed(const std::optional<double> &temperatureC) const;
    // The frame a picture is laid on, long and short side in mm: an instant
    // print's own, or the recipe's format's.
    std::pair<double, double> frame(double formatLongMM, double formatShortMM) const;
    bool isMonochrome() const { return channelModel == "bw"; }
    bool isMotionPicture() const { return use == "cine"; }
    bool isFilmStage() const { return stage == "filming"; }
    float softestGrade() const { return gradeSoftest.value_or(kGradeMax); }
    float defaultGrade() const { return gradeDefault.value_or(kGradeReference); }
    float logExposureStart() const { return logExposure.front(); }
    float logExposureStep() const
    {
        return (logExposure.back() - logExposure.front()) / float(logExposure.size() - 1);
    }
    Mat3 rgbToXYZMatrix() const { return rgbToXYZ.value_or(Mat3::identity()); }
    const LensFilterRef *lensFilter(const std::optional<std::string> &id) const;
    // agx develop() works on curves shifted so each channel's minimum is zero.
    std::vector<float> normalizedDensityCurves() const;
};

struct SharedData {
    std::vector<float> cmfs;                            // (81, 3)
    std::map<std::string, std::vector<float>> illuminants;
    std::vector<float> dichroicFilters;                 // (81, 3), CMY order
    // paper -> illuminant -> film -> c, m, y
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<float>>>> neutralPrintFilters;
    std::map<std::string, std::map<std::string, std::vector<float>>> xyzToRGB;   // illuminant -> space -> row-major

    const std::vector<float> &illuminant(const std::string &name) const;
    Mat3 xyzToRGBMatrix(const std::string &viewing, const std::string &output) const;
    float cmfNormalization(const std::string &viewing) const;
    float3 illuminantXYZ(const std::string &viewing) const;
    std::optional<float3> neutralFilters(const std::string &paper, const std::string &illuminant,
                                         const std::string &film) const;
};

// A stock as the index lists it (PackIndexEntry).
struct PackIndexEntry {
    std::string stock, name, type, support, targetPrint, channelModel, use;
    bool isMonochrome() const { return channelModel == "bw"; }
    bool isMotionPicture() const { return use == "cine"; }
    bool isInstantPrint() const { return support == "paper"; }   // a film that is its own print
};

// The stock pickers' sections, in the order they are listed
// (PackIndexEntry.groups): films by kind, papers by colour.
struct StockGroup {
    std::string title;
    std::vector<PackIndexEntry> entries;
};
std::vector<StockGroup> stockGroups(const std::vector<PackIndexEntry> &entries, bool films);

// The films the Film menu lists, as Settings' Films tab leaves them
// (FilmMenu). The setting keeps the hidden films' stock names,
// comma-separated, so the films a later version adds show.
namespace filmmenu {
inline constexpr const char *kHiddenKey = "hiddenFilms";
std::set<std::string> hidden(const std::string &stored);
std::string stored(const std::set<std::string> &hidden);
// Every film but the hidden ones, and the one in use whatever the setting,
// so the menu shows what a picture, clip or preset uses.
std::vector<PackIndexEntry> shown(const std::vector<PackIndexEntry> &entries, const std::string &stored,
                                  const std::string &inUse);
} // namespace filmmenu

// The packs directory: index.json, shared.json, one <stock>.json each, and
// the tcLut binaries. Packs are read once and kept.
class PackLibrary
{
public:
    explicit PackLibrary(const QString &directory);     // throws std::runtime_error
    // Where the packs are for a tool run from the port's root: the Mac app's,
    // in mac/ in the port's own repository or beside windows/ in the public
    // one, else the copy tools/pc/remote.sh keeps beside the sources.
    static QString defaultDirectory();

    const ProfilePack &pack(const std::string &stock);
    const SharedData &shared() const { return m_shared; }
    // The stock's chromaticity LUT, (size, size, 3), or the one for a camera
    // filter in front of it.
    const std::vector<float> &tcLut(const ProfilePack &pack, const LensFilterRef *lensFilter = nullptr);
    std::vector<std::string> printStocks() const { return m_printStocks; }
    const std::vector<PackIndexEntry> &filmEntries() const { return m_filmEntries; }
    const std::vector<PackIndexEntry> &paperEntries() const { return m_paperEntries; }
    const std::vector<std::string> &outputColorSpaces() const { return m_outputColorSpaces; }

private:
    QString m_directory;
    SharedData m_shared;
    std::vector<std::string> m_printStocks;
    std::vector<PackIndexEntry> m_filmEntries, m_paperEntries;
    std::vector<std::string> m_outputColorSpaces;
    std::map<std::string, std::unique_ptr<ProfilePack>> m_packs;
    std::map<std::string, std::vector<float>> m_tcLuts;
};
