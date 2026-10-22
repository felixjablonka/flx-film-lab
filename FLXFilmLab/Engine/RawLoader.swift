import Foundation
import CoreImage
import CoreGraphics
import UniformTypeIdentifiers

/// RAW decode via Core Image.
///
/// macOS's own RAW pipeline reads Nikon NEF and Leica DNG (and the rest of
/// Adobe's DNG variants) natively and on the GPU, so the app ships no LibRaw.
/// The decode is configured scene-referred and linear — no tone curve, no
/// highlight boost, no gamut mapping — because everything after this point is
/// the film's job.
enum RawLoader {

    enum WhiteBalance: String, Codable, CaseIterable, Identifiable {
        /// Balance the decode for the illuminant the *film* is balanced for.
        ///
        /// This is the default, and it is what makes the simulation mean
        /// anything under mixed light. A daylight stock shot under tungsten
        /// really does go orange — that is the film reacting to light it was
        /// not designed for. Decoding "as shot" lets the camera neutralise the
        /// illuminant before the film ever sees it, which quietly throws that
        /// away and makes every stock behave as if the light were perfect.
        case filmReference = "Film reference"
        case asShot = "As shot"
        case daylight = "Daylight"
        case tungsten = "Tungsten"
        case custom = "Custom"
        var id: String { rawValue }
    }

    /// Correlated colour temperature of the illuminants the profiles name.
    static func referenceTemperature(_ illuminant: String) -> Double {
        switch illuminant {
        case "T": return 2856          // CIE A, the tungsten stocks' reference
        case "TH-KG3", "TH-KG3-L": return 3400
        case "D50": return 5003
        case "D65": return 6504
        case "K75P": return 5400
        default: return 5503           // D55, what most daylight stocks are balanced for
        }
    }

    /// CIE 1931 chromaticity of the illuminants that are not blackbodies.
    ///
    /// Core Image reads a temperature with tint 0 as a blackbody — a lamp of
    /// that colour — and the CIE daylights sit well off that line, at tint +10
    /// on its scale. Balancing a daylight stock's decode for "5503 K" therefore
    /// neutralised a pinker light than D55, and every daylight frame decoded
    /// green-yellow: a* -1.9, b* +1.5 on a mid-grey, which a slide film then
    /// steepened to a* -2.0, b* +1.4 through Provia and more through Velvia.
    /// The chromaticity is the illuminant itself, the same for every camera.
    static func referenceChromaticity(_ illuminant: String) -> CGPoint? {
        switch illuminant {
        case "D50": return CGPoint(x: 0.34567, y: 0.35850)
        case "D55": return CGPoint(x: 0.33242, y: 0.34743)
        case "D65": return CGPoint(x: 0.31271, y: 0.32902)
        default: return nil            // CIE A and the tungsten lamps are blackbodies
        }
    }

    struct Options {
        var whiteBalance: WhiteBalance = .filmReference
        /// The illuminant the stock is balanced for, used by `.filmReference`.
        var filmReference: String = "D55"
        var temperature: Double = 5500
        var tint: Double = 0
        /// Longest edge of the decoded image. The interactive path renders a
        /// proxy; export decodes at native size.
        var maxDimension: Int?
    }

    struct Decoded {
        var pixels: [Float]      // RGBA32F, row-major
        var width: Int
        var height: Int
        var isRaw: Bool
        var cameraMake: String?
        var cameraModel: String?
    }

    enum LoadError: Error, LocalizedError {
        case unreadable(URL)
        case renderFailed
        var errorDescription: String? {
            switch self {
            case .unreadable(let url): return "Could not read \(url.lastPathComponent)."
            case .renderFailed: return "Could not render the decoded image."
            }
        }
    }

    /// The engine's working space: linear Rec.2020, matching the
    /// `WORKING_COLOR_SPACE` the packs and the parity references were baked in.
    static let workingColorSpace: CGColorSpace =
        CGColorSpace(name: CGColorSpace.extendedLinearITUR_2020)
        ?? CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!

    private static let context = CIContext(options: [
        .workingColorSpace: workingColorSpace,
        .workingFormat: CIFormat.RGBAf,
        .cacheIntermediates: false,
    ])

