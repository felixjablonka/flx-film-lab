// Vendored from Glow (~/Documents/Glow, Sources/GlowCore/Look.swift), unchanged
// apart from this note, so it can be compared and kept in sync with Glow.

import CoreGraphics
import Foundation

/// A complete set of effect settings.
///
/// Spatial values are relative to the image's long edge, so a look renders the same on a 24 MP and
/// a 45 MP file, and the preview matches the full-resolution export.
public struct GlowLook: Codable, Equatable, Sendable {
    public var bloom = BloomSettings()
    public var glow = GlowSettings()
    public var halation = HalationSettings()

    public init() {}

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        bloom = c.value(.bloom, default: BloomSettings())
        glow = c.value(.glow, default: GlowSettings())
        halation = c.value(.halation, default: HalationSettings())
    }
}

public enum BloomBlend: String, Codable, CaseIterable, Sendable {
    case lighten, screen, add

    public var title: String {
        switch self {
        case .lighten: return "Lighten"
        case .screen: return "Screen"
        case .add: return "Add"
        }
    }
}

/// The Photoshop "Pro Mist" recipe: select the highlights, blur them, blend them back with Lighten.
/// Works on display-encoded values, like Photoshop does.
public struct BloomSettings: Codable, Equatable, Sendable {
    public var enabled = true
    /// Layer opacity in percent, 0...200. Above 100 behaves like stacking duplicated layers.
    public var amount: Double = 100
    /// Blur size slider, 0...100 (see `BlurSize`).
    public var size: Double = 45
    /// Display brightness, 0...100, where the highlight selection starts.
    public var threshold: Double = 80
    /// Share of the range above the threshold over which the selection fades in, 0...100.
    public var softness: Double = 100
    public var blend: BloomBlend = .lighten
    /// Wide veil that lifts shadows like a physical mist filter, 0...100.
    public var haze: Double = 0

    public init() {}

    public var isActive: Bool { enabled && (amount > 0 || haze > 0) }

    public init(from decoder: Decoder) throws {
        let d = BloomSettings()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = c.value(.enabled, default: d.enabled)
        amount = c.value(.amount, default: d.amount)
        size = c.value(.size, default: d.size)
        threshold = c.value(.threshold, default: d.threshold)
        softness = c.value(.softness, default: d.softness)
        blend = c.value(.blend, default: d.blend)
        haze = c.value(.haze, default: d.haze)
    }
}

/// NegPy-style lens glow: highlights blurred and added back in linear light, on all channels.
public struct GlowSettings: Codable, Equatable, Sendable {
    public var enabled = false
    /// Strength, 0...100.
    public var amount: Double = 30
    public var size: Double = 40
    /// Display brightness, 0...100, where the glow starts.
    public var threshold: Double = 50

    public init() {}

    public var isActive: Bool { enabled && amount > 0 }

    public init(from decoder: Decoder) throws {
        let d = GlowSettings()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = c.value(.enabled, default: d.enabled)
        amount = c.value(.amount, default: d.amount)
        size = c.value(.size, default: d.size)
        threshold = c.value(.threshold, default: d.threshold)
    }
}

/// NegPy-style halation: red-dominant scatter of light bouncing back off the film base.
public struct HalationSettings: Codable, Equatable, Sendable {
    public var enabled = false
    /// Strength, 0...100.
    public var amount: Double = 40
    public var size: Double = 52
    /// Display brightness, 0...100. The default 83 equals NegPy's 0.65 linear threshold.
    public var threshold: Double = 83
    /// Linear-light weights of the scattered light; NegPy uses (1, 0.3, 0.05).
    public var tint = LinearRGB(r: 1.0, g: 0.3, b: 0.05)
    /// Keep halation off the highlight itself so it only shows as a ring around it.
    public var haloOnly = false

    public init() {}

    public var isActive: Bool { enabled && amount > 0 }

    public init(from decoder: Decoder) throws {
        let d = HalationSettings()
        let c = try decoder.container(keyedBy: CodingKeys.self)
        enabled = c.value(.enabled, default: d.enabled)
        amount = c.value(.amount, default: d.amount)
        size = c.value(.size, default: d.size)
        threshold = c.value(.threshold, default: d.threshold)
        tint = c.value(.tint, default: d.tint)
        haloOnly = c.value(.haloOnly, default: d.haloOnly)
    }
}

public struct LinearRGB: Codable, Equatable, Sendable {
    public var r: Double
    public var g: Double
    public var b: Double

    public init(r: Double, g: Double, b: Double) {
        self.r = r
        self.g = g
        self.b = b
    }
}

/// Maps the 0...100 size sliders to a Gaussian sigma relative to the image.
public enum BlurSize {
    /// Sigma as a fraction of the long edge: 0.1 % at 0, 5 % at 100, on a log scale.
    /// The default bloom size 45 gives 0.58 %, which is 35 px on a 6048 px wide file.
    public static func sigmaFraction(_ slider: Double) -> Double {
        0.001 * pow(50, min(max(slider, 0), 100) / 100)
    }

    public static func sigmaPixels(_ slider: Double, longEdge: CGFloat) -> CGFloat {
        CGFloat(sigmaFraction(slider)) * longEdge
    }
}

public enum SRGB {
    public static func decode(_ v: Double) -> Double {
        v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)
    }

    public static func encode(_ v: Double) -> Double {
        v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
    }
}

extension KeyedDecodingContainer {
    /// Decodes a value if present and valid, so settings saved by an older version still load.
    func value<T: Decodable>(_ key: Key, default fallback: T) -> T {
        ((try? decodeIfPresent(T.self, forKey: key)) ?? nil) ?? fallback
    }
}
