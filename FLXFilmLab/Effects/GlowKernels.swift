// Vendored from the author's Glow app (Sources/GlowCore/Kernels.swift), unchanged
// apart from this note, so it can be compared and kept in sync with Glow.

import CoreImage
import Foundation

/// The custom Core Image kernels, compiled from Metal source at runtime.
///
/// Runtime compilation (`CIKernel.kernels(withMetalString:)`) means the app builds with the Xcode
/// Command Line Tools alone; the offline Metal compiler is not needed.
///
/// Each kernel is compiled from its own source string: when several kernels come from one string,
/// Core Image can confuse them in its program cache and run one kernel's code for another.
///
/// Every kernel returns clear where its input is clear. Core Image may evaluate a kernel outside
/// the extent it was applied with and relies on that, so a crop to the same extent would not help.
public final class GlowKernels: @unchecked Sendable {
    public static let shared: GlowKernels = {
        do {
            return try GlowKernels()
        } catch {
            fatalError("Glow: could not compile Core Image kernels: \(error)")
        }
    }()

    let bloomSelect: CIColorKernel
    let bloomComposite: CIColorKernel
    let glowSelect: CIColorKernel
    let halationSelect: CIColorKernel
    let lightCombine: CIColorKernel
    let mix: CIColorKernel
    let maskMix: CIColorKernel
    let maskOverlay: CIColorKernel

    init() throws {
        func kernel(_ name: String) throws -> CIColorKernel {
            guard let body = Self.functions[name] else { throw GlowError.kernel(name) }
            let source = Self.prelude + "extern \"C\" { namespace coreimage {\n" + body + "\n}}\n"
            guard let found = try CIKernel.kernels(withMetalString: source).first(where: { $0.name == name }) as? CIColorKernel
            else { throw GlowError.kernel(name) }
            return found
        }
        bloomSelect = try kernel("glowBloomSelect")
        bloomComposite = try kernel("glowBloomComposite")
        glowSelect = try kernel("glowGlowSelect")
        halationSelect = try kernel("glowHalationSelect")
        lightCombine = try kernel("glowLightCombine")
        mix = try kernel("glowMix")
        maskMix = try kernel("glowMaskMix")
        maskOverlay = try kernel("glowMaskOverlay")
    }

    static let prelude = """
    #include <CoreImage/CoreImage.h>
    using namespace metal;

    namespace glowfx {
        inline float encode1(float c) {
            c = max(c, 0.0f);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * powr(c, 1.0f / 2.4f) - 0.055f;
        }
        inline float decode1(float c) {
            c = max(c, 0.0f);
            return c <= 0.04045f ? c / 12.92f : powr((c + 0.055f) / 1.055f, 2.4f);
        }
        inline float3 encode(float3 c) { return float3(encode1(c.r), encode1(c.g), encode1(c.b)); }
        inline float ramp(float x, float t0, float t1) { return saturate((x - t0) / max(t1 - t0, 1.0e-4f)); }
        // Adds light e to a with a smooth shoulder instead of a hard clip at 1.0.
        inline float softAdd(float a, float e) {
            if (e <= 0.0f) { return a; }
            float r = 1.0f - a;
            if (r <= 1.0e-4f) { return a; }
            return 1.0f - r * exp(-e / r);
        }
    }

    """

