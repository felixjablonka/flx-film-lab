//  motion.metal — the Motion mode's per-frame kernel
//
//  One pass from the decoded frame to display light: Y'CbCr planes to the log
//  format's code values (or, for Apple Log, its 0...1 signal), those through
//  the baked print look, out as linear light
//  with Rec.709 primaries. Everything spatial (the Glow effects) runs after
//  this in Core Image.
//
//  Compiled at runtime from source, like flx.metal.

#include <metal_stdlib>
using namespace metal;

// Must match FramePipeline.GradeUniforms.
struct GradeUniforms {
    float2 lumaSize;         // source luma plane, pixels
    float2 chromaSize;       // source chroma plane, texels
    float2 chromaScale;      // chroma texels per luma pixel
    float2 chromaSiting;     // luma-pixel position of chroma sample 0 (0 co-sited, 0.5 between)
    float  texelMax;         // a texel's integer full scale: 65535, or 255 for 8 bits
    float  codeStep;         // integers per code value: 64 where 10 bits top a 16-bit word
    float  yBlack;           // and its levels, in those code values
    float  yWhite;
    float  cMid;
    float  cRange;
    float  kr;               // Y'CbCr matrix
    float  kb;
    int    fullRange;        // code from the signal: 1023 E' (full) or 64 + 876 E' (video)
    int    lutSize;
    int    mode;             // 0 graded, 1 the log footage as QuickTime shows it, 2 code values
    int    taps;             // supersamples per axis when the output is smaller than the source
    int    onSignal;         // the LUT takes the 0...1 signal (Apple Log), not code / 1023
};

// A texel as the frame's own code values. Ten bits sit at the top of a 16-bit
// word and only they count: some decoders fill the six below with copies of
// the top bits, which read as part of the value add up to a code to it.
inline float4 codeTexel(texture2d<float, access::read> t, uint2 i, constant GradeUniforms &u) {
    return floor(round(t.read(i) * u.texelMax) / u.codeStep);
}

