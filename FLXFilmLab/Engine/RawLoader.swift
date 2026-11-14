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
        case daylightD55 = "Daylight D55"
        /// Stored as "Daylight", as presets saved before D55 joined it name it.
        case daylight = "Daylight"
        /// A 3200 K lamp, the light tungsten film is balanced for.
        case tungsten3200 = "Tungsten 3200 K"
        /// A household bulb, 2850 K. Stored as "Tungsten", as presets saved
        /// before 3200 K joined it name it.
        case tungsten = "Tungsten"
        case custom = "Custom"
        var id: String { rawValue }

        /// What the menu shows.
        var label: String {
            switch self {
            case .daylight: return "Daylight D65"
            case .tungsten: return "Tungsten 2850 K"
            default: return rawValue
            }
        }
    }

    /// Correlated colour temperature of the illuminants the profiles name.
    static func referenceTemperature(_ illuminant: String) -> Double {
        switch illuminant {
        case "BB3200": return 3200     // a 3200 K lamp, the tungsten stocks' reference
        case "T": return 2856          // agx's household bulb, read as CIE A
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
        // The Planckian 3200 K, where agx and the CIE put it. Core Image's own
        // "3200 K" at tint 0 sits 14 K off it, and reads this as 3186 K, tint -0.4.
        case "BB3200": return CGPoint(x: 0.42336, y: 0.39895)
        default: return nil            // CIE A and the printing lamps: Core Image's own temperatures
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
        /// As the file's EXIF says: mm, seconds, ISO.
        var focalLength: Double? = nil
        var exposureTime: Double? = nil
        var iso: Double? = nil
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

        let properties = metadata(of: url)
        let tiff = properties?[kCGImagePropertyTIFFDictionary] as? [CFString: Any]
        let exif = properties?[kCGImagePropertyExifDictionary] as? [CFString: Any]

        return Decoded(pixels: pixels, width: width, height: height, isRaw: isRaw,
                       cameraMake: tiff?[kCGImagePropertyTIFFMake] as? String,
                       cameraModel: tiff?[kCGImagePropertyTIFFModel] as? String,
                       focalLength: (exif?[kCGImagePropertyExifFocalLength] as? NSNumber)?.doubleValue,
                       exposureTime: (exif?[kCGImagePropertyExifExposureTime] as? NSNumber)?.doubleValue,
                       iso: ((exif?[kCGImagePropertyExifISOSpeedRatings] as? [NSNumber])?.first)?.doubleValue)
    }

    // MARK: - The original

    private static let displayContext = CIContext(options: [.cacheIntermediates: false])
    private static let displaySpace = CGColorSpace(name: CGColorSpace.displayP3) ?? CGColorSpace(name: CGColorSpace.sRGB)!

    /// The picture before film and paper (Original, \): the RAW as macOS
    /// renders it by default, as Preview and Photos show it, as shot, its long
    /// side `longEdge`. A picture already rendered is itself. Its lens is
    /// corrected, as the film's decode corrects it, so the two frames match.
    /// About 0.07-0.4 s at 2560 px on an M4 Pro.
    static func original(url: URL, longEdge: Int) -> CGImage? {
        var image: CIImage
        if isRawFile(url), let filter = CIRAWFilter(imageURL: url) {
            filter.isLensCorrectionEnabled = true
            let native = max(filter.nativeSize.width, filter.nativeSize.height)
            if native > 0 { filter.scaleFactor = Float(min(1, CGFloat(longEdge) / native)) }
            guard let output = filter.outputImage else { return nil }
            image = output
        } else if let loaded = CIImage(contentsOf: url, options: [.applyOrientationProperty: true]) {
            image = loaded
        } else {
            return nil
        }
        let long = max(image.extent.width, image.extent.height)
        if long > CGFloat(longEdge) {
            let scale = CGFloat(longEdge) / long
            image = image.applyingFilter("CILanczosScaleTransform",
                                         parameters: [kCIInputScaleKey: scale, kCIInputAspectRatioKey: 1])
        }
        let extent = image.extent.integral
        return displayContext.createCGImage(image, from: extent, format: .RGBA8, colorSpace: displaySpace)
    }

    /// `original` in the print's place: scaled to the decode the print was
    /// made from, then turned, mirrored, levelled and cropped as the print is
    /// (`map`), so a toggle between the two moves nothing.
    static func placed(_ original: CGImage, on map: GeometryMap) -> CGImage? {
        let source = CIImage(cgImage: original)
        let w = map.sourceSize.width, h = map.sourceSize.height
        let fit = CGAffineTransform(scaleX: w / source.extent.width, y: h / source.extent.height)
        // the map's pixels run down from the top, Core Image's up from the bottom
        let downSource = CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: 0, ty: h)
        let upOutput = CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: 0, ty: CGFloat(map.outputHeight))
        let transform = fit.concatenating(downSource).concatenating(map.sourceToOutput).concatenating(upOutput)
        let out = CGRect(x: 0, y: 0, width: map.outputWidth, height: map.outputHeight)
        let placed = source.clampedToExtent().transformed(by: transform).cropped(to: out)
        // where levelling leaves the corners of the whole frame empty, they are empty
        let frame = CIImage(color: .white).cropped(to: source.extent).transformed(by: transform)
        let masked = placed.applyingFilter("CIBlendWithMask", parameters: [
            kCIInputBackgroundImageKey: CIImage(color: .clear).cropped(to: out),
            kCIInputMaskImageKey: frame.cropped(to: out),
        ])
        return displayContext.createCGImage(map.isIdentity ? placed : masked, from: out, format: .RGBA8,
                                            colorSpace: original.colorSpace ?? displaySpace)
    }

    /// What the file says about the photograph, as ImageIO reads it: EXIF,
    /// TIFF, GPS and the rest. The export carries it over (`Exporter.write`).
    static func metadata(of url: URL) -> [CFString: Any]? {
        CGImageSourceCreateWithURL(url as CFURL, nil)
            .flatMap { CGImageSourceCopyPropertiesAtIndex($0, 0, nil) as? [CFString: Any] }
    }

    // MARK: - Core Image's RAW decoder version

    /// The version of Core Image's RAW decoder the film decode stays on.
    /// macOS 27 brings a version 9 for some cameras (so far the Ricoh GR IIIx's
    /// and Leica M's DNGs and the Fujifilm X-T3's RAFs), but Core Image still
    /// picks 8 for them. Set up as the film decode is, 9 decoded those files
    /// cyan: red at a quarter of 8's or below zero, a median ΔE of 17-22 from
    /// 8. At Apple's own defaults the two look alike. The Windows app's colour
    /// is measured from 8's decode as well. So where Core Image's default is
    /// newer, the decode asks for 8 if the file offers it; a camera that only
    /// a newer version reads keeps that. Moving on is a step taken on purpose:
    /// the checks fail the day Core Image's default for a sample raw passes 8.
    /// The Original (`original`) stays on Core Image's default: it shows what
    /// macOS shows.
    static let filmDecoderVersion = 8

    /// A decoder version's number: "8" and "8.dng" are both 8.
    static func number(of version: CIRAWDecoderVersion) -> Int? {
        Int(version.rawValue.prefix(while: \.isNumber))
    }

    /// Keeps `filter` on `filmDecoderVersion` where Core Image's default is newer.
    static func keepDecoderVersion(_ filter: CIRAWFilter) {
        guard let current = number(of: filter.decoderVersion), current > filmDecoderVersion,
              let kept = filter.supportedDecoderVersions.first(where: { number(of: $0) == filmDecoderVersion })
        else { return }
        filter.decoderVersion = kept
    }

    static func configure(_ filter: CIRAWFilter, options: Options) {
        // The version first: choosing one puts its shadow boost and shadow bias
        // back to their defaults (0.9 and 5.0 on a Ricoh or Leica DNG), so the
        // settings below come after it.
        keepDecoderVersion(filter)
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
        case .daylightD55:
            filter.neutralChromaticity = referenceChromaticity("D55")!
        case .daylight:
            filter.neutralChromaticity = referenceChromaticity("D65")!
        case .tungsten3200:
            filter.neutralChromaticity = referenceChromaticity("BB3200")!
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

/// The photograph in one line, as the status under the print shows it: the
/// camera, its focal length, the shutter speed and the ISO, "Nikon Z f  ·
/// 35 mm  ·  1/250 s  ·  ISO 400", leaving out what the file does not say.
/// The Windows app's `photoLine` writes the same.
enum PhotoLine {
    static func text(camera: String, focalLength: Double?, shutter: Double?, iso: Double?) -> String {
        var parts: [String] = []
        if !camera.isEmpty { parts.append(camera) }
        if let f = focalLength, f > 0 {
            parts.append(f < 10 ? String(format: "%.1f mm", f) : "\(Int(f.rounded())) mm")
        }
        if let t = shutter, t > 0 { parts.append(shutterSpeed(t)) }
        if let iso, iso > 0 { parts.append("ISO \(Int(iso.rounded()))") }
        return parts.joined(separator: "  ·  ")
    }

    /// As cameras show it: 1/250 s, 1/2 s, 0.6 s, 2 s, 1.3 s.
    static func shutterSpeed(_ t: Double) -> String {
        if t < 1 {
            let under = (1 / t).rounded()
            if under >= 1, abs(1 / under - t) / t < 0.05 { return "1/\(Int(under)) s" }
            return String(format: "%.1f s", t)
        }
        if abs(t - t.rounded()) < 0.05 { return "\(Int(t.rounded())) s" }
        return String(format: "%.1f s", t)
    }

    /// The camera as people name it: its maker's name as a brand, not the
    /// company's (NIKON CORPORATION is Nikon), and the model without the
    /// maker's name again in front (NIKON Z 7 is Z 7).
    static func camera(make: String?, model: String?) -> String {
        let brands = [("NIKON", "Nikon"), ("CANON", "Canon"), ("SONY", "Sony"), ("FUJIFILM", "Fujifilm"),
                      ("RICOH", "Ricoh"), ("PENTAX", "Pentax"), ("LEICA", "Leica"), ("OLYMPUS", "Olympus"),
                      ("OM DIGITAL", "OM System"), ("PANASONIC", "Panasonic"), ("HASSELBLAD", "Hasselblad"),
                      ("SIGMA", "Sigma"), ("APPLE", "Apple"), ("GOOGLE", "Google"), ("SAMSUNG", "Samsung"),
                      ("PHASE ONE", "Phase One"), ("DJI", "DJI")]
        let maker = (make ?? "").trimmingCharacters(in: .whitespaces)
        var name = (model ?? "").trimmingCharacters(in: .whitespaces)
        let brand = brands.first { maker.uppercased().hasPrefix($0.0) }?.1 ?? maker
        for (key, _) in brands where name.uppercased().hasPrefix(key + " ") {
            name = String(name.dropFirst(key.count + 1))
            break
        }
        return [brand, name].filter { !$0.isEmpty }.joined(separator: " ")
    }
}
