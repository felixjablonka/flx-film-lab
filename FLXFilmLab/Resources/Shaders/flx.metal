//  flx.metal — FLX Film Lab render graph
//
//  A port of the agx-emulsion / spektrafilm per-pixel model
//  (https://github.com/andreavolpato/spektrafilm, GPLv3) to Metal compute.
//  Function names below name their Python originals so the two can be diffed.
//
//  Compiled at runtime from source: the Metal offline toolchain ships with
//  Xcode, and this project builds against Command Line Tools only.

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// Layout constants — must match Constants.swift
// ---------------------------------------------------------------------------

constant int NW = 81;            // 380..780 nm at 5 nm, agx SPECTRAL_SHAPE
constant int NCURVE = 256;       // density-curve samples, agx LOG_EXPOSURE
constant int COUPLER_OVERSAMPLING = 16;   // the pre-coupler curves' finer grid
constant int NCURVE0 = (NCURVE - 1) * COUPLER_OVERSAMPLING + 1;

// Offsets into the packed spectral-table buffer
constant int OFF_FILM_CHANNEL_DENSITY = 0;      // NW*3
constant int OFF_FILM_BASE_DENSITY    = 243;    // NW
constant int OFF_FILM_VALID           = 324;    // NW
constant int OFF_PRINT_ILLUMINANT     = 405;    // NW  (enlarger source x dichroics)
constant int OFF_PAPER_SENSITIVITY    = 486;    // NW*3
constant int OFF_PRINT_CHANNEL_DENSITY = 729;   // NW*3
constant int OFF_PRINT_BASE_DENSITY   = 972;    // NW
constant int OFF_PRINT_VALID          = 1053;   // NW
constant int OFF_VIEWING_ILLUMINANT   = 1134;   // NW
constant int OFF_CMFS                 = 1215;   // NW*3

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

// The print's shape (Renderer.render, step 2d): GeometryMap.outputToSource,
// a print pixel's position to the frame's, both with pixel edges on whole
// numbers. `active` 0 is the frame as it is.
struct GeometryUniforms {
    float2 column0;
    float2 column1;
    float2 offset;
    float2 sourceSize;
    int    active;
    int    pad;
};

struct FilmUniforms {
    float3x3 rgbToXYZ;        // working RGB -> XYZ, CAT16-adapted to the film's reference illuminant
    float exposureScale;      // 2^exposureEV
    int   tcLutSize;
    int   intoTriangle;       // black-and-white stocks: see intoTriangle()
    int   gradientCount;      // exposure gradients in buffer 1
};

// One exposure gradient, handles in pixels. ExposureGradientUniform in Swift.
struct ExposureGradientU {
    float2 a;                 // the full compensation at and behind a
    float2 b;                 // none at and past b
    float  ev;
    float  _p0, _p1, _p2;
};

struct DevelopUniforms {
    float logE0;              // first sample of the film's log-exposure axis
    float logEStep;
    float3 gammaFactor;       // density_curve_gamma, per channel
    float3 densityMax;        // nanmax(density_curves, axis=0)
    float couplersAmount;     // 0 disables the correction pass
    int   isPositive;
    float edgeCap;            // 1: hold the diffused inhibitor's edge effect by `edgeScale`
    float diffusionFraction;  // share of the inhibitor that spreads (1: agx)
};

struct PrintUniforms {
    float3x3 xyzToRGB;        // XYZ under the viewing illuminant -> output RGB
    float3 exposureFactorMidgray;
    float3 preflashRaw;
    float printExposure;      // linear multiplier (includes the Density control)
    float cmfNormalization;
    float toneLogE0;          // paper log-exposure axis, for the tone LUT
    float toneLogEStep;
    int   toneLutWidth;
    float gradeSpanMin;       // local-grade axis of the tone LUT, in ISO-R points
    float gradeSpanMax;
    int   hasDodgeBurn;
    float glareAmount;        // print_render.glare.percent / 100, deterministic part
    float3 illuminantXYZ;
    int   gamutCompress;
    float gamutThreshold;
    float gamutLimit;
    float gamutPower;
    // Transparency mode: skip the enlarger and paper entirely and read the
    // developed film directly, the way a slide is viewed on a light table.
    // Its white point on the clear base is folded into xyzToRGB.
    int   scanFilm;
    float _pad3, _pad4, _pad5;
    // Output RGB <-> Oklab LMS (D65-relative), for whichever output space is
    // selected, so gamut compression measures hue and chroma in that space.
    float3x3 gamutRGBToLMS;
    float3x3 gamutLMSToRGB;
    // Brightness / Shadows / Highlights: a density change for all three dye
    // layers, tabulated by mean density over [0, toneShiftMax]. 0 = none.
    float toneShiftMax;
    int   toneShiftCount;
    float _pad6, _pad7;
};