// Bilinear by hand: the hardware filter quantises its weights, and a texel
// centre has to come back exactly for the code values to be exact.
inline float4 bilinearCodes(texture2d<float, access::read> t, float2 p, constant GradeUniforms &u) {
    float2 q = p - 0.5f;
    int2 size = int2(t.get_width(), t.get_height());
    int2 i0 = int2(floor(q));
    float2 f = q - float2(i0);
    int2 a = clamp(i0, int2(0), size - 1);
    int2 b = clamp(i0 + 1, int2(0), size - 1);
    float4 v00 = codeTexel(t, uint2(a.x, a.y), u), v10 = codeTexel(t, uint2(b.x, a.y), u);
    float4 v01 = codeTexel(t, uint2(a.x, b.y), u), v11 = codeTexel(t, uint2(b.x, b.y), u);
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

// 10-bit code values R'G'B' at a luma-plane position (texel space, centres at +0.5).
inline float3 codesAt(texture2d<float, access::read> luma,
                      texture2d<float, access::read> chroma,
                      float2 p, constant GradeUniforms &u) {
    float y = bilinearCodes(luma, p, u).r;
    float2 c = (p - 0.5f - u.chromaSiting) * u.chromaScale + 0.5f;
    float2 cbcr = bilinearCodes(chroma, c, u).rg;
    float ey = (y - u.yBlack) / (u.yWhite - u.yBlack);
    float ecb = (cbcr.x - u.cMid) / u.cRange;
    float ecr = (cbcr.y - u.cMid) / u.cRange;
    float r = ey + 2.0f * (1.0f - u.kr) * ecr;
    float b = ey + 2.0f * (1.0f - u.kb) * ecb;
    float g = (ey - u.kr * r - u.kb * b) / (1.0f - u.kr - u.kb);
    float3 e = float3(r, g, b);
    // N-Log and V-Log are defined on the code values the camera records, so a
    // video-range signal keeps its 64 offset.
    return u.fullRange != 0 ? e * 1023.0f : 64.0f + 876.0f * e;
}

inline float3 corner(texture3d<float, access::read> lut, int3 i, int dr, int dg, int db) {
    return lut.read(uint3(i.x + dr, i.y + dg, i.z + db)).rgb;
}

// Tetrahedral interpolation, the same split as LookLUT.sample on the CPU.
inline float3 lookup(texture3d<float, access::read> lut, float3 p, int n) {
    float3 q = clamp(p, 0.0f, 1.0f) * float(n - 1);
    int3 i = min(int3(q), int3(n - 2));
    float3 f = q - float3(i);
    float3 c000 = corner(lut, i, 0, 0, 0), c111 = corner(lut, i, 1, 1, 1);
    if (f.x > f.y) {
        if (f.y > f.z) {
            float3 c100 = corner(lut, i, 1, 0, 0), c110 = corner(lut, i, 1, 1, 0);
            return c000 + f.x * (c100 - c000) + f.y * (c110 - c100) + f.z * (c111 - c110);
        } else if (f.x > f.z) {
            float3 c100 = corner(lut, i, 1, 0, 0), c101 = corner(lut, i, 1, 0, 1);
            return c000 + f.x * (c100 - c000) + f.z * (c101 - c100) + f.y * (c111 - c101);
        } else {
            float3 c001 = corner(lut, i, 0, 0, 1), c101 = corner(lut, i, 1, 0, 1);
            return c000 + f.z * (c001 - c000) + f.x * (c101 - c001) + f.y * (c111 - c101);
        }
    } else {
        if (f.z > f.y) {
            float3 c001 = corner(lut, i, 0, 0, 1), c011 = corner(lut, i, 0, 1, 1);
            return c000 + f.z * (c001 - c000) + f.y * (c011 - c001) + f.x * (c111 - c011);
        } else if (f.z > f.x) {
            float3 c010 = corner(lut, i, 0, 1, 0), c011 = corner(lut, i, 0, 1, 1);
            return c000 + f.y * (c010 - c000) + f.z * (c011 - c010) + f.x * (c111 - c011);
        } else {
            float3 c010 = corner(lut, i, 0, 1, 0), c110 = corner(lut, i, 1, 1, 0);
            return c000 + f.y * (c010 - c000) + f.x * (c110 - c010) + f.z * (c111 - c110);
        }
    }
}

kernel void grade_frame(texture2d<float, access::read>  luma   [[texture(0)]],
                        texture2d<float, access::read>  chroma [[texture(1)]],
                        texture3d<float, access::read>  lut    [[texture(2)]],
                        texture2d<float, access::write> dst    [[texture(3)]],
                        constant GradeUniforms &u              [[buffer(0)]],
                        uint2 gid                              [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;

    // Output pixel centre in source pixels. Smaller outputs (the preview)
    // average a few samples across the footprint rather than aliasing.
    float2 scale = u.lumaSize / float2(dst.get_width(), dst.get_height());
    int taps = max(u.taps, 1);
    float3 code = float3(0.0f);
    for (int j = 0; j < taps; ++j) {
        for (int i = 0; i < taps; ++i) {
            float2 offset = (float2(i, j) + 0.5f) / float(taps);
            code += codesAt(luma, chroma, (float2(gid) + offset) * scale, u);
        }
    }
    code /= float(taps * taps);
    float3 nc = code / 1023.0f;

    float3 rgb;
    if (u.mode == 2) {
        rgb = nc;
    } else if (u.mode == 1) {
        // The untouched log footage the way QuickTime shows a log file
        // tagged BT.2020 / Rec.709 transfer: a 1.961 power on the video-range
        // signal, BT.2020 primaries taken to Rec.709.
        float3 e = clamp((code - 64.0f) / 876.0f, 0.0f, 1.0f);
        float3 l = pow(e, 1.961f);
        rgb = float3( 1.660491f * l.r - 0.587641f * l.g - 0.072850f * l.b,
                     -0.124550f * l.r + 1.132900f * l.g - 0.008349f * l.b,
                     -0.018151f * l.r - 0.100579f * l.g + 1.118730f * l.b);
        rgb = clamp(rgb, 0.0f, 1.0f);
    } else {
        // The LUT is encoded with a 2.4 power, which interpolates evenly
        // across the tonal scale; the rest of the chain works in light.
        // Apple Log is defined on the signal once the levels are out.
        float3 lutIn = (u.onSignal != 0 && u.fullRange == 0) ? (code - 64.0f) / 876.0f : nc;
        float3 v = lookup(lut, lutIn, u.lutSize);
        rgb = pow(max(v, 0.0f), 2.4f);
    }
    dst.write(float4(rgb, 1.0f), gid);
}
