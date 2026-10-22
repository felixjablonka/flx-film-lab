import Foundation
import simd
@testable import FLXCore

// Numeric parity harness: runs the Metal engine over the same inputs
// tools/bake/bake_reference.py pushed through the real agx-emulsion pipeline,
// and compares every tap. "Looks like film" is not evidence of a correct port;
// this is.

struct Manifest: Decodable {
    struct TapRef: Decodable { let file: String; let shape: [Int] }
    struct Case: Decodable {
        let shape: [Int]
        let input: String
        let taps: [String: TapRef]
    }
    struct Combo: Decodable {
        let key: String
        let params: Params
        let cases: [String: Case]
    }
    struct Params: Decodable {
        let film: String
        let print: String
        let outputColorSpace: String
        let outputGamutCompress: String
        let scanFilm: Bool?
    }
    let combos: [Combo]
}

func readFloats(_ url: URL) -> [Float]? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return data.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
}

/// The reference is generated with `debug.lut_mode = True`, which forces agx
/// into a deterministic per-pixel transform. Match it exactly.
func lutModeRecipe(film: String, print: String, outputSpace: String, scanFilm: Bool) -> Recipe {
    var r = Recipe()
    r.filmStock = film
    r.printStock = print
    r.exposureEV = 0
    r.autoExposure = false
    r.density = 0
    r.autoDensity = false
    r.autoGrade = false
    r.couplersEnabled = true
    r.couplersAmount = 1.0
    r.couplersDiffusion = 0          // lut_mode zeroes the spatial diffusion
    r.halationEnabled = false
    r.grainEnabled = false
    r.glareEnabled = false
    r.gamutCompress = false
    r.outputColorSpace = outputSpace
    // The reference renders every stock through the print chain, including the
    // slide films. The app's transparency default is a usability choice layered
    // on top; here we assert the physics underneath it.
    r.scanFilm = scanFilm
    // agx reports absolute transmittance from `scan_film`; the app's light-table
    // white balance is a deliberate addition on top, so it is off here.
    r.normalizeToBase = false
    return r
}

struct Stats {
    var maxAbs: Float = 0
    var meanAbs: Float = 0
    var count: Int = 0
    var worstIndex: Int = -1
    var worstReference: Float = 0
    var worstActual: Float = 0
}

/// Compares RGB triples, skipping the alpha the GPU writes but agx has no
/// concept of.
func compare(reference: [Float], actual: [Float], pixels: Int) -> Stats {
    var s = Stats()
    var sum: Float = 0
    for i in 0..<pixels {
        for c in 0..<3 {
            let ref = reference[i * 3 + c]
            let got = actual[i * 4 + c]
            guard ref.isFinite, got.isFinite else { continue }
            let d = abs(ref - got)
            sum += d
            s.count += 1
            if d > s.maxAbs {
                s.maxAbs = d
                s.worstIndex = i
                s.worstReference = ref
                s.worstActual = got
            }
        }
    }
    s.meanAbs = s.count > 0 ? sum / Float(s.count) : 0
    return s
}

// Per-tap tolerances. The film taps are compared in log-exposure and density
// units; rgb_out is linear scene values in [0, 1].
let tolerances: [String: Float] = [
    "log_e_film": 2e-3,
    "cmy_film": 2e-3,
    "rgb_out": 2e-3,
]

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let parityDir = root.appendingPathComponent("tests/parity")

guard let manifestData = try? Data(contentsOf: parityDir.appendingPathComponent("manifest.json")),
      let manifest = try? JSONDecoder().decode(Manifest.self, from: manifestData) else {
    FileHandle.standardError.write(Data("no manifest.json — run tools/bake/bake_reference.py\n".utf8))
    exit(2)
}

let renderer: Renderer
do {
    renderer = try Renderer()
} catch {
    FileHandle.standardError.write(Data("renderer init failed: \(error)\n".utf8))
    exit(2)
}
print("device: \(renderer.device.name)\n")

var failures = 0
var checks = 0

for combo in manifest.combos {
    print("\(combo.key)")
    let recipe = lutModeRecipe(film: combo.params.film,
                               print: combo.params.print,
                               outputSpace: combo.params.outputColorSpace,
                               scanFilm: combo.params.scanFilm ?? false)
    let setup: RenderSetup
    do {
        setup = try RenderSetupBuilder.build(recipe: recipe)
    } catch {
        print("   setup failed: \(error)")
        failures += 1
        continue
    }

    for (caseName, testCase) in combo.cases.sorted(by: { $0.key < $1.key }) {
        let height = testCase.shape[0], width = testCase.shape[1]
        guard let input = readFloats(parityDir.appendingPathComponent(testCase.input)) else {
            print("   \(caseName): missing input")
            failures += 1
            continue
        }
        // The reference is RGB; the engine's textures are RGBA.
        var rgba = [Float](repeating: 1, count: width * height * 4)
        for i in 0..<(width * height) {
            rgba[i * 4 + 0] = input[i * 3 + 0]
            rgba[i * 4 + 1] = input[i * 3 + 1]
            rgba[i * 4 + 2] = input[i * 3 + 2]
        }
        renderer.setImage(rgba, width: width, height: height)

        do {
            _ = try renderer.render(setup: setup, recipe: recipe)
        } catch {
            print("   \(caseName): render failed: \(error)")
            failures += 1
            continue
        }

        var line = "   \(caseName.padding(toLength: 14, withPad: " ", startingAt: 0))"
        for tap in ["log_e_film", "cmy_film", "rgb_out"] {
            guard let ref = testCase.taps[tap].flatMap({ readFloats(parityDir.appendingPathComponent($0.file)) }),
                  let got = renderer.readTap(tap) else { continue }
            let s = compare(reference: ref, actual: got, pixels: width * height)
            checks += 1
            let tol = tolerances[tap] ?? 1e-3
            let ok = s.maxAbs <= tol
            if !ok { failures += 1 }
            line += String(format: "  %@ %@max %.5f mean %.6f",
                           tap, ok ? "ok " : "FAIL ", s.maxAbs, s.meanAbs)
            if !ok {
                line += String(format: " (px %d: ref %.5f got %.5f)", s.worstIndex, s.worstReference, s.worstActual)
            }
        }
        print(line)
    }
}

print("\n\(checks - failures)/\(checks) tap comparisons within tolerance")
exit(failures == 0 ? 0 : 1)
