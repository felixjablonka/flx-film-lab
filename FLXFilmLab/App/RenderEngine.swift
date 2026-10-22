import Foundation
import CoreGraphics

/// The preview's render loop, off the main thread.
///
/// A slider tick used to run all of this on the main actor — the meters, the
/// mask rasteriser, the whole GPU graph and the readback, up to ~60 ms with
/// masks and the autos on — so the controls stalled between frames. Here it
/// runs on its own actor, and each stage is skipped when nothing it depends on
/// has changed: the film stage (inside `Renderer`), each mask's coverage, the
/// auto-exposure reading and the print meters' reading.
actor RenderEngine {

    struct Output {
        var image: CGImage?
        /// The recipe as printed, every metered value filled in.
        var resolved: Recipe
        var metered: Metered
        var milliseconds: Double
    }

    /// What the meters read, for the controls whose auto is on.
    struct Metered {
        var exposureEV: Double?
        var brightness: Double?
        var grade: Double?
    }

    private let renderer: Renderer
    private var source: RawLoader.Decoded?
    /// Identifies the image currently held, so a render requested for one
    /// image is never drawn from the next.
    private(set) var sourceID = 0

    private var exposureReading: (stock: String, ev: Float)?
    private var printReading: (key: PrintReadingKey, reading: RenderSetupBuilder.PrintReading?)?
    private var coverages: [UUID: (geometry: MaskRasterizer.Geometry, coverage: [Float])] = [:]
    private var lastMasks: [LocalMask]?

    /// Everything the print meters' reading depends on. It is retaken only
    /// when one of these moves, not on every tone or colour slider tick.
    private struct PrintReadingKey: Equatable {
        var film: String
        var lensFilter: String?
        var paper: String
        var scanFilm: Bool?
        var exposureEV: Double
        var filterY: Double
        var filterM: Double
        var gamma: Double

        init(_ r: Recipe) {
            film = r.filmStock
            lensFilter = r.lensFilter
            paper = r.printStock
            scanFilm = r.scanFilm
            exposureEV = r.exposureEV
            filterY = r.filterShiftY
            filterM = r.filterShiftM
            gamma = r.densityCurveGamma
        }
    }

    init() throws {
        renderer = try Renderer()
    }

    /// Hand the engine a newly decoded image. Every cached reading belonged to
    /// the previous one.
    func setSource(_ decoded: RawLoader.Decoded) -> Int {
        renderer.setImage(decoded.pixels, width: decoded.width, height: decoded.height)
        source = decoded
        sourceID += 1
        exposureReading = nil
        printReading = nil
        coverages = [:]
        lastMasks = nil
        return sourceID
    }

    /// The recipe as it will actually print: each control whose auto is on
    /// carries the meter's value in place of the stored one.
    func resolve(_ recipe: Recipe) throws -> (recipe: Recipe, metered: Metered) {
        guard let source else { return (recipe, Metered()) }
        var working = recipe
        var metered = Metered()

        if recipe.autoExposure, let film = try? PackLibrary.shared.pack(recipe.filmStock) {
            if exposureReading?.stock != recipe.filmStock {
                let ev = RenderSetupBuilder.autoExposureEV(pixels: source.pixels, width: source.width,
                                                           height: source.height, film: film)
                exposureReading = (recipe.filmStock, ev)
            }
            if let ev = exposureReading?.ev {
                working.exposureEV = Double(ev)
                metered.exposureEV = Double(ev)
            }
        }

        guard working.autoDensity || working.autoGrade else { return (working, metered) }
        let key = PrintReadingKey(working)
        if printReading?.key != key {
            let reading = try RenderSetupBuilder.readPrint(recipe: working, pixels: source.pixels,
                                                           width: source.width, height: source.height,
                                                           exposureEV: Float(working.exposureEV))
            printReading = (key, reading)
        }
        guard let reading = printReading?.reading else { return (working, metered) }
        let resolved = RenderSetupBuilder.applyAuto(recipe: working, reading: reading)
        if resolved.autoDensity { metered.brightness = resolved.brightness }
        if resolved.autoGrade { metered.grade = resolved.grade }
        return (resolved, metered)
    }

    /// Render `recipe` for the image `expected` — or nothing, if that image
    /// has been replaced since the request was made.
    func render(_ recipe: Recipe, for expected: Int) throws -> Output? {
        guard expected == sourceID, let source else { return nil }
        let start = CFAbsoluteTimeGetCurrent()
        let (resolved, metered) = try resolve(recipe)
        let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: Float(resolved.exposureEV))
        updateMasks(resolved.masks, width: source.width, height: source.height)
        _ = try renderer.render(setup: setup, recipe: resolved)
        let image = renderer.makeDisplayImage(colorSpaceName: resolved.outputColorSpace)
        return Output(image: image, resolved: resolved, metered: metered,
                      milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000)
    }

    /// Rebuild the dodge-and-burn map only when the masks change, and redraw
    /// only the masks whose geometry changed: dragging a mask's exposure or
    /// contrast just reweights coverages already drawn.
    private func updateMasks(_ masks: [LocalMask], width: Int, height: Int) {
        guard masks != lastMasks else { return }
        lastMasks = masks
        let active = masks.filter(MaskRasterizer.isActive)
        guard !active.isEmpty else {
            renderer.clearDodgeBurn()
            coverages = [:]
            return
        }
        let size = MaskRasterizer.mapSize(width: width, height: height)
        var drawn: [(mask: LocalMask, coverage: [Float])] = []
        var kept: [UUID: (geometry: MaskRasterizer.Geometry, coverage: [Float])] = [:]
        for mask in active {
            let geometry = MaskRasterizer.Geometry(mask)
            let coverage: [Float]
            if let cached = coverages[mask.id], cached.geometry == geometry {
                coverage = cached.coverage
            } else {
                coverage = MaskRasterizer.coverage(of: mask, mapWidth: size.width, mapHeight: size.height)
            }
            kept[mask.id] = (geometry, coverage)
            drawn.append((mask, coverage))
        }
        coverages = kept
        renderer.setDodgeBurnMap(MaskRasterizer.combine(drawn, mapWidth: size.width, mapHeight: size.height))
    }
}
