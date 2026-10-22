// Vendored from Glow (~/Documents/Glow, Sources/GlowCore/Pipeline.swift), unchanged
// apart from this note, so it can be compared and kept in sync with Glow.

import CoreImage
import Foundation

public enum EffectKind: String, CaseIterable, Codable, Sendable {
    case bloom, glow, halation
}

/// Builds the Core Image graph for a look.
///
/// Input images must be opaque, in Core Image's linear working space, with their extent at the
/// origin. `longEdge` is the long edge of that image in pixels; blur sizes scale with it.
public enum GlowPipeline {
    public static func apply(_ look: GlowLook, to image: CIImage, longEdge: CGFloat) -> CIImage {
        let kernels = GlowKernels.shared
        let extent = image.extent
        var result = image
        if look.bloom.isActive {
            result = bloom(look.bloom, image: image, longEdge: longEdge)
        }

        let glowOn = look.glow.isActive
        let halationOn = look.halation.isActive
        guard glowOn || halationOn else { return result }

        var glowLayer = CIImage.black
        if glowOn {
            let source = run(kernels.glowSelect, extent, [image, look.glow.threshold / 100, 1.0, 0.0])
            glowLayer = blur(source, sigma: BlurSize.sigmaPixels(look.glow.size, longEdge: longEdge), extent: extent)
        }
        let h = halationThresholds(look.halation)
        var halationLayer = CIImage.black
        if halationOn {
            let tint = look.halation.tint
            let source = run(kernels.halationSelect, extent, [image, h.t0, h.t1, tint.r, tint.g, tint.b, 0.0])
            halationLayer = blur(source, sigma: BlurSize.sigmaPixels(look.halation.size, longEdge: longEdge), extent: extent)
        }
        return run(kernels.lightCombine, extent, [
            result, image, glowLayer, halationLayer,
            glowOn ? look.glow.amount / 100 : 0.0,
            halationOn ? look.halation.amount / 100 : 0.0,
            h.t0, h.t1, look.halation.haloOnly ? 1.0 : 0.0,
        ])
    }

    /// The highlight selection of one effect as a grayscale image, for tuning its threshold.
    public static func selectionPreview(_ kind: EffectKind, look: GlowLook, image: CIImage) -> CIImage {
        let kernels = GlowKernels.shared
        let extent = image.extent
        switch kind {
        case .bloom:
            let display = image.applyingFilter("CILinearToSRGBToneCurve")
            let t = bloomThresholds(look.bloom)
            return run(kernels.bloomSelect, extent, [display, t.t0, t.t1, 1.0]).applyingFilter("CISRGBToneCurveToLinear")
        case .glow:
            return run(kernels.glowSelect, extent, [image, look.glow.threshold / 100, 1.0, 1.0])
        case .halation:
            let h = halationThresholds(look.halation)
            return run(kernels.halationSelect, extent, [image, h.t0, h.t1, 1.0, 1.0, 1.0, 1.0])
        }
    }

    /// Scales a mask image to cover `extent`.
    public static func fitMask(_ mask: CIImage, to extent: CGRect) -> CIImage {
        let sx = extent.width / mask.extent.width
        let sy = extent.height / mask.extent.height
        return mask.clampedToExtent()
            .transformed(by: CGAffineTransform(scaleX: sx, y: sy))
            .cropped(to: extent)
    }

    /// Shows `effected` where the mask is white and `original` where it is black.
    public static func applyMask(_ mask: CIImage, effected: CIImage, original: CIImage) -> CIImage {
        run(GlowKernels.shared.maskMix, original.extent, [original, effected, mask])
    }

    public static func maskOverlay(_ image: CIImage, mask: CIImage, strength: Double = 0.55) -> CIImage {
        run(GlowKernels.shared.maskOverlay, image.extent, [image, mask, strength])
    }

    // MARK: - Steps

    static func bloom(_ s: BloomSettings, image: CIImage, longEdge: CGFloat) -> CIImage {
        let kernels = GlowKernels.shared
        let extent = image.extent
        let sigma = BlurSize.sigmaPixels(s.size, longEdge: longEdge)
        var linear = image
        if s.amount > 0 {
            let display = image.applyingFilter("CILinearToSRGBToneCurve")
            let t = bloomThresholds(s)
            let layer = run(kernels.bloomSelect, extent, [display, t.t0, t.t1, 0.0])
            let blurred = blur(layer, sigma: sigma, extent: extent)
            let blended = run(kernels.bloomComposite, extent, [display, blurred, s.amount / 100, blendCode(s.blend)])
            linear = blended.applyingFilter("CISRGBToneCurveToLinear")
        }
        if s.haze > 0 {
            // Energy-conserving scatter: a share of all light is spread wide, lifting the shadows.
            let veil = blur(linear, sigma: min(sigma * 5, longEdge * 0.08), extent: extent)
            linear = run(kernels.mix, extent, [linear, veil, s.haze / 100 * 0.35])
        }
        return linear
    }

    static func blur(_ image: CIImage, sigma: CGFloat, extent: CGRect) -> CIImage {
        guard sigma >= 0.3 else { return image }
        return image.clampedToExtent().applyingGaussianBlur(sigma: Double(sigma)).cropped(to: extent)
    }

    static func bloomThresholds(_ s: BloomSettings) -> (t0: Double, t1: Double) {
        let t0 = min(max(s.threshold / 100, 0), 1)
        return (t0, t0 + (1 - t0) * min(max(s.softness, 0), 100) / 100)
    }

    /// Halation is masked on linear light (as in NegPy); the slider is display brightness.
    static func halationThresholds(_ s: HalationSettings) -> (t0: Double, t1: Double) {
        (SRGB.decode(min(max(s.threshold / 100, 0), 1)), 1.0)
    }

    static func blendCode(_ blend: BloomBlend) -> Double {
        switch blend {
        case .lighten: return 0
        case .screen: return 1
        case .add: return 2
        }
    }

    static func run(_ kernel: CIColorKernel, _ extent: CGRect, _ arguments: [Any]) -> CIImage {
        guard let output = kernel.apply(extent: extent, arguments: arguments) else {
            assertionFailure("Glow: kernel \(kernel.name) rejected its arguments")
            return CIImage.empty().cropped(to: extent)
        }
        return output
    }
}
