#pragma once

#include <cstdint>
#include <vector>

// A look's grain as its print shows it, by the print's own density: what the
// frame pipeline lays on a finished frame (PrintGrain.swift,
// FramePipeline::grained).
//
// The grain is the negative's: each emulsion layer's Selwyn coefficient by
// the negative's density, from the stock's own data (GrainDefaults), and its
// dye clouds' size. The print sees it through its spectral response: a change
// in one layer's density moves each of the print's channels, its own most,
// the others through the dyes' overlapping absorptions. So the look's grey
// scale is rendered as a ramp of light through the engine
// (LookBaker::printGrain), once as it is and once with each layer of the
// negative a little denser and a little thinner
// (FilmRenderer::setNegativeOffset), and read off each step: the negative's
// densities, the print's, and how the print's channels answer each layer.
// Each pixel's grain is then each layer's own, drawn at the negative density
// that prints the pixel's tone, mixed as the print mixes them. Where the
// print is clipped, the paper's white or its deepest black, the answer and
// the grain go to nothing, as on a print. A colour film on black-and-white
// paper mixes its three layers' grains into one neutral grain; a
// black-and-white film has one record.
struct PrintGrainTable {
    // Each record's Selwyn coefficient, OD^2 um^2, [record][sample][layer],
    // laid out as GrainDefaults::selwynUm2, by the print density of the
    // channel that record prints (the red channel's for the cyan layer).
    std::vector<float> selwynUm2;
    // How the print's density answers each record's, d D_print / d D_negative,
    // [channel][sample][record], by the channel's own print density.
    std::vector<float> response;
    // the print density at each channel's last sample
    float tableDensityMax[3] = {};
    int samples = 0;
    // the negative's dye clouds or silver clumps, um, [record][layer]
    float cloudUm[9] = {};
    // one record for the three channels: a black-and-white film
    bool monochrome = false;
    // the stock's part of every frame's seed, as in Stills
    uint32_t stockSeed = 0;

    bool operator==(const PrintGrainTable &) const = default;

    // The grey ramp the table is read from: scene light from 10 stops below
    // an 18% grey to 10 above, `count` steps, RGBA.
    static std::vector<float> greyRamp(int count);

    // A layer's coefficient at a negative density, as film_grain_layer reads
    // it: `selwyn` [record][sample][layer] of `tableSamples` a record, up to
    // `tableDensityMax`.
    static float negativeSelwyn(const std::vector<float> &selwyn, const float tableDensityMax[3], int tableSamples,
                                int record, int layer, float density);

    // The table from the ramp's renders, RGBA each, `count` steps: the
    // developed negative's net density per record, the print's light, and for
    // each record the print's light with that record `step` denser and `step`
    // thinner. The stock's own grain: `selwyn`, `tableDensityMax`,
    // `tableSamples` and `cloudUm`, as RenderSetup carries them.
    static PrintGrainTable build(const std::vector<float> &negative, const std::vector<float> &print,
                                 const std::vector<float> denser[3], const std::vector<float> thinner[3], float step,
                                 int count, const std::vector<float> &selwyn, const float tableDensityMax[3],
                                 int tableSamples, const float cloudUm[9], bool monochromeFilm, uint32_t stockSeed,
                                 int samples = 64);
};
