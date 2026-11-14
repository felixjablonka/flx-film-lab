// flxmeasure — Apple's RAW decoder rendering one raw file at the white
// balances Apple's colour is measured at (tools/calibrate/calibrate.py), as
// the Mac app's RawLoader sets it up but with everything that is not colour
// switched off: sharpening, colour noise reduction, moiré reduction and lens
// correction (whose vignetting would vary the gain across the frame), and
// the picture left as the sensor has it, unrotated.
//
//   flxmeasure <raw> <outdir> [--size PX] [--probe K:TINT ...]
//
// Writes into <outdir>:
//   as-shot.f32            the camera's own white balance
//   <name>.f32             each white balance of the list below, scaled to PX on the long side (1200)
//   probe-K-TINT.f32       each --probe, a temperature and tint off the grid
//   align.f32              D55 at half size, the middle 2048 x 2048 at most, to find Apple's crop with
//   apple.json             what CIRAWFilter says about the file, and what each dump is

import CoreImage
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
guard args.count >= 2 else { fail("usage: flxmeasure <raw> <outdir> [--size PX]") }
let input = URL(fileURLWithPath: args.removeFirst())
let outdir = URL(fileURLWithPath: args.removeFirst(), isDirectory: true)
let size = args.firstIndex(of: "--size").flatMap { $0 + 1 < args.count ? Int(args[$0 + 1]) : nil } ?? 1200

let workingSpace = CGColorSpace(name: CGColorSpace.extendedLinearITUR_2020)!
let context = CIContext(options: [.workingColorSpace: workingSpace, .workingFormat: CIFormat.RGBAf,
                                  .cacheIntermediates: false])

guard let filter = CIRAWFilter(imageURL: input) else { fail("CIRAWFilter cannot read \(input.path)") }
// RawLoader.keepDecoderVersion: Core Image's RAW decoder 8 where its default is
// newer and the file offers 8 (RawLoader.filmDecoderVersion; macOS 27's version
// 9 decodes the film decode's setup cyan). First, as there: choosing a version
// puts its shadow boost and shadow bias back to their defaults.
func decoderNumber(_ version: CIRAWDecoderVersion) -> Int? { Int(version.rawValue.prefix(while: \.isNumber)) }
if let current = decoderNumber(filter.decoderVersion), current > 8,
   let kept = filter.supportedDecoderVersions.first(where: { decoderNumber($0) == 8 }) {
    filter.decoderVersion = kept
}
try? FileManager.default.createDirectory(at: outdir, withIntermediateDirectories: true)
let exifOrientation = filter.orientation.rawValue
let defaults: [String: Any] = [
    "sharpnessAmount": filter.sharpnessAmount, "colorNoiseReductionAmount": filter.colorNoiseReductionAmount,
    "luminanceNoiseReductionAmount": filter.luminanceNoiseReductionAmount,
    "moireReductionAmount": filter.moireReductionAmount, "detailAmount": filter.detailAmount,
    "lensCorrectionSupported": filter.isLensCorrectionSupported,
]
let asShot: [String: Any] = ["temperature": filter.neutralTemperature, "tint": filter.neutralTint,
                             "x": filter.neutralChromaticity.x, "y": filter.neutralChromaticity.y]

// RawLoader.configure, then everything but colour off
filter.boostAmount = 0
filter.boostShadowAmount = 0
filter.shadowBias = 0
filter.luminanceNoiseReductionAmount = 0
filter.isGamutMappingEnabled = false
filter.localToneMapAmount = 0
filter.extendedDynamicRangeAmount = 2
filter.sharpnessAmount = 0
filter.colorNoiseReductionAmount = 0
filter.moireReductionAmount = 0
filter.isLensCorrectionEnabled = false
filter.orientation = .up

func write(_ image: CIImage, bounds: CGRect, to name: String) -> [String: Any] {
    let extent = bounds.integral
    let width = Int(extent.width), height = Int(extent.height)
    var pixels = [Float](repeating: 0, count: width * height * 4)
    pixels.withUnsafeMutableBytes {
        context.render(image, toBitmap: $0.baseAddress!, rowBytes: width * 16, bounds: extent, format: .RGBAf,
                       colorSpace: workingSpace)
    }
    var header = Data("FLXD".utf8)
    for v in [UInt32(1), UInt32(width), UInt32(height), 4, 0, 0, 0] {
        withUnsafeBytes(of: v.littleEndian) { header.append(contentsOf: $0) }
    }
    do {
        try (header + pixels.withUnsafeBytes { Data($0) }).write(to: outdir.appendingPathComponent(name + ".f32"))
    } catch {
        fail("cannot write \(name).f32: \(error)")
    }
    return ["x": Int(extent.minX), "y": Int(extent.minY), "width": width, "height": height]
}

