import SwiftUI
import AppKit
import QuartzCore
import UniformTypeIdentifiers

/// The preview canvas: a Metal layer the `PreviewRenderer` draws into, and the
/// display link that pulls frames from the player at the screen's own rate.
///
/// The layer is in extended linear sRGB, so the system takes the print's
/// display light to whatever screen it is on — the same light an export
/// tagged for Apple displays decodes to in QuickTime.
struct PreviewView: NSViewRepresentable {
    let model: ClipModel

    func makeNSView(context: Context) -> PreviewNSView {
        let view = PreviewNSView()
        view.model = model
        return view
    }

    func updateNSView(_ nsView: PreviewNSView, context: Context) {}
}

final class PreviewNSView: NSView {
    weak var model: ClipModel?
    private var link: CADisplayLink?
    private var attached = false

    private var metalLayer: CAMetalLayer? { layer as? CAMetalLayer }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .duringViewResize
        registerForDraggedTypes([.fileURL])
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func makeBackingLayer() -> CALayer { CAMetalLayer() }

    override var isOpaque: Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard let window, let model, let metalLayer else {
            link?.invalidate()
            link = nil
            return
        }
        metalLayer.contentsScale = window.backingScaleFactor
        if !attached {
            model.preview.attach(metalLayer)
            attached = true
        }
        updateDrawableSize()
        if link == nil {
            let link = displayLink(target: self, selector: #selector(tick(_:)))
            link.add(to: .main, forMode: .common)
            self.link = link
        }
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        metalLayer?.contentsScale = window?.backingScaleFactor ?? 2
        updateDrawableSize()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        updateDrawableSize()
    }

    private func updateDrawableSize() {
        guard let metalLayer else { return }
        let scale = metalLayer.contentsScale
        let size = CGSize(width: max(bounds.width * scale, 1), height: max(bounds.height * scale, 1))
        if metalLayer.drawableSize != size {
            metalLayer.drawableSize = size
            model?.preview.redraw()
        }
    }

    @objc private func tick(_ link: CADisplayLink) {
        model?.playback.tick(hostTime: link.targetTimestamp)
    }

    // MARK: Probe

    private var tracking: NSTrackingArea?

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        model?.probe(at: sourcePixel(for: convert(event.locationInWindow, from: nil)))
    }

    override func mouseExited(with event: NSEvent) {
        model?.probe(at: nil)
    }

    /// The source pixel under a view point: the renderer's own placement,
    /// undone, rotation included.
    private func sourcePixel(for point: CGPoint) -> CGPoint? {
        guard let model, let clip = model.clip, let metalLayer else { return nil }
        let scale = metalLayer.contentsScale
        let placement = FramePipeline.Placement(source: clip.size, quarterTurns: clip.quarterTurns,
                                                destination: metalLayer.drawableSize, padding: 20 * scale)
        // The drawable's rows run down from the top; the view's y runs up.
        let p = CGPoint(x: point.x * scale, y: (bounds.height - point.y) * scale)
        let f = placement.frame
        guard f.contains(p) else { return nil }
        let u = (p.x - f.minX) / f.width, v = (p.y - f.minY) / f.height
        let (w, h) = (clip.size.width, clip.size.height)
        switch clip.quarterTurns {
        case 1: return CGPoint(x: v * w, y: (1 - u) * h)
        case 2: return CGPoint(x: (1 - u) * w, y: (1 - v) * h)
        case 3: return CGPoint(x: (1 - v) * w, y: u * h)
        default: return CGPoint(x: u * w, y: v * h)
        }
    }

    // MARK: Drop a clip

    private func movieURL(_ info: NSDraggingInfo) -> URL? {
        let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
        let urls = info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: options) as? [URL]
        return urls?.first { url in
            (UTType(filenameExtension: url.pathExtension.lowercased())?.conforms(to: .movie)) ?? false
        }
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        movieURL(sender) == nil ? [] : .copy
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let url = movieURL(sender), let model else { return false }
        model.open(url)
        return true
    }
}