    static let functions: [String: String] = [
        // Bloom highlight layer. Input is display-encoded. Output is premultiplied: the colour of the
        // selected highlights with the selection as alpha, like Photoshop's "Layer via Copy".
        "glowBloomSelect": """
        [[stitchable]] float4 glowBloomSelect(sample_t s, float t0, float t1, float preview) {
            float m = glowfx::ramp(dot(s.rgb, float3(0.299f, 0.587f, 0.114f)), t0, t1);
            if (preview > 0.5f) { return float4(m, m, m, 1.0f) * s.a; }
            return float4(s.rgb * m, m);
        }
        """,

        // Blends the blurred highlight layer over the display-encoded base, as Photoshop does with
        // layer alpha and opacity. mode: 0 lighten, 1 screen, 2 add. amount > 1 stacks layers.
        "glowBloomComposite": """
        [[stitchable]] float4 glowBloomComposite(sample_t base, sample_t layer, float amount, float mode) {
            float3 a = base.rgb;
            float alpha = layer.a;
            float3 c = alpha > 1.0e-5f ? layer.rgb / alpha : float3(0.0f);
            float k = amount <= 1.0f ? amount * alpha : 1.0f - powr(max(1.0f - alpha, 0.0f), amount);
            float3 o;
            if (mode < 0.5f) {
                o = a + k * max(c - a, 0.0f);
            } else if (mode < 1.5f) {
                o = a + k * c * max(1.0f - a, 0.0f);
            } else {
                o = a + amount * layer.rgb;
            }
            return float4(o * base.a, base.a);
        }
        """,

        // Glow source: linear image weighted by a highlight mask measured on display brightness.
        "glowGlowSelect": """
        [[stitchable]] float4 glowGlowSelect(sample_t s, float t0, float t1, float preview) {
            float3 lin = s.rgb;
            float m = glowfx::ramp(dot(glowfx::encode(lin), float3(0.2126f, 0.7152f, 0.0722f)), t0, t1);
            if (preview > 0.5f) { float g = glowfx::decode1(m); return float4(g, g, g, 1.0f) * s.a; }
            return float4(lin * m, 1.0f) * s.a;
        }
        """,

        // Halation source: the red channel of the highlights (masked in linear light), tinted.
        "glowHalationSelect": """
        [[stitchable]] float4 glowHalationSelect(sample_t s, float t0, float t1,
                                                 float tintR, float tintG, float tintB, float preview) {
            float3 lin = s.rgb;
            float m = glowfx::ramp(dot(lin, float3(0.2126f, 0.7152f, 0.0722f)), t0, t1);
            if (preview > 0.5f) { float g = glowfx::decode1(m); return float4(g, g, g, 1.0f) * s.a; }
            return float4(float3(tintR, tintG, tintB) * (max(lin.r, 0.0f) * m), 1.0f) * s.a;
        }
        """,

        // Adds blurred glow and halation light to the base, in linear light.
        "glowLightCombine": """
        [[stitchable]] float4 glowLightCombine(sample_t base, sample_t original, sample_t glowLayer, sample_t halationLayer,
                                               float glowAmount, float halationAmount,
                                               float halationT0, float halationT1, float haloOnly) {
            float3 a = base.rgb;
            float3 h = halationLayer.rgb * halationAmount;
            if (haloOnly > 0.5f) {
                float l = dot(original.rgb, float3(0.2126f, 0.7152f, 0.0722f));
                h *= 1.0f - glowfx::ramp(l, halationT0, halationT1);
            }
            float3 e = glowLayer.rgb * glowAmount + h;
            float3 o = float3(glowfx::softAdd(a.r, e.r), glowfx::softAdd(a.g, e.g), glowfx::softAdd(a.b, e.b));
            return float4(o * base.a, base.a);
        }
        """,

        "glowMix": """
        [[stitchable]] float4 glowMix(sample_t a, sample_t b, float t) {
            return mix(a, b, t);
        }
        """,

        // Painted mask: 1 shows the effect, 0 shows the original.
        "glowMaskMix": """
        [[stitchable]] float4 glowMaskMix(sample_t original, sample_t effected, sample_t mask) {
            return mix(original, effected, saturate(mask.r));
        }
        """,

        // Tints the areas where the effect is hidden, like Lightroom's mask overlay.
        "glowMaskOverlay": """
        [[stitchable]] float4 glowMaskOverlay(sample_t image, sample_t mask, float strength) {
            float hidden = 1.0f - saturate(mask.r);
            return float4(mix(image.rgb, float3(0.80f, 0.04f, 0.06f) * image.a, hidden * strength), image.a);
        }
        """,
    ]
}
