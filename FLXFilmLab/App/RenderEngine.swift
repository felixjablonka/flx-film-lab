import Foundation
import CoreGraphics
import CoreImage

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
        /// Where the image's pixels come from in the frame.
        var map: GeometryMap
        var milliseconds: Double
        /// The finished print's histogram, and the one the curves were given.
        var histogram: PrintHistogram?
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

    private var exposureReading: (stock: String, geometry: FrameGeometry, ev: Float)?
    private var printReading: (key: PrintReadingKey, reading: RenderSetupBuilder.PrintReading?)?
    /// The part of the frame the meters last read, and the crop it was cut
    /// for (`FrameGeometry.upright`).
    private var meteredArea: (geometry: FrameGeometry, pixels: [Float], width: Int, height: Int)?
    private var coverages: [UUID: (geometry: MaskRasterizer.Geometry, coverage: [Float])] = [:]
    private var lastMasks: [LocalMask]?
    private var grainSeed: UInt32 = 0

    /// The loupe's renderer, which holds the frame at full size while the
    /// loupe is open, and what that frame was decoded with.
    private var loupe: Renderer?
    private var loupeFrame: (key: String, width: Int, height: Int)?
    /// The masks the loupe's map was drawn for.
    private var loupeMasks: [LocalMask]?
    /// The recipe whose print, before its effects, the preview's renderer
    /// keeps (`Renderer.printBeforeEffects`): the loupe's effects gather the
    /// light from beyond the part from it.
    private var keptBeforeEffects: Recipe?

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
        var geometry: FrameGeometry

        init(_ r: Recipe, geometry: FrameGeometry) {
            film = r.filmStock
            lensFilter = r.lensFilter
            paper = r.printStock
            scanFilm = r.scanFilm
            exposureEV = r.exposureEV
            filterY = r.filterShiftY
            filterM = r.filterShiftM
            gamma = r.densityCurveGamma
            self.geometry = geometry
        }
    }

    init() throws {
        renderer = try Renderer()
        renderer.measuresHistogram = true
    }

    /// Hand the engine a newly decoded image, with its grain seed
    /// (`Renderer.grainSeed(for:)`). Every cached reading belonged to the
    /// previous one.
    func setSource(_ decoded: RawLoader.Decoded, grainSeed: UInt32) -> Int {
        renderer.setImage(decoded.pixels, width: decoded.width, height: decoded.height)
        renderer.grainSeed = grainSeed
        self.grainSeed = grainSeed
        keptBeforeEffects = nil
        source = decoded
        sourceID += 1
        exposureReading = nil
        printReading = nil
        meteredArea = nil
        coverages = [:]
        lastMasks = nil
        return sourceID
    }

    /// The recipe as it will actually print: each control whose auto is on
    /// carries the meter's value in place of the stored one. The meters read
    /// only what the print keeps of the frame: the recipe's crop, or
    /// `metering`'s while a crop is being drawn.
    func resolve(_ recipe: Recipe, metering: FrameGeometry? = nil) throws -> (recipe: Recipe, metered: Metered) {
        guard source != nil else { return (recipe, Metered()) }
        var working = recipe
        var metered = Metered()
        let geometry = (metering ?? recipe.geometry).upright

        if recipe.autoExposure, let film = try? PackLibrary.shared.pack(recipe.filmStock) {
            if exposureReading?.stock != recipe.filmStock || exposureReading?.geometry != geometry {
                let area = meteredArea(geometry)
                let ev = RenderSetupBuilder.autoExposureEV(pixels: area.pixels, width: area.width,
                                                           height: area.height, film: film)
                exposureReading = (recipe.filmStock, geometry, ev)
            }
            if let ev = exposureReading?.ev {
                working.exposureEV = Double(ev)
                metered.exposureEV = Double(ev)
            }
        }

        guard working.autoDensity || working.autoGrade else { return (working, metered) }
        let key = PrintReadingKey(working, geometry: geometry)
        if printReading?.key != key {
            let area = meteredArea(geometry)
            let reading = try RenderSetupBuilder.readPrint(recipe: working, pixels: area.pixels,
                                                           width: area.width, height: area.height,
                                                           exposureEV: Float(working.exposureEV))
            printReading = (key, reading)
        }
        guard let reading = printReading?.reading else { return (working, metered) }
        let resolved = RenderSetupBuilder.applyAuto(recipe: working, reading: reading)
        if resolved.autoDensity { metered.brightness = resolved.brightness }
        if resolved.autoGrade { metered.grade = resolved.grade }
        return (resolved, metered)
    }

    /// The part of the frame an upright crop keeps, cut once per crop.
    private func meteredArea(_ upright: FrameGeometry) -> (pixels: [Float], width: Int, height: Int) {
        guard let source else { return ([], 0, 0) }
        if let area = meteredArea, area.geometry == upright { return (area.pixels, area.width, area.height) }
        let area = upright.metered(source.pixels, width: source.width, height: source.height)
        meteredArea = (upright, area.pixels, area.width, area.height)
        return area
    }

    /// Render `recipe` for the image `expected` — or nothing, if that image
    /// has been replaced since the request was made. `wholeFrame` shows the
    /// whole frame, turned and levelled, for drawing the crop on, and
    /// `metering` is the crop the meters read meanwhile.
    func render(_ recipe: Recipe, for expected: Int, wholeFrame: Bool = false,
                metering: FrameGeometry? = nil) throws -> Output? {
        guard expected == sourceID, let source else { return nil }
        let start = CFAbsoluteTimeGetCurrent()
        let (resolved, metered) = try resolve(recipe, metering: metering)
        let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: Float(resolved.exposureEV))
        updateMasks(resolved.masks, width: source.width, height: source.height)
        _ = try renderer.render(setup: setup, recipe: resolved, wholeFrame: wholeFrame)
        keptBeforeEffects = renderer.keepsPrintBeforeEffects && !wholeFrame && (resolved.effects?.isActive ?? false)
            ? resolved : nil
        let image = renderer.makeDisplayImage(colorSpaceName: resolved.outputColorSpace)
        let map = GeometryMap(resolved.geometry, sourceWidth: source.width, sourceHeight: source.height,
                              wholeFrame: wholeFrame)
        let histogram = renderer.histogram.map { PrintHistogram(after: $0.after, before: $0.before) }
        return Output(image: image, resolved: resolved, metered: metered, map: map,
                      milliseconds: (CFAbsoluteTimeGetCurrent() - start) * 1000, histogram: histogram)
    }

    // MARK: - The loupe

    /// What the frame held at full size for the loupe was decoded with, if
    /// one is held.
    var loupeKey: String? { loupeFrame?.key }

    /// Hold `decoded`, the image on screen decoded at full size, for the
    /// loupe. False if another image has come in meanwhile.
    func holdFullFrame(_ decoded: RawLoader.Decoded, key: String, for expected: Int) throws -> Bool {
        guard expected == sourceID else { return false }
        if loupe == nil { loupe = try Renderer() }
        guard let loupe else { return false }
        loupe.releaseFrame()
        loupe.grainSeed = grainSeed
        loupe.holdFrame(decoded.pixels, width: decoded.width, height: decoded.height)
        guard loupe.holdsFrame else {
            loupeFrame = nil
            return false
        }
        loupeFrame = (key, decoded.width, decoded.height)
        loupeMasks = nil
        renderer.keepsPrintBeforeEffects = true
        return true
    }

    /// Let the frame held for the loupe go (about 16 bytes a pixel: 720 MB
    /// for 45 MP), with everything made for it.
    func releaseFullFrame() {
        loupe = nil
        loupeFrame = nil
        loupeMasks = nil
        keptBeforeEffects = nil
        renderer.keepsPrintBeforeEffects = false
    }

    /// The print at 100% under the loupe: the window of `width` x `height`
    /// pixels of the print at full size centred on `centre` (fractions of the
    /// print), made from the frame held at full size with the values the
    /// preview resolved, exactly as the export makes those pixels. Only that
    /// part of the print is made, with what each stage needs around it.
    func renderLoupe(_ recipe: Recipe, centre: CGPoint, width: Int, height: Int,
                     key: String, for expected: Int) throws -> CGImage? {
        guard expected == sourceID, let loupe, let frame = loupeFrame, frame.key == key,
              width > 0, height > 0 else { return nil }
        let (resolved, _) = try resolve(recipe)
        let setup = try RenderSetupBuilder.build(recipe: resolved, exposureEV: Float(resolved.exposureEV))
        let map = GeometryMap(resolved.geometry, sourceWidth: frame.width, sourceHeight: frame.height)
        let ow = map.outputWidth, oh = map.outputHeight
        let w = min(width, ow), h = min(height, oh)
        let x = min(max(Int((centre.x * CGFloat(ow) - CGFloat(w) / 2).rounded()), 0), ow - w)
        let y = min(max(Int((centre.y * CGFloat(oh) - CGFloat(h) / 2).rounded()), 0), oh - h)
        let reach = Renderer.reach(setup: setup, recipe: resolved, frameWidth: frame.width,
                                   frameHeight: frame.height, shaped: !map.isIdentity)
        let (part, box) = Renderer.loupeRegions(window: CGRect(x: x, y: y, width: w, height: h),
                                                map: map, reach: reach)
        guard loupe.setWindow(x: Int(box.minX), y: Int(box.minY), width: Int(box.width),
                              height: Int(box.height)) else { return nil }
        // The masks drawn on the frame at full size, as the export draws them.
        let masks = resolved.masks.filter(\.enabled)
        if masks != loupeMasks {
            loupeMasks = masks
            if masks.isEmpty {
                loupe.clearDodgeBurn()
            } else {
                loupe.setDodgeBurnMap(MaskRasterizer.rasterize(masks: masks, width: frame.width, height: frame.height))
            }
        }
        var beyond: CIImage?
        if keptBeforeEffects == resolved, let print = renderer.printBeforeEffects {
            beyond = CIImage(mtlTexture: print, options: [.colorSpace: CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!])
        }
        _ = try loupe.render(setup: setup, recipe: resolved, part: Renderer.Part(rect: part, beyond: beyond))
        let made = loupe.madeRect
        return loupe.makeDisplayImage(colorSpaceName: resolved.outputColorSpace)?
            .cropping(to: CGRect(x: x - Int(made.minX), y: y - Int(made.minY), width: w, height: h))
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