    static func isRawFile(_ url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension.lowercased()) else {
            // Leica .RWL and some Nikon variants may not resolve to a UTType
            return ["nef", "nrw", "dng", "rwl", "raw", "arw", "cr2", "cr3", "raf", "orf", "rw2"]
                .contains(url.pathExtension.lowercased())
        }
        return type.conforms(to: .rawImage) || url.pathExtension.lowercased() == "dng"
    }

    static func load(url: URL, options: Options) throws -> Decoded {
        let image: CIImage
        let isRaw = isRawFile(url)

        if isRaw, let filter = CIRAWFilter(imageURL: url) {
            configure(filter, options: options)
            guard let output = filter.outputImage else { throw LoadError.unreadable(url) }
            image = output
        } else if let loaded = CIImage(contentsOf: url, options: [.applyOrientationProperty: true]) {
            image = loaded
        } else {
            throw LoadError.unreadable(url)
        }

        let scaled = downscale(image, maxDimension: options.maxDimension)
        let extent = scaled.extent.integral
        let width = Int(extent.width), height = Int(extent.height)
        guard width > 0, height > 0 else { throw LoadError.renderFailed }

        var pixels = [Float](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { raw in
            context.render(scaled,
                           toBitmap: raw.baseAddress!,
                           rowBytes: width * 16,
                           bounds: extent,
                           format: .RGBAf,
                           colorSpace: workingColorSpace)
        }

        let tiff = metadata(of: url)?[kCGImagePropertyTIFFDictionary] as? [CFString: Any]

        return Decoded(pixels: pixels, width: width, height: height, isRaw: isRaw,
                       cameraMake: tiff?[kCGImagePropertyTIFFMake] as? String,
                       cameraModel: tiff?[kCGImagePropertyTIFFModel] as? String)
    }

    /// What the file says about the photograph, as ImageIO reads it: EXIF,
    /// TIFF, GPS and the rest. The export carries it over (`Exporter.write`).
    static func metadata(of url: URL) -> [CFString: Any]? {
        CGImageSourceCreateWithURL(url as CFURL, nil)
            .flatMap { CGImageSourceCopyPropertiesAtIndex($0, 0, nil) as? [CFString: Any] }
    }

    private static func configure(_ filter: CIRAWFilter, options: Options) {
        // Scene-referred and linear: the film model wants the light the sensor
        // actually recorded, not a rendered picture.
        filter.boostAmount = 0                 // no default tone curve
        filter.boostShadowAmount = 0
        // Apple subtracts a camera-specific "shadow bias" from the linear data
        // to hide noise: 3.0 on a Nikon Z 7 or Z f at any ISO, 5.0 on a Leica M
        // (Typ 240). That pushed the darkest light the sensor recorded below
        // zero, which exposes the film to nothing, so no Exposure could bring
        // it back: the left side of DSC_0871 (+18 DN over black on the sensor)
        // decoded at a median of -0.003. With none, the decode is the sensor's
        // own black-subtracted signal, as Capture One reads it.
        filter.shadowBias = 0
        // No luminance noise reduction: the film's own grain is the texture,
        // and Apple's smoothing (0.36-0.40 on a Z f) would sit under it.
        // Colour noise reduction and sharpening stay at Apple's defaults for
        // the camera: the first costs no detail, the second adds a third.
        filter.luminanceNoiseReductionAmount = 0
        filter.isGamutMappingEnabled = false
        filter.isLensCorrectionEnabled = true
        filter.localToneMapAmount = 0
        filter.extendedDynamicRangeAmount = 2  // keep highlights above 1.0

        switch options.whiteBalance {
        case .filmReference:
            if let xy = referenceChromaticity(options.filmReference) {
                filter.neutralChromaticity = xy
            } else {
                filter.neutralTemperature = Float(referenceTemperature(options.filmReference))
                filter.neutralTint = 0
            }
        case .asShot:
            break                               // leave the camera's own neutral point
        case .daylight:
            filter.neutralChromaticity = referenceChromaticity("D65")!
        case .tungsten:
            filter.neutralTemperature = 2850
            filter.neutralTint = 0
        case .custom:
            filter.neutralTemperature = Float(options.temperature)
            filter.neutralTint = Float(options.tint)
        }
    }

    private static func downscale(_ image: CIImage, maxDimension: Int?) -> CIImage {
        guard let maxDimension, maxDimension > 0 else { return image }
        let longEdge = max(image.extent.width, image.extent.height)
        guard longEdge > CGFloat(maxDimension) else { return image }
        let scale = CGFloat(maxDimension) / longEdge
        let filter = CIFilter(name: "CILanczosScaleTransform")!
        filter.setValue(image, forKey: kCIInputImageKey)
        filter.setValue(scale, forKey: kCIInputScaleKey)
        filter.setValue(1.0, forKey: kCIInputAspectRatioKey)
        return (filter.outputImage ?? image).clampedToExtent()
            .cropped(to: CGRect(x: 0, y: 0,
                                width: (image.extent.width * scale).rounded(.down),
                                height: (image.extent.height * scale).rounded(.down)))
    }
}
