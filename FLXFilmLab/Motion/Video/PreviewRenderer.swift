import Foundation
import Metal
import QuartzCore
import CoreImage
import CoreVideo

/// What surrounds the frame in the preview: the same three surrounds as the
/// Stills mode, because brightness and colour are judged against them.
enum Surround: String, CaseIterable, Identifiable {
    case dark = "Dark"
    case middleGrey = "Middle grey"
    case white = "White"

    var id: String { rawValue }

    /// Linear light; the Stills mode's sRGB greys 0.07, 0.4663 (L* 50) and 1.
    var linear: Float {
        switch self {
        case .dark: return 0.00619
        case .middleGrey: return 0.1842
        case .white: return 1
        }
    }

    var symbol: String {
        switch self {
        case .dark: return "circle.fill"
        case .middleGrey: return "circle.lefthalf.filled"
        case .white: return "circle"
        }
    }

    var next: Surround {
        let all = Surround.allCases
        return all[(all.firstIndex(of: self)! + 1) % all.count]
    }
}

/// Draws the current frame into the preview layer, off the main thread.
///
/// Holds the latest decoded frame, so a slider moved while paused redraws it at
/// once. While one frame is drawing, further requests collapse into a single
/// follow-up with the newest state, so a drag never queues up stale frames.
final class PreviewRenderer: @unchecked Sendable {

    struct State {
        var effects = GlowLook()
        var input = InputSettings()
        var mode: FramePipeline.Mode = .graded
        var quarterTurns = 0
        var surround = Surround.dark
    }

    let pipeline: FramePipeline
    private let queue = DispatchQueue(label: "FLXFilmLab.motion.preview", qos: .userInteractive)
    // Everything below is touched only on `queue`.
    private var layer: CAMetalLayer?
    private var frame: CVPixelBuffer?
    private var state = State()
    private var inFlight = false
    private var pending = false
    private var warmedUp = false

    /// After each drawn frame, on the main queue: how long it took, or why it failed.
    var onDraw: (@MainActor (Double, Error?) -> Void)?

    init() throws {
        pipeline = try FramePipeline()
    }

    /// Call on the main thread, once the view has its layer.
    func attach(_ layer: CAMetalLayer) {
        layer.device = pipeline.device
        layer.pixelFormat = .rgba16Float
        layer.colorspace = FramePipeline.linearSRGB
        layer.framebufferOnly = false
        layer.isOpaque = true
        queue.async {
            self.layer = layer
            self.draw()
        }
    }

    func show(_ frame: CVPixelBuffer, quarterTurns: Int) {
        queue.async {
            self.frame = frame
            self.state.quarterTurns = quarterTurns
            self.draw()
        }
    }

    func setLUT(_ lut: LookLUT) {
        queue.async {
            self.pipeline.setLUT(lut)
            if !self.warmedUp {
                self.warmedUp = true
                self.warmUp()
            }
            self.draw()
        }
    }

    /// Core Image compiles its kernels on first use, which costs the first
    /// frame a couple of seconds. Running a tiny frame through every path
    /// right after launch moves that to while a clip is being chosen.
    private func warmUp() {
        var buffer: CVPixelBuffer?
        CVPixelBufferCreate(nil, 64, 36, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
                            [kCVPixelBufferMetalCompatibilityKey: true,
                             kCVPixelBufferIOSurfacePropertiesKey: [String: Any]()] as CFDictionary, &buffer)
        guard let buffer else { return }
        CVPixelBufferLockBaseAddress(buffer, [])
        for plane in 0..<2 {
            let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!
                .assumingMemoryBound(to: UInt16.self)
            let count = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) / 2 * CVPixelBufferGetHeightOfPlane(buffer, plane)
            for i in 0..<count { base[i] = UInt16(plane == 0 ? 372 : 512) << 6 }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        var every = GlowLook()
        every.bloom.enabled = true
        every.glow.enabled = true
        every.halation.enabled = true
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 128, height: 128, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .private
        guard let target = pipeline.device.makeTexture(descriptor: d) else { return }
        for effects in [GlowLook(), every] {
            guard let cb = pipeline.queue.makeCommandBuffer() else { return }
            try? pipeline.renderPreview(buffer, into: target, quarterTurns: 0, padding: 4,
                                        background: CIColor(red: 0, green: 0, blue: 0), effects: effects,
                                        settings: InputSettings(), mode: .graded, commandBuffer: cb)
            cb.commit()
            cb.waitUntilCompleted()
        }
    }

    func update(_ change: @escaping @Sendable (inout State) -> Void) {
        queue.async {
            change(&self.state)
            self.draw()
        }
    }

    /// The layer changed size.
    func redraw() {
        queue.async { self.draw() }
    }

    private func draw() {
        guard !inFlight else {
            pending = true
            return
        }
        guard let layer, layer.drawableSize.width >= 1, layer.drawableSize.height >= 1,
              let drawable = layer.nextDrawable(),
              let cb = pipeline.queue.makeCommandBuffer() else { return }
        let start = CACurrentMediaTime()
        let surround = CIColor(red: CGFloat(state.surround.linear), green: CGFloat(state.surround.linear),
                               blue: CGFloat(state.surround.linear), alpha: 1,
                               colorSpace: FramePipeline.linearSRGB) ?? CIColor(red: 0, green: 0, blue: 0)
        var failure: Error?
        do {
            if let frame, pipeline.hasLUT {
                try pipeline.renderPreview(frame, into: drawable.texture, quarterTurns: state.quarterTurns,
                                           padding: 20 * layer.contentsScale, background: surround,
                                           effects: state.effects, settings: state.input, mode: state.mode,
                                           commandBuffer: cb)
            } else {
                try pipeline.fill(drawable.texture, with: surround, commandBuffer: cb)
            }
        } catch {
            failure = error
        }
        inFlight = true
        cb.addCompletedHandler { [weak self] _ in
            let ms = (CACurrentMediaTime() - start) * 1000
            guard let self else { return }
            self.queue.async {
                self.inFlight = false
                if self.pending {
                    self.pending = false
                    self.draw()
                }
            }
            if let report = self.onDraw {
                DispatchQueue.main.async { MainActor.assumeIsolated { report(ms, failure) } }
            }
        }
        cb.present(drawable)
        cb.commit()
    }
}
