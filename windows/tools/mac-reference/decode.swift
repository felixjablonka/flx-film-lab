// flxdecode — Apple's RAW decoder as the Mac app's RawLoader sets it up
// (CIRAWFilter, rendered into linear Rec.2020, downscaled with Core Image's
// Lanczos), writing the decoded picture as a dump for the port's own decoder
// to be compared with. Each of the steps RawLoader leaves at Apple's defaults
// can be switched off, to compare the port with Apple's core decode first and
// add them one by one.
//
//   flxdecode <raw> <out.f32> [--size PX] [--reference D55 | --temperature K --tint T | --as-shot]
//             [--no-sharpening] [--no-colour-nr] [--no-lens-correction] [--no-moire]
//
// --temperature and --tint are RawLoader's custom white balance, Core
// Image's own scale.
//
// With none of the --no- flags it decodes as RawLoader.load does, bit for
// bit (its configure() is private, so it is repeated here); out.json beside
// the dump holds what CIRAWFilter says about the file.

import CoreImage
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
guard args.count >= 2 else {
    fail("usage: flxdecode <raw> <out.f32> [--size PX] [--reference D55] [--no-sharpening] [--no-colour-nr] [--no-lens-correction] [--no-moire]")
}
let input = URL(fileURLWithPath: args.removeFirst())
let output = URL(fileURLWithPath: args.removeFirst())
func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
let size = option("--size").flatMap(Int.init)
let reference = option("--reference") ?? "D55"

// RawLoader.workingColorSpace and its context.
let workingSpace = CGColorSpace(name: CGColorSpace.extendedLinearITUR_2020)!
let context = CIContext(options: [.workingColorSpace: workingSpace, .workingFormat: CIFormat.RGBAf,
                                  .cacheIntermediates: false])

guard let filter = CIRAWFilter(imageURL: input) else { fail("CIRAWFilter cannot read \(input.path)") }
let defaults: [String: Any] = [
    "sharpnessAmount": filter.sharpnessAmount, "colorNoiseReductionAmount": filter.colorNoiseReductionAmount,
    "luminanceNoiseReductionAmount": filter.luminanceNoiseReductionAmount,
    "moireReductionAmount": filter.moireReductionAmount, "detailAmount": filter.detailAmount,
    "lensCorrectionSupported": filter.isLensCorrectionSupported,
]
let asShot: [String: Any] = ["temperature": filter.neutralTemperature, "tint": filter.neutralTint,
                             "x": filter.neutralChromaticity.x, "y": filter.neutralChromaticity.y]

// RawLoader.configure, for .filmReference.
filter.boostAmount = 0
filter.boostShadowAmount = 0
filter.shadowBias = 0
filter.luminanceNoiseReductionAmount = 0
filter.isGamutMappingEnabled = false
filter.isLensCorrectionEnabled = true
filter.localToneMapAmount = 0
filter.extendedDynamicRangeAmount = 2
// RawLoader.referenceChromaticity: BB3200, the Planckian 3200 K, is also its
// Tungsten 3200 K, and D55 its Daylight D55
let chromaticity: [String: CGPoint] = ["D50": CGPoint(x: 0.34567, y: 0.35850), "D55": CGPoint(x: 0.33242, y: 0.34743),
                                       "D65": CGPoint(x: 0.31271, y: 0.32902), "BB3200": CGPoint(x: 0.42336, y: 0.39895)]
let temperature: [String: Float] = ["T": 2856, "TH-KG3": 3400, "TH-KG3-L": 3400, "D50": 5003, "D65": 6504, "K75P": 5400]
if args.contains("--as-shot") {
    // RawLoader's .asShot: the camera's own neutral point
} else if let k = option("--temperature").flatMap(Float.init) {
    filter.neutralTemperature = k                   // RawLoader's .custom
    filter.neutralTint = option("--tint").flatMap(Float.init) ?? 0
} else if let xy = chromaticity[reference] {
    filter.neutralChromaticity = xy
} else {
    filter.neutralTemperature = temperature[reference] ?? 5503
    filter.neutralTint = 0
}

// The steps RawLoader leaves at Apple's defaults.
if args.contains("--no-sharpening") { filter.sharpnessAmount = 0 }
if args.contains("--no-colour-nr") { filter.colorNoiseReductionAmount = 0 }
if args.contains("--no-lens-correction") { filter.isLensCorrectionEnabled = false }
if args.contains("--no-moire") { filter.moireReductionAmount = 0 }

guard var image = filter.outputImage else { fail("no image from \(input.path)") }
let native = image.extent
// RawLoader.downscale
if let size, size > 0, max(image.extent.width, image.extent.height) > CGFloat(size) {
    let scale = CGFloat(size) / max(image.extent.width, image.extent.height)
    let lanczos = CIFilter(name: "CILanczosScaleTransform")!
    lanczos.setValue(image, forKey: kCIInputImageKey)
    lanczos.setValue(scale, forKey: kCIInputScaleKey)
    lanczos.setValue(1.0, forKey: kCIInputAspectRatioKey)
    image = (lanczos.outputImage ?? image).clampedToExtent()
        .cropped(to: CGRect(x: 0, y: 0, width: (image.extent.width * scale).rounded(.down),
                            height: (image.extent.height * scale).rounded(.down)))
}
let extent = image.extent.integral
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
    try (header + pixels.withUnsafeBytes { Data($0) }).write(to: output)
    let info: [String: Any] = [
        "input": input.lastPathComponent, "width": width, "height": height,
        "nativeWidth": Int(native.width), "nativeHeight": Int(native.height),
        "baselineExposure": filter.baselineExposure, "orientation": filter.orientation.rawValue,
        "asShot": asShot,
        "reference": reference, "defaults": defaults,
        "neutral": ["temperature": filter.neutralTemperature, "tint": filter.neutralTint],
        "switchedOff": args.filter { $0.hasPrefix("--no-") },
    ]
    try JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys])
        .write(to: output.deletingPathExtension().appendingPathExtension("json"))
} catch {
    fail("cannot write \(output.path): \(error)")
}
print("\(input.lastPathComponent): \(Int(native.width)) x \(Int(native.height)) -> \(width) x \(height), \(output.path)")
