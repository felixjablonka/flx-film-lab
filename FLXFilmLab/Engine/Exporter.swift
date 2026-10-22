import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

/// Writes the rendered print out, with the output profile embedded so the file
/// means the same thing everywhere it lands.
enum Exporter {

    enum Format: String, CaseIterable, Identifiable {
        case tiff16 = "TIFF (16-bit)"
        case jpeg = "JPEG"
        case png = "PNG"
        var id: String { rawValue }

        var fileExtension: String {
            switch self {
            case .tiff16: return "tif"
            case .jpeg: return "jpg"
            case .png: return "png"
            }
        }

        var utType: UTType {
            switch self {
            case .tiff16: return .tiff
            case .jpeg: return .jpeg
            case .png: return .png
            }
        }

        var isSixteenBit: Bool { self == .tiff16 }
    }

    enum ExportError: Error, LocalizedError {
        case renderFailed
        case destinationFailed
        var errorDescription: String? {
            switch self {
            case .renderFailed: return "The full-resolution render did not complete."
            case .destinationFailed: return "Could not create the output file."
            }
        }
    }

    /// `linear` is RGBA float in the output colour space, straight off the GPU.
    /// `metadata` is the source file's (`RawLoader.metadata(of:)`); what in it
    /// describes the photograph is written into the file too.
    static func write(linear: [Float], width: Int, height: Int, to url: URL,
                      format: Format, colorSpace: String,
                      metadata: [CFString: Any]? = nil) throws {
        let cs = Renderer.cgColorSpace(named: colorSpace)
        let encode = transferFunction(for: colorSpace)

        let image: CGImage
        if format.isSixteenBit {
            var pixels = [UInt16](repeating: 0, count: width * height * 3)
            for i in 0..<(width * height) {
                for c in 0..<3 {
                    let v = encode(max(linear[i * 4 + c], 0))
                    pixels[i * 3 + c] = UInt16(min(max(v, 0), 1) * 65535 + 0.5)
                }
            }
            image = try makeImage(pixels: &pixels, width: width, height: height,
                                  bitsPerComponent: 16, colorSpace: cs)
        } else {
            var pixels = [UInt8](repeating: 0, count: width * height * 3)
            for i in 0..<(width * height) {
                for c in 0..<3 {
                    let v = encode(max(linear[i * 4 + c], 0))
                    pixels[i * 3 + c] = UInt8(min(max(v, 0), 1) * 255 + 0.5)
                }
            }
            image = try makeImage(pixels: &pixels, width: width, height: height,
                                  bitsPerComponent: 8, colorSpace: cs)
        }

        guard let destination = CGImageDestinationCreateWithURL(
            url as CFURL, format.utType.identifier as CFString, 1, nil) else {
            throw ExportError.destinationFailed
        }
        var properties = exportProperties(source: metadata, width: width, height: height,
                                          colorSpace: colorSpace)
        if format == .jpeg { properties[kCGImageDestinationLossyCompressionQuality] = 0.95 }
        CGImageDestinationAddImage(destination, image, properties as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { throw ExportError.destinationFailed }
    }

    /// The source's description of the photograph, for the exported file:
    /// when it was taken, with which camera and lens, at what settings, where.
    ///
    /// EXIF, its auxiliary lens data, GPS and IPTC come along whole, and from
    /// TIFF the camera, the time and the credits. Works the same for any file
    /// ImageIO reads: NEF, DNG and the other raw formats, TIFF, JPEG. What
    /// described the raw file itself — its compression and pixel layout, the
    /// maker's private notes — does not describe the print and stays behind.
    /// The pixels were turned upright on decode, so the orientation is reset;
    /// the size and colour space are the export's own.
    static func exportProperties(source: [CFString: Any]?, width: Int, height: Int,
                                 colorSpace: String) -> [CFString: Any] {
        var exif = source?[kCGImagePropertyExifDictionary] as? [CFString: Any] ?? [:]
        for key in [kCGImagePropertyExifMakerNote, kCGImagePropertyExifCFAPattern,
                    kCGImagePropertyExifComponentsConfiguration,
                    kCGImagePropertyExifCompressedBitsPerPixel] {
            exif[key] = nil
        }
        exif[kCGImagePropertyExifPixelXDimension] = width
        exif[kCGImagePropertyExifPixelYDimension] = height
        // EXIF can name one colour space, sRGB. For any other the tag is left
        // out (ImageIO will not write "uncalibrated" anyway), and the embedded
        // profile says which it is.
        exif[kCGImagePropertyExifColorSpace] = colorSpace == "sRGB" ? 1 : nil
        // A Nikon body names a lens without a CPU (manual or adapted) only in
        // its maker notes, which ImageIO reads into the auxiliary dictionary.
        // The standard tag is where Photos and Lightroom look for it.
        let aux = source?[kCGImagePropertyExifAuxDictionary] as? [CFString: Any]
        if exif[kCGImagePropertyExifLensModel] == nil,
           let lens = aux?[kCGImagePropertyExifAuxLensModel] as? String {
            exif[kCGImagePropertyExifLensModel] = lens
        }

        var tiff: [CFString: Any] = [kCGImagePropertyTIFFOrientation: 1,
                                     kCGImagePropertyTIFFSoftware: "FLX Film Lab"]
        let sourceTIFF = source?[kCGImagePropertyTIFFDictionary] as? [CFString: Any] ?? [:]
        for key in [kCGImagePropertyTIFFMake, kCGImagePropertyTIFFModel, kCGImagePropertyTIFFDateTime,
                    kCGImagePropertyTIFFArtist, kCGImagePropertyTIFFCopyright,
                    kCGImagePropertyTIFFImageDescription] {
            if let value = sourceTIFF[key] { tiff[key] = value }
        }

        var properties: [CFString: Any] = [kCGImagePropertyOrientation: 1,
                                           kCGImagePropertyExifDictionary: exif,
                                           kCGImagePropertyTIFFDictionary: tiff]
        for key in [kCGImagePropertyExifAuxDictionary, kCGImagePropertyIPTCDictionary] {
            if let value = source?[key] { properties[key] = value }
        }
        if let gps = source?[kCGImagePropertyGPSDictionary] as? [CFString: Any], hasPosition(gps) {
            properties[kCGImagePropertyGPSDictionary] = gps
        }
        return properties
    }

    /// Whether a GPS block records where the picture was taken. A Leica M
    /// (Typ 240) without a GPS fix still writes one: status V (void), latitude
    /// and longitude 0 — which a photo library would place off West Africa.
    static func hasPosition(_ gps: [CFString: Any]) -> Bool {
        guard let latitude = (gps[kCGImagePropertyGPSLatitude] as? NSNumber)?.doubleValue,
              let longitude = (gps[kCGImagePropertyGPSLongitude] as? NSNumber)?.doubleValue else {
            return false
        }
        if (gps[kCGImagePropertyGPSStatus] as? String) == "V" { return false }
        return latitude != 0 || longitude != 0
    }

    private static func makeImage<T>(pixels: inout [T], width: Int, height: Int,
                                     bitsPerComponent: Int, colorSpace: CGColorSpace) throws -> CGImage {
        let bytesPerComponent = bitsPerComponent / 8
        let bytesPerRow = width * 3 * bytesPerComponent
        let data = pixels.withUnsafeBytes { Data($0) }
        guard let provider = CGDataProvider(data: data as CFData) else { throw ExportError.renderFailed }
        var info = CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue)
        // The samples are written in host order, which is little-endian on every
        // Mac this runs on. Declaring big-endian here byte-swapped every 16-bit
        // sample, which is what turned 16-bit TIFF exports into noise.
        if bitsPerComponent == 16 { info.insert(.byteOrder16Little) }
        guard let image = CGImage(width: width, height: height,
                                  bitsPerComponent: bitsPerComponent,
                                  bitsPerPixel: bitsPerComponent * 3,
                                  bytesPerRow: bytesPerRow, space: colorSpace,
                                  bitmapInfo: info, provider: provider, decode: nil,
                                  shouldInterpolate: false, intent: .defaultIntent) else {
            throw ExportError.renderFailed
        }
        return image
    }

    /// The output space's OETF. sRGB and Display P3 share the sRGB piecewise
    /// curve; Rec.2020 files are written with the same one and tagged with the
    /// sRGB-gamma variant of the Rec.2020 profile, so they round-trip through
    /// the CGColorSpace that carries them.
    private static func transferFunction(for colorSpace: String) -> (Float) -> Float {
        if colorSpace == "ProPhoto RGB" {
            return { pow($0, 1 / 1.80078125) }
        }
        return { v in
            v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055
        }
    }
}