// ---------------------------------------------------------------------------
// Interpolation primitives
// ---------------------------------------------------------------------------

// Mitchell–Netravali B = C = 1/3, matching agx utils/fast_interp_lut.mitchell_weight
inline float mitchellWeight(float t) {
    const float B = 1.0f / 3.0f;
    const float C = 1.0f / 3.0f;
    float x = fabs(t);
    float x2 = x * x;
    float x3 = x2 * x;
    if (x < 1.0f) {
        return (1.0f / 6.0f) * ((12.0f - 9.0f * B - 6.0f * C) * x3 +
                                (-18.0f + 12.0f * B + 6.0f * C) * x2 + (6.0f - 2.0f * B));
    }
    if (x < 2.0f) {
        return (1.0f / 6.0f) * ((-B - 6.0f * C) * x3 + (6.0f * B + 30.0f * C) * x2 +
                                (-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C));
    }
    return 0.0f;
}

// agx utils/fast_interp_lut.safe_index — symmetric reflection at the edges
inline int safeIndex(int idx, int L) {
    if (idx < 0) return -idx;
    if (idx >= L) return 2 * (L - 1) - idx;
    return idx;
}

// agx utils/fast_interp_lut._cubic_interp_lut_at_2d.
// The LUT is a numpy (L, L, 3) array uploaded row-major, so its first axis is
// the texture's y and its second axis is x — hence read(uint2(second, first)).
inline float3 sampleTcLut(texture2d<float, access::read> lut, float2 tc, int L) {
    float2 c = clamp(tc, 0.0f, 1.0f) * float(L - 1);

    int2 base;
    float2 frac;
    for (int d = 0; d < 2; ++d) {
        float v = c[d];
        if (v >= float(L - 1)) { base[d] = L - 2; frac[d] = 1.0f; }
        else                   { base[d] = int(floor(v)); frac[d] = v - float(base[d]); }
    }

    float w0[4], w1[4];
    for (int k = 0; k < 4; ++k) {
        w0[k] = mitchellWeight(frac.x + 1.0f - float(k));
        w1[k] = mitchellWeight(frac.y + 1.0f - float(k));
    }

    float3 acc = float3(0.0f);
    float weightSum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        int xi = safeIndex(base.x - 1 + i, L);
        for (int j = 0; j < 4; ++j) {
            int yj = safeIndex(base.y - 1 + j, L);
            float w = w0[i] * w1[j];
            weightSum += w;
            acc += w * lut.read(uint2(uint(yj), uint(xi))).rgb;
        }
    }
    return weightSum != 0.0f ? acc / weightSum : acc;
}

// agx utils/fast_interp.fast_interp — linear, clamped to the endpoint values.
// The log-exposure axis is uniform, so the search collapses to arithmetic.
inline float3 interpCurves(constant float *curves, float3 x, float logE0, float logEStep, float3 gamma,
                           int n = NCURVE) {
    float3 out;
    for (int c = 0; c < 3; ++c) {
        // agx divides the axis by gamma; multiplying x is the same map and avoids a divide per pixel
        float pos = (x[c] * gamma[c] - logE0) / logEStep;
        pos = clamp(pos, 0.0f, float(n - 1));
        int i0 = int(floor(pos));
        int i1 = min(i0 + 1, n - 1);
        float t = pos - float(i0);
        float y0 = curves[i0 * 3 + c];
        float y1 = curves[i1 * 3 + c];
        out[c] = mix(y0, y1, t);
    }
    return out;
}

// agx utils/spectral_upsampling._tri2quad
inline float2 tri2quad(float2 xy) {
    float y = xy.y / max(1.0f - xy.x, 1e-10f);
    float x = (1.0f - xy.x) * (1.0f - xy.x);
    return clamp(float2(x, y), 0.0f, 1.0f);
}

// A chromaticity outside the xy triangle (x < 0, y < 0 or x + y > 1) has a
// negative X, Y or Z: demosaicing noise in deep shadow, or a colour at the
// edge of the working gamut that the adaptation to the stock's white pushed
// across. agx clamps the LUT coordinate, which lands it on the LUT's edge — an
// extreme purple-red. A colour film spreads that over three layers; a
// black-and-white film sees it as several times brighter than anything real,
// and a dark red ear printed as a light patch on Tri-X. For those stocks the
// chromaticity is pulled back along the line to white onto the triangle's
// edge, and the LUT's own gamut compression handles it like any saturated
// colour. Colour stocks keep agx's clamp, which the parity references hold
// them to. RenderSetupBuilder.intoTriangle is the CPU twin.
inline float2 intoTriangle(float2 xy, float2 white) {
    float2 d = xy - white;
    float t = 1.0f;
    float s = d.x + d.y;
    if (s > 0.0f)   t = min(t, (1.0f - white.x - white.y) / s);
    if (d.x < 0.0f) t = min(t, -white.x / d.x);
    if (d.y < 0.0f) t = min(t, -white.y / d.y);
    return white + d * max(t, 0.0f);
}