// RawLoader.downscale
func scaled(_ image: CIImage, by scale: CGFloat) -> CIImage {
    let lanczos = CIFilter(name: "CILanczosScaleTransform")!
    lanczos.setValue(image, forKey: kCIInputImageKey)
    lanczos.setValue(scale, forKey: kCIInputScaleKey)
    lanczos.setValue(1.0, forKey: kCIInputAspectRatioKey)
    return (lanczos.outputImage ?? image).clampedToExtent()
        .cropped(to: CGRect(x: 0, y: 0, width: (image.extent.width * scale).rounded(.down),
                            height: (image.extent.height * scale).rounded(.down)))
}

func render(_ name: String) -> [String: Any] {
    guard let image = filter.outputImage else { fail("no image from \(input.path)") }
    let scale = min(1, CGFloat(size) / max(image.extent.width, image.extent.height))
    let small = scale < 1 ? scaled(image, by: scale) : image
    var entry = write(small, bounds: small.extent, to: name)
    entry["temperature"] = filter.neutralTemperature
    entry["tint"] = filter.neutralTint
    entry["scale"] = scale
    return entry
}

guard let first = filter.outputImage else { fail("no image from \(input.path)") }
let native = first.extent
var dumps: [String: Any] = [:]
let start = Date()
dumps["as-shot"] = render("as-shot")

// D55 at half size, the middle of it, for the crop
filter.neutralChromaticity = CGPoint(x: 0.33242, y: 0.34743)
let half = scaled(filter.outputImage!, by: 0.5)
let w = min(2048, half.extent.width.rounded(.down)), h = min(2048, half.extent.height.rounded(.down))
let middle = CGRect(x: ((half.extent.width - w) / 2).rounded(.down), y: ((half.extent.height - h) / 2).rounded(.down),
                    width: w, height: h)
var align = write(half, bounds: middle, to: "align")
align["scale"] = 0.5
align["fullWidth"] = Int(half.extent.width)
align["fullHeight"] = Int(half.extent.height)
dumps["align"] = align

// the app's own white balances: the films' references (RawLoader's
// referenceChromaticity and referenceTemperature), Daylight D55 and D65,
// Tungsten 3200 K (BB3200, the Planckian 3200 K) and 2850 K
for (name, xy) in [("D50", CGPoint(x: 0.34567, y: 0.35850)), ("D55", CGPoint(x: 0.33242, y: 0.34743)),
                   ("D65", CGPoint(x: 0.31271, y: 0.32902)), ("BB3200", CGPoint(x: 0.42336, y: 0.39895))] {
    filter.neutralChromaticity = xy
    dumps[name] = render(name)
}
for (name, kelvin) in [("T", Float(2856)), ("TH-KG3", 3400), ("K75P", 5400), ("Tungsten", 2850)] {
    filter.neutralTemperature = kelvin
    filter.neutralTint = 0
    dumps[name] = render(name)
}
// any probes asked for, between the grid's points
for (i, arg) in args.enumerated() where arg == "--probe" && i + 1 < args.count {
    let parts = args[i + 1].split(separator: ":").compactMap { Float($0) }
    guard parts.count == 2 else { fail("--probe takes K:TINT, not \(args[i + 1])") }
    filter.neutralTemperature = parts[0]
    filter.neutralTint = parts[1]
    dumps["probe-\(Int(parts[0]))-\(Int(parts[1]))"] = render("probe-\(Int(parts[0]))-\(Int(parts[1]))")
}
// and a grid of temperatures and tints, for Custom and As shot
for kelvin in [2000, 2500, 2856, 3400, 4500, 5500, 6504, 8000, 10000] {
    for tint in [-50, -25, -10, 0, 10, 25, 50] {
        filter.neutralTemperature = Float(kelvin)
        filter.neutralTint = Float(tint)
        dumps["grid-\(kelvin)-\(tint)"] = render("grid-\(kelvin)-\(tint)")
    }
}

let info: [String: Any] = [
    "input": input.lastPathComponent, "nativeWidth": Int(native.width), "nativeHeight": Int(native.height),
    "baselineExposure": filter.baselineExposure, "exifOrientation": exifOrientation, "asShot": asShot,
    "defaults": defaults, "dumps": dumps, "seconds": Date().timeIntervalSince(start),
    "os": ProcessInfo.processInfo.operatingSystemVersionString,
]
do {
    try JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys])
        .write(to: outdir.appendingPathComponent("apple.json"))
} catch {
    fail("cannot write apple.json: \(error)")
}
print("\(input.lastPathComponent): \(Int(native.width)) x \(Int(native.height)), \(dumps.count) dumps in "
      + String(format: "%.1f s", Date().timeIntervalSince(start)))
