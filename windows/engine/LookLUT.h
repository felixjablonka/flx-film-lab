#pragma once

#include "LogFormats.h"
#include "Packs.h"
#include "Recipe.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class QRhi;
class FilmRenderer;
class SetupBuilder;

// A print look as a 3D lookup table: log footage in, display RGB out
// (LookLUT.swift). Everything the engine does per pixel (film exposure,
// development, the couplers' colour, the enlarger, the paper and its tone
// controls, the scan) is a function of the pixel's own colour, so it is
// sampled once on a lattice and interpolated; what depends on neighbours or
// on the whole frame is switched off first (LookBaker::perPixel).
struct LookLUT {
    int size = 0;
    LutInput input = LutInput::NLogCode;
    LutOutput output = LutOutput::BT1886;
    // RGB triples, red fastest, then green, then blue: the .cube order, and
    // a 3D texture's indexed (r, g, b)
    std::vector<float> values;

    // tetrahedral interpolation, as the grade kernel does it
    Vec3d sample(Vec3d p) const;
    // RGBA, for a 3D texture
    std::vector<float> rgba() const;
    // The Adobe / Resolve .cube text, plain ASCII.
    std::string cubeText(const std::string &title, const std::vector<std::string> &comments = {},
                         bool domain = true) const;
};

// Samples the engine onto a lattice, keeping its renderer and lattice between
// bakes, so a re-bake after a slider move is one render of the lattice.
class LookBaker
{
public:
    LookBaker(QRhi *rhi, PackLibrary &packs);
    ~LookBaker();

    // The recipe as a LUT can carry it: every per-pixel control as set, the
    // meters, halation, grain, the couplers' diffusion, masks, gradients,
    // effects and the crop off.
    static Recipe perPixel(Recipe recipe);

    // Throws std::runtime_error.
    LookLUT bake(const Recipe &recipe, int size = 65, LutInput input = LutInput::NLogCode,
                 LutOutput output = LutOutput::BT1886);

private:
    std::unique_ptr<FilmRenderer> m_renderer;
    std::unique_ptr<SetupBuilder> m_builder;
    std::optional<std::pair<int, LutInput>> m_lattice;
};