// ---------------------------------------------------------------------------
// Stage 1 — filming.expose : working RGB -> film log exposure
// ---------------------------------------------------------------------------

/// The stacked exposure gradients' compensation in EV at pixel position `p`:
/// each one's smoothstep along its handles, as the darkroom gradient's, and
/// the stack summed. ExposureGradient.exposureEV in Recipe.swift is the same
/// on the CPU.
static float gradientEV(float2 p, constant ExposureGradientU *gradients, int count)
{
    float ev = 0.0f;
    for (int i = 0; i < count; i++) {
        float2 d = gradients[i].b - gradients[i].a;
        float lengthSquared = dot(d, d);
        if (lengthSquared <= 1e-9f) continue;
        float t = clamp(dot(p - gradients[i].a, d) / lengthSquared, 0.0f, 1.0f);
        ev += gradients[i].ev * (1.0f - t * t * (3.0f - 2.0f * t));
    }
    return ev;
}

kernel void film_expose(texture2d<float, access::read>  src   [[texture(0)]],
                        texture2d<float, access::write> dst   [[texture(1)]],
                        texture2d<float, access::read>  tcLut [[texture(2)]],
                        constant FilmUniforms &u              [[buffer(0)]],
                        constant ExposureGradientU *gradients [[buffer(1)]],
                        uint2 gid                             [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;

    float3 rgb = src.read(gid).rgb;
    float3 xyz = u.rgbToXYZ * rgb;

    float b = xyz.x + xyz.y + xyz.z;
    if (!isfinite(b)) b = 0.0f;                       // agx: b = np.nan_to_num(b)
    float2 xy = xyz.xy / max(b, 1e-10f);

    if (u.intoTriangle != 0) {
        float3 w = u.rgbToXYZ * float3(1.0f);         // the stock's reference white
        xy = intoTriangle(xy, w.xy / (w.x + w.y + w.z));
    }
    float3 raw = sampleTcLut(tcLut, tri2quad(xy), u.tcLutSize) * b;
    raw *= u.exposureScale;
    // Exposure compensation where the gradients cover: more or less light on
    // the film, before halation spreads any of it.
    if (u.gradientCount > 0) raw *= exp2(gradientEV(float2(gid) + 0.5f, gradients, u.gradientCount));

    // Linear irradiance, not log: halation and scatter propagate *light*, so
    // they have to happen before the characteristic curve. `film_log` closes
    // the stage.
    dst.write(float4(raw, 1.0f), gid);
}

kernel void film_log(texture2d<float, access::read>  src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     uint2 gid                           [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 raw = src.read(gid).rgb;
    dst.write(float4(log10(max(raw, 0.0f) + 1e-10f), 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Halation and in-emulsion scatter
//
// agx model/diffusion.apply_halation_um. Both stages act on linear irradiance
// *before* the film's characteristic curve, which is what makes halation bloom
// only where the highlights are genuinely bright rather than everywhere.
//
// The blurs themselves are MPS passes on the host side; these kernels do the
// per-channel mixing, because the sigmas differ per channel and MPS blurs all
// channels alike. Each channel's blur is run over the whole image and this
// gather picks the matching one out.
// ---------------------------------------------------------------------------

kernel void gather_channels(texture2d<float, access::read>  blurR [[texture(0)]],
                            texture2d<float, access::read>  blurG [[texture(1)]],
                            texture2d<float, access::read>  blurB [[texture(2)]],
                            texture2d<float, access::write> dst   [[texture(3)]],
                            uint2 gid                             [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(float4(blurR.read(gid).r, blurG.read(gid).g, blurB.read(gid).b, 1.0f), gid);
}

struct ScatterUniforms {
    float3 tailWeight;      // w_s per channel
    float amount;           // fraction of photons that scatter at all
    float _pad;
};

// E1 = (1 - s) * E0 + s * [(1 - w) * core + w * tail]
kernel void scatter_mix(texture2d<float, access::read>  src  [[texture(0)]],
                        texture2d<float, access::read>  core [[texture(1)]],
                        texture2d<float, access::read>  tail [[texture(2)]],
                        texture2d<float, access::write> dst  [[texture(3)]],
                        constant ScatterUniforms &u          [[buffer(0)]],
                        uint2 gid                            [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 e0 = src.read(gid).rgb;
    float3 scattered = (1.0f - u.tailWeight) * core.read(gid).rgb + u.tailWeight * tail.read(gid).rgb;
    dst.write(float4(mix(e0, scattered, u.amount), 1.0f), gid);
}

struct HalationUniforms {
    float3 strength;        // a_tot per channel
    int renormalize;
};

// E2 = E1 + a * Σ_k w_k G(sigma_h * sqrt(k)) E1, optionally renormalised so the
// added light does not also raise the overall exposure.
kernel void halation_add(texture2d<float, access::read>  src     [[texture(0)]],
                         texture2d<float, access::read>  blurred [[texture(1)]],
                         texture2d<float, access::write> dst     [[texture(2)]],
                         constant HalationUniforms &u            [[buffer(0)]],
                         uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 e = src.read(gid).rgb + u.strength * blurred.read(gid).rgb;
    if (u.renormalize != 0) e /= (1.0f + u.strength);
    dst.write(float4(e, 1.0f), gid);
}

// Weighted accumulation of one bounce into the halation sum.
kernel void accumulate_scaled(texture2d<float, access::read>       src   [[texture(0)]],
                              texture2d<float, access::read_write> accum [[texture(1)]],
                              constant float &weight                     [[buffer(0)]],
                              constant int &reset                        [[buffer(1)]],
                              uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= accum.get_width() || gid.y >= accum.get_height()) return;
    float3 add = weight * src.read(gid).rgb;
    float3 base = reset != 0 ? float3(0.0f) : accum.read(gid).rgb;
    accum.write(float4(base + add, 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Grain — agx model/grain.layer_particle_model
//
// Development is a counting process: each pixel holds a finite number of silver
// halide particles, a Poisson draw sets how many are available, and a Binomial
// draw sets how many actually develop at that exposure. Grain is therefore
// loudest in the midtones and quiet at both ends, which is what film does and
// what an additive noise layer does not.
// ---------------------------------------------------------------------------

// Counter-based hashing (Wang / PCG style): deterministic per pixel, so the
// same frame grains identically every render and tiles seamlessly on export.
inline uint hashCombine(uint a, uint b) {
    uint h = a * 0x9E3779B9u + b + 0x85EBCA6Bu;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline float uniformFrom(thread uint &state) {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) * (1.0f / 16777216.0f);
}

// Box-Muller, used for the normal approximations below.
inline float normalFrom(thread uint &state) {
    float u1 = max(uniformFrom(state), 1e-7f);
    float u2 = uniformFrom(state);
    return sqrt(-2.0f * log(u1)) * cos(6.2831853f * u2);
}

inline float poissonSample(float lambda, thread uint &state) {
    if (lambda <= 0.0f) return 0.0f;
    if (lambda < 30.0f) {
        // Knuth: exact, and cheap while lambda is small
        float target = exp(-lambda);
        float product = uniformFrom(state);
        float k = 0.0f;
        while (product > target && k < 200.0f) { k += 1.0f; product *= uniformFrom(state); }
        return k;
    }
    return max(round(lambda + sqrt(lambda) * normalFrom(state)), 0.0f);
}

inline float binomialSample(float n, float p, thread uint &state) {
    if (n <= 0.0f || p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return n;
    if (n < 25.0f) {
        float count = 0.0f;
        for (float i = 0.0f; i < n; i += 1.0f) { if (uniformFrom(state) < p) count += 1.0f; }
        return count;
    }
    float mean = n * p;
    float sd = sqrt(max(n * p * (1.0f - p), 1e-9f));
    return clamp(round(mean + sd * normalFrom(state)), 0.0f, n);
}

struct GrainUniforms {
    float3 particlesPerPixel;
    float3 densityMin;
    float3 densityMax;      // curve D-max plus densityMin, as agx computes it
    float3 uniformity;
    float amount;           // creative scale on the deviation from the clean density
    uint  seed;
    uint  monochrome;       // one silver layer: every channel draws the same grains
    float _pad1;
};

kernel void film_grain(texture2d<float, access::read>  src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       constant GrainUniforms &u           [[buffer(0)]],
                       uint2 gid                           [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 density = src.read(gid).rgb + u.densityMin;
    float3 out;
    for (int c = 0; c < 3; ++c) {
        float dMax = max(u.densityMax[c], 1e-4f);
        float n = max(u.particlesPerPixel[c], 1e-4f);
        float p = clamp(density[c] / dMax, 1e-6f, 1.0f - 1e-6f);
        float odParticle = dMax / n;

        // A black-and-white film's three channels are one layer: drawing them
        // from one random field keeps the grain itself neutral.
        uint layer = u.monochrome != 0 ? 0u : uint(c);
        uint state = hashCombine(hashCombine(gid.x, gid.y), u.seed + layer * 7919u);
        float saturation = 1.0f - p * u.uniformity[c] * (1.0f - 1e-6f);
        float seeds = poissonSample(n / saturation, state);
        float developed = binomialSample(seeds, p, state);
        float grained = developed * odParticle * saturation;

        // `amount` scales only the deviation, so 0 returns the clean density
        // exactly and 2 doubles the visible granularity without shifting tone.
        out[c] = (grained - density[c]) * u.amount;
    }
    // The grain alone, as a deviation from the clean density: it is softened
    // before it is laid on (film_grain_apply).
    dst.write(float4(out, 1.0f), gid);
}

// The softened grain on the clean density.
kernel void film_grain_apply(texture2d<float, access::read>  clean [[texture(0)]],
                             texture2d<float, access::read>  grain [[texture(1)]],
                             texture2d<float, access::write> dst   [[texture(2)]],
                             uint2 gid                             [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(float4(clean.read(gid).rgb + grain.read(gid).rgb, 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Stage 2 — filming.develop : log exposure -> negative CMY density
// ---------------------------------------------------------------------------

// First pass: the stock's own characteristic curves, min-subtracted.
kernel void film_develop(texture2d<float, access::read>  logE [[texture(0)]],
                         texture2d<float, access::write> dst  [[texture(1)]],
                         constant float *curves               [[buffer(0)]],
                         constant DevelopUniforms &u          [[buffer(1)]],
                         uint2 gid                            [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 le = logE.read(gid).rgb;
    float3 density = interpCurves(curves, le, u.logE0, u.logEStep, u.gammaFactor);
    dst.write(float4(density, 1.0f), gid);
}

// DIR couplers, part 1: how much inhibitor each layer released.
// agx model/couplers.compute_exposure_correction_dir_couplers, up to the blur.
kernel void couplers_amount(texture2d<float, access::read>  density [[texture(0)]],
                            texture2d<float, access::write> dst     [[texture(1)]],
                            constant float3x3 &matrix               [[buffer(0)]],
                            constant DevelopUniforms &u             [[buffer(1)]],
                            uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 d = density.read(gid).rgb;
    float3 silver = u.isPositive != 0 ? (u.densityMax - d) : d;
    // density_silver[..., k] released by donor k acts on receiver m via M[k][m];
    // float3x3 is column-major, so the transpose-free form is a row dot per receiver.
    float3 correction;
    for (int m = 0; m < 3; ++m) {
        correction[m] = silver.x * matrix[0][m] + silver.y * matrix[1][m] + silver.z * matrix[2][m];
    }
    dst.write(float4(correction, 1.0f), gid);
}

// DIR couplers, part 2: re-develop against the pre-coupler curves at the
// inhibited exposure. `curves0` is compute_density_curves_before_dir_couplers(),
// on a grid COUPLER_OVERSAMPLING times finer than the density curves', and
// `edgeScale` shares that grid.
kernel void film_develop_couplers(texture2d<float, access::read>  logE       [[texture(0)]],
                                  texture2d<float, access::read>  correction [[texture(1)]],
                                  texture2d<float, access::write> dst        [[texture(2)]],
                                  texture2d<float, access::read>  local      [[texture(3)]],
                                  constant float *curves0                    [[buffer(0)]],
                                  constant DevelopUniforms &u                [[buffer(1)]],
                                  constant float *edgeScale                  [[buffer(2)]],
                                  uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float step0 = u.logEStep / float(COUPLER_OVERSAMPLING);
    float3 x = logE.read(gid).rgb;
    float3 c = correction.read(gid).rgb;
    if (u.edgeCap > 0.5f || u.diffusionFraction < 0.999f) {
        // Keep the local inhibition whole and only part of what diffusion adds
        // or takes away at an edge: the share that spreads, held where the
        // edge gain would run past the stock's measured one (edgeGainScale).
        float3 l = local.read(gid).rgb;
        float3 keep = float3(u.diffusionFraction);
        if (u.edgeCap > 0.5f) keep *= interpCurves(edgeScale, x, u.logE0, step0, u.gammaFactor, NCURVE0);
        c = l + keep * (c - l);
    }
    float3 le = x - c;
    float3 density = interpCurves(curves0, le, u.logE0, step0, u.gammaFactor, NCURVE0);
    dst.write(float4(density, 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Stage 3 — printing.expose + printing.develop + scanning.scan, fused
//
// All three are per-pixel, so they run as one kernel: the tone controls that
// live between them stay interactive without a round trip through memory.
// ---------------------------------------------------------------------------

// agx utils/gamut_compression.reinhard_knee — the shape shared by every
// algorithm in that module.
inline float reinhardKnee(float d, float threshold, float limit, float power) {
    if (d <= threshold) return d;
    float s = limit - threshold;
    if (s <= 0.0f) return d;
    float n = (d - threshold) / s;
    return threshold + s * n / pow(1.0f + pow(n, power), 1.0f / power);
}

// Oklab's nonlinear stage: cube-rooted LMS <-> Lab (Ottosson's M2 and inverse).
inline float3 lmsToOklab(float3 lms) {
    float3 c = sign(lms) * pow(fabs(lms), float3(1.0f / 3.0f));
    return float3(0.2104542553f * c.x + 0.7936177850f * c.y - 0.0040720468f * c.z,
                  1.9779984951f * c.x - 2.4285922050f * c.y + 0.4505937099f * c.z,
                  0.0259040371f * c.x + 0.7827717662f * c.y - 0.8086757660f * c.z);
}

inline float3 oklabToLMS(float3 lab) {
    float3 c = float3(lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z,
                      lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z,
                      lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z);
    return c * c * c;
}

inline bool inOutputGamut(float L, float2 ab, constant PrintUniforms &u) {
    float3 rgb = u.gamutLMSToRGB * oklabToLMS(float3(L, ab));
    return all(rgb >= -1e-6f) && all(rgb <= 1.0f + 1e-6f);
}

// agx utils/gamut_compression.compress_rgb_oklch_chroma: chroma as a fraction
// of the most the output space can hold at this lightness and hue, through the
// knee, with lightness and hue held. Colours outside the space come back along
// their own hue line; neutrals never move. agx reads the boundary from a cached
// table; here it is found per pixel by bisection, which the GPU absorbs.
//
// What this replaced measured the excess against Oklab chroma itself, not
// against the boundary, and then clamped per channel: in practice a clip, with
// the hue shifts that brings, and with sRGB matrices whatever the output space.
inline float3 compressGamut(float3 rgb, constant PrintUniforms &u) {
    if (u.gamutCompress == 0) return rgb;
    float3 lab = lmsToOklab(u.gamutRGBToLMS * rgb);
    float L = lab.x;
    if (L <= 0.0f) return float3(0.0f);
    float chroma = length(lab.yz);
    if (chroma <= 1e-7f) return clamp(rgb, 0.0f, 1.0f);
    float2 hue = lab.yz / chroma;

    // At L >= 1 only white fits; the bisection then converges on zero chroma.
    float Lb = min(L, 1.0f);
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; ++i) {
        float mid = 0.5f * (lo + hi);
        if (inOutputGamut(Lb, hue * mid, u)) lo = mid; else hi = mid;
    }
    float cMax = max(lo, 1e-9f);

    float d = chroma / cMax;
    float dCompressed = reinhardKnee(d, u.gamutThreshold, u.gamutLimit, u.gamutPower);
    if (dCompressed >= d && L <= 1.0f) return rgb;
    return u.gamutLMSToRGB * oklabToLMS(float3(Lb, hue * dCompressed * cMax));
}

kernel void print_and_scan(texture2d<float, access::read>  cmyFilm    [[texture(0)]],
                           texture2d<float, access::write> dst        [[texture(1)]],
                           texture2d<float, access::read>  toneLut    [[texture(2)]],
                           texture2d<float>                dodgeBurn  [[texture(3)]],
                           constant float *tables                     [[buffer(0)]],
                           constant PrintUniforms &u                  [[buffer(1)]],
                           constant float *toneShift                  [[buffer(2)]],
                           constant GeometryUniforms &shape           [[buffer(3)]],
                           uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;

    float3 cmy = cmyFilm.read(gid).rgb;

    // The map is smaller than the frame (feathered masks are low-frequency) and
    // is sampled bilinearly, so it costs the same at any output resolution.
    // It is drawn on the frame, so a shaped print finds it through its shape.
    constexpr sampler maskSampler(coord::normalized, filter::linear, address::clamp_to_edge);
    float2 db = float2(0.0f);
    if (u.hasDodgeBurn != 0) {
        float2 uv = (float2(gid) + 0.5f) / float2(dst.get_width(), dst.get_height());
        if (shape.active != 0) {
            float2 o = float2(gid) + 0.5f;
            uv = (shape.column0 * o.x + shape.column1 * o.y + shape.offset) / shape.sourceSize;
        }
        db = dodgeBurn.sample(maskSampler, uv).rg;
    }
    float localGrade = db.y;                          // ISO-R points

    float3 toneInput;
    if (u.scanFilm != 0) {
        // Transparency: the developed film *is* the picture. The tone LUT is
        // indexed in density rather than print log-exposure, so brightness and
        // dodge/burn both arrive as density offsets — one stop is log10(2), and
        // more density is darker, matching the print convention.
        float stops = log2(max(u.printExposure, 1e-6f)) + db.x;
        toneInput = cmy + stops * 0.30103f;
    } else {
        // ---- printing.expose : 81-band integral through the negative -------
        float3 raw = float3(0.0f);
        for (int l = 0; l < NW; ++l) {
            float valid = tables[OFF_FILM_VALID + l];
            if (valid == 0.0f) continue;              // agx zeroes non-finite transmittance
            float dSpec = cmy.x * tables[OFF_FILM_CHANNEL_DENSITY + l * 3 + 0]
                        + cmy.y * tables[OFF_FILM_CHANNEL_DENSITY + l * 3 + 1]
                        + cmy.z * tables[OFF_FILM_CHANNEL_DENSITY + l * 3 + 2]
                        + tables[OFF_FILM_BASE_DENSITY + l];
            float light = exp10(-dSpec) * tables[OFF_PRINT_ILLUMINANT + l];
            raw.x += light * tables[OFF_PAPER_SENSITIVITY + l * 3 + 0];
            raw.y += light * tables[OFF_PAPER_SENSITIVITY + l * 3 + 1];
            raw.z += light * tables[OFF_PAPER_SENSITIVITY + l * 3 + 2];
        }
        raw *= u.exposureFactorMidgray;
        raw += u.preflashRaw;
        raw *= u.printExposure;
        raw *= exp2(db.x);                            // dodge & burn, in stops
        toneInput = log10(max(raw, 0.0f) + 1e-10f);
    }

    float3 logRawPrint = toneInput;

    // ---- paper curve + tone controls, baked into a 2D transfer LUT ---------
    // x: print log exposure (or, in transparency mode, density). y: local
    // grade, so a mask can burn through a different filter without needing its
    // own LUT.
    //
    // Interpolated by hand rather than through `filter::linear`: the hardware
    // filter quantises its sub-texel weight, and across the transparency LUT's
    // ~5-density axis that showed up as a 0.004 error in the output. Reading
    // texels and lerping in float32 makes the identity case exact, which is
    // what lets the parity tests hold at defaults.
    float gradeSpan = max(u.gradeSpanMax - u.gradeSpanMin, 1e-6f);
    int lutHeight = int(toneLut.get_height());
    float gradePos = clamp((localGrade - u.gradeSpanMin) / gradeSpan, 0.0f, 1.0f) * float(lutHeight - 1);
    int y0 = int(floor(gradePos));
    int y1 = min(y0 + 1, lutHeight - 1);
    float ty = gradePos - float(y0);

    float3 cmyPrint;
    for (int c = 0; c < 3; ++c) {
        float pos = clamp((logRawPrint[c] - u.toneLogE0) / u.toneLogEStep,
                          0.0f, float(u.toneLutWidth - 1));
        int x0 = int(floor(pos));
        int x1 = min(x0 + 1, u.toneLutWidth - 1);
        float tx = pos - float(x0);
        float v00 = toneLut.read(uint2(uint(x0), uint(y0)))[c];
        float v10 = toneLut.read(uint2(uint(x1), uint(y0)))[c];
        float v01 = toneLut.read(uint2(uint(x0), uint(y1)))[c];
        float v11 = toneLut.read(uint2(uint(x1), uint(y1)))[c];
        cmyPrint[c] = mix(mix(v00, v10, tx), mix(v01, v11, tx), ty);
    }

    // ---- Brightness, Shadows, Highlights -----------------------------------
    // One density change for all three layers, looked up by their mean, so a
    // tone moves lighter or darker without its hue turning (ToneCurve.NeutralShift).
    if (u.toneShiftCount > 1) {
        float meanDensity = (cmyPrint.x + cmyPrint.y + cmyPrint.z) / 3.0f;
        float pos = clamp(meanDensity / u.toneShiftMax, 0.0f, 1.0f) * float(u.toneShiftCount - 1);
        int i0 = min(int(floor(pos)), u.toneShiftCount - 2);
        float shift = mix(toneShift[i0], toneShift[i0 + 1], pos - float(i0));
        cmyPrint = max(cmyPrint + shift, 0.0f);
    }

    // ---- scanning.scan : print dyes under the viewing illuminant -----------
    float3 xyz = float3(0.0f);
    for (int l = 0; l < NW; ++l) {
        float valid = tables[OFF_PRINT_VALID + l];
        if (valid == 0.0f) continue;
        float dSpec = cmyPrint.x * tables[OFF_PRINT_CHANNEL_DENSITY + l * 3 + 0]
                    + cmyPrint.y * tables[OFF_PRINT_CHANNEL_DENSITY + l * 3 + 1]
                    + cmyPrint.z * tables[OFF_PRINT_CHANNEL_DENSITY + l * 3 + 2]
                    + tables[OFF_PRINT_BASE_DENSITY + l];
        float light = exp10(-dSpec) * tables[OFF_VIEWING_ILLUMINANT + l];
        xyz.x += light * tables[OFF_CMFS + l * 3 + 0];
        xyz.y += light * tables[OFF_CMFS + l * 3 + 1];
        xyz.z += light * tables[OFF_CMFS + l * 3 + 2];
    }
    xyz /= u.cmfNormalization;

    if (u.glareAmount > 0.0f) {
        xyz += u.glareAmount * u.illuminantXYZ;       // agx model/glare.add_glare, mean term
    }

    float3 rgb = u.xyzToRGB * xyz;
    rgb = compressGamut(rgb, u);

    dst.write(float4(rgb, 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Grain as texture only — see Renderer.render, step 3b
// ---------------------------------------------------------------------------

// Scale each pixel so that its neighbourhood averages to the grain-free light.
// A gain rather than an offset: subtracting the grain's local average pushed
// the dark channel of saturated colours below zero, where every file clips it.
kernel void grain_restore_mean(texture2d<float, access::read_write> print         [[texture(0)]],
                               texture2d<float, access::read>       meanGrainy    [[texture(1)]],
                               texture2d<float, access::read>       meanGrainFree [[texture(2)]],
                               uint2 gid                                          [[thread_position_in_grid]])
{
    if (gid.x >= print.get_width() || gid.y >= print.get_height()) return;
    float3 gain = clamp(max(meanGrainFree.read(gid).rgb, 0.0f) / max(meanGrainy.read(gid).rgb, 1e-6f),
                        0.25f, 4.0f);
    print.write(float4(max(print.read(gid).rgb, 0.0f) * gain, 1.0f), gid);
}


// Catmull-Rom weights for the four taps around a sample `t` past the second.
static inline float4 catmull_rom(float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * float4(-t3 + 2.0f * t2 - t,
                         3.0f * t3 - 5.0f * t2 + 2.0f,
                         -3.0f * t3 + 4.0f * t2 + t,
                         t3 - t2);
}

// The developed negative turned, mirrored, levelled and cropped, before the
// grain is drawn on it. Bicubic: bilinear softens a levelled picture in
// bands, wherever its samples fall between pixels. Turns and crops land on
// pixel centres, where the weights are 0, 1, 0, 0 and the pixels are copied
// exactly. Nothing goes past the four nearest pixels, so a hard edge does not
// ring.
kernel void apply_geometry(texture2d<float, access::read>  src [[texture(0)]],
                           texture2d<float, access::write> dst [[texture(1)]],
                           constant GeometryUniforms &u        [[buffer(0)]],
                           uint2 gid                           [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 o = float2(gid) + 0.5f;
    float2 p = u.column0 * o.x + u.column1 * o.y + u.offset - 0.5f;   // pixel centres on whole numbers
    float2 nearest = round(p);
    if (all(abs(p - nearest) < 1e-3f)) p = nearest;
    float2 f = floor(p);
    float4 wx = catmull_rom(p.x - f.x), wy = catmull_rom(p.y - f.y);
    int2 base = int2(f) - 1;
    int2 last = int2(src.get_width(), src.get_height()) - 1;
    float3 c = 0.0f;
    for (int j = 0; j < 4; j++) {
        uint y = uint(clamp(base.y + j, 0, last.y));
        float3 row = 0.0f;
        for (int i = 0; i < 4; i++) {
            row += wx[i] * src.read(uint2(uint(clamp(base.x + i, 0, last.x)), y)).rgb;
        }
        c += wy[j] * row;
    }
    uint2 n0 = uint2(clamp(int2(f), int2(0), last)), n1 = uint2(clamp(int2(f) + 1, int2(0), last));
    float3 a = src.read(n0).rgb, b = src.read(uint2(n1.x, n0.y)).rgb;
    float3 d = src.read(uint2(n0.x, n1.y)).rgb, e = src.read(n1).rgb;
    dst.write(float4(clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e))), 1.0f), gid);
}

// ---------------------------------------------------------------------------
// Display transform — output encoding for the on-screen texture
// ---------------------------------------------------------------------------

// `coverage` 1 makes the parts of the print outside the frame transparent,
// premultiplied, so the canvas shows through the empty corners of a levelled
// frame while its crop is drawn.
kernel void encode_display(texture2d<float, access::read>  src   [[texture(0)]],
                           texture2d<float, access::write> dst   [[texture(1)]],
                           constant int &encoding                [[buffer(0)]],
                           constant int &coverage                [[buffer(1)]],
                           constant GeometryUniforms &shape      [[buffer(2)]],
                           uint2 gid                             [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float3 c = max(src.read(gid).rgb, 0.0f);
    if (encoding == 1) {
        // sRGB / Display P3 piecewise transfer function
        c = select(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, c <= 0.0031308f);
    } else if (encoding == 2) {
        c = pow(c, 1.0f / 1.80078125f);               // ProPhoto RGB (ROMM)
    }
    float a = 1.0f;
    if (coverage == 1) {
        float2 o = float2(gid) + 0.5f;
        float2 s = shape.column0 * o.x + shape.column1 * o.y + shape.offset;
        float inside = min(min(s.x, s.y), min(shape.sourceSize.x - s.x, shape.sourceSize.y - s.y));
        a = clamp(inside + 0.5f, 0.0f, 1.0f);
    }
    dst.write(float4(clamp(c, 0.0f, 1.0f) * a, a), gid);
}
