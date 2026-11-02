#include "LookLUT.h"

#include "FilmRenderer.h"
#include "SetupBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

Vec3d LookLUT::sample(Vec3d p) const
{
    const int n1 = size - 1;
    Vec3d q{};
    std::array<int, 3> i{};
    Vec3d f{};
    for (size_t c = 0; c < 3; ++c) {
        q[c] = std::clamp(p[c], 0.0, 1.0) * n1;
        i[c] = std::min(int(q[c]), size - 2);
        f[c] = q[c] - i[c];
    }
    auto v = [&](int dr, int dg, int db) {
        const size_t o = (size_t(i[0] + dr) + size_t(size) * (size_t(i[1] + dg) + size_t(size) * size_t(i[2] + db))) * 3;
        return Vec3d{values[o], values[o + 1], values[o + 2]};
    };
    auto lerp3 = [](Vec3d a, Vec3d b, Vec3d c, Vec3d d, double fa, double fb, double fc) {
        Vec3d out{};
        for (size_t k = 0; k < 3; ++k)
            out[k] = a[k] + fa * (b[k] - a[k]) + fb * (c[k] - b[k]) + fc * (d[k] - c[k]);
        return out;
    };
    const Vec3d c000 = v(0, 0, 0), c111 = v(1, 1, 1);
    if (f[0] > f[1]) {
        if (f[1] > f[2])
            return lerp3(c000, v(1, 0, 0), v(1, 1, 0), c111, f[0], f[1], f[2]);
        if (f[0] > f[2])
            return lerp3(c000, v(1, 0, 0), v(1, 0, 1), c111, f[0], f[2], f[1]);
        return lerp3(c000, v(0, 0, 1), v(1, 0, 1), c111, f[2], f[0], f[1]);
    }
    if (f[2] > f[1])
        return lerp3(c000, v(0, 0, 1), v(0, 1, 1), c111, f[2], f[1], f[0]);
    if (f[2] > f[0])
        return lerp3(c000, v(0, 1, 0), v(0, 1, 1), c111, f[1], f[2], f[0]);
    return lerp3(c000, v(0, 1, 0), v(1, 1, 0), c111, f[1], f[0], f[2]);
}

std::vector<float> LookLUT::rgba() const
{
    const size_t n = size_t(size) * size * size;
    std::vector<float> out(n * 4, 1.0f);
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < 3; ++c)
            out[i * 4 + c] = values[i * 3 + c];
    return out;
}

std::string LookLUT::cubeText(const std::string &title, const std::vector<std::string> &comments, bool domain) const
{
    auto ascii = [](const std::string &s) {
        std::string out;
        for (unsigned char c : s)
            if (c < 128)
                out.push_back(char(c));
        return out;
    };
    std::string titled;
    for (char c : ascii(title))
        if (c != '"')
            titled.push_back(c);
    std::string text = "TITLE \"" + titled + "\"\n";
    std::vector<std::string> lines = comments;
    lines.push_back("Input: " + lutInputComment(input));
    lines.push_back("Output: " + lutOutputComment(output));
    for (const std::string &line : lines)
        text += "# " + ascii(line) + "\n";
    text += "LUT_3D_SIZE " + std::to_string(size) + "\n";
    if (domain)
        text += "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
    const size_t n = size_t(size) * size * size;
    std::string body(n * 30, '\0');
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        // display values, 0 to 1, whatever rounding left a slide's clear base at
        const double r = std::clamp(double(values[i * 3]), 0.0, 1.0), g = std::clamp(double(values[i * 3 + 1]), 0.0, 1.0),
                     b = std::clamp(double(values[i * 3 + 2]), 0.0, 1.0);
        used += size_t(std::snprintf(&body[used], body.size() - used, "%.6f %.6f %.6f\n", r, g, b));
    }
    body.resize(used);
    return text + body;
}

LookBaker::LookBaker(QRhi *rhi, PackLibrary &packs)
{
    m_renderer = std::make_unique<FilmRenderer>(rhi, FilmRenderer::Blur::Own);
    m_renderer->setSkipsUnchangedFilm(true);
    m_builder = std::make_unique<SetupBuilder>(packs);
}

LookBaker::~LookBaker() = default;

Recipe LookBaker::perPixel(Recipe r)
{
    r.autoExposure = false;
    r.autoDensity = false;
    r.autoGrade = false;
    r.halationEnabled = false;
    r.grainEnabled = false;
    r.couplersDiffusion = 0;
    r.masks.clear();
    r.gradients.clear();                // they cover part of the frame
    r.effects.reset();                  // bloom and glow spread light between pixels
    r.geometry = {};                    // the lattice is not a picture
    r.shaped = false;
    r.outputColorSpace = "sRGB";        // Rec.709 primaries; the encoding is the LUT's own
    return r;
}

LookLUT LookBaker::bake(const Recipe &recipe, int size, LutInput input, LutOutput output)
{
    const Recipe r = perPixel(recipe);
    const int n = size;
    const int width = n * n, height = n;
    if (!m_lattice || m_lattice->first != size || m_lattice->second != input) {
        // the lattice as an image of n*n by n pixels, laid out so its pixel
        // order is the LUT's: red fastest, then green, then blue
        std::vector<double> axis(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k)
            axis[size_t(k)] = lutInputLinear(input, double(k) / (n - 1));
        const auto gamut = lutInputGamut(input);
        std::vector<float> px(size_t(width) * height * 4, 1.0f);
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int rr = 0; rr < n; ++rr) {
                    const size_t o = (size_t(rr) + size_t(n) * (size_t(g) + size_t(n) * size_t(b))) * 4;
                    Vec3d light{axis[size_t(rr)], axis[size_t(g)], axis[size_t(b)]};
                    if (gamut)
                        light = *gamut * light;
                    px[o] = float(light[0]);
                    px[o + 1] = float(light[1]);
                    px[o + 2] = float(light[2]);
                }
        m_renderer->setPicture(px.data(), width, height);
        m_lattice.emplace(size, input);
    }
    BuiltSetup built = m_builder->build(r, float(r.exposureEV), width, height);
    m_renderer->load(built.setup);
    std::vector<float> linear;
    m_renderer->render(nullptr, &linear);
    if (linear.size() != size_t(width) * height * 4)
        throw std::runtime_error("the engine produced no pixels for the lattice");
    LookLUT lut;
    lut.size = n;
    lut.input = input;
    lut.output = output;
    lut.values.resize(size_t(n) * n * n * 3);
    for (size_t i = 0; i < size_t(n) * n * n; ++i)
        for (size_t c = 0; c < 3; ++c)
            lut.values[i * 3 + c] = float(lutOutputEncode(output, double(linear[i * 4 + c])));
    return lut;
}
