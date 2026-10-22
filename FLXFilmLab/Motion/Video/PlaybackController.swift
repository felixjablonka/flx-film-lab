import Foundation
import AVFoundation
import Combine
import QuartzCore

/// Plays a clip: AVPlayer keeps the clock and the sound, and hands over each
/// decoded frame — untouched, as recorded — for the preview to grade.
///
/// Nothing is drawn by the player itself. The view's display link asks for
/// the frame due at each refresh; paused, a seek or a step produces exactly
/// one new frame.
@MainActor
final class PlaybackController: ObservableObject {
    @Published private(set) var isPlaying = false
    @Published private(set) var time: Double = 0
    @Published private(set) var duration: Double = 0
    @Published private(set) var frameRate: Double = 0

    let player = AVPlayer()
    private var output: AVPlayerItemVideoOutput?
    private var info: ClipInfo?
    private var timeObserver: Any?
    private var endObserver: NSObjectProtocol?
    private var statusObservation: NSKeyValueObservation?
    private var seekTarget: CMTime?
    private var isSeeking = false

    /// Each newly decoded frame, with the clip it belongs to.
    var onFrame: ((CVPixelBuffer, ClipInfo) -> Void)?

    init() {
        player.actionAtItemEnd = .pause
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 30),
                                                      queue: .main) { [weak self] t in
            MainActor.assumeIsolated {
                guard let self, !self.isSeeking, self.seekTarget == nil else { return }
                self.time = t.seconds.isFinite ? t.seconds : 0
            }
        }
    }

    func load(_ clip: ClipInfo) {
        pause()
        let item = AVPlayerItem(asset: AVURLAsset(url: clip.url))
        let output = AVPlayerItemVideoOutput(pixelBufferAttributes: clip.decodeAttributes)
        item.add(output)
        self.output = output
        info = clip
        if let endObserver { NotificationCenter.default.removeObserver(endObserver) }
        endObserver = NotificationCenter.default.addObserver(forName: AVPlayerItem.didPlayToEndTimeNotification,
                                                             object: item, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.isPlaying = false }
        }
        // The first frame only arrives once something asks for one.
        statusObservation = item.observe(\.status, options: [.new]) { [weak self] item, _ in
            guard item.status == .readyToPlay else { return }
            DispatchQueue.main.async {
                MainActor.assumeIsolated { self?.seek(to: 0) }
            }
        }
        player.replaceCurrentItem(with: item)
        duration = clip.durationSeconds
        frameRate = clip.frameRate
        time = 0
    }

    /// From the display link: hand on the frame due at `hostTime`, if it is new.
    func tick(hostTime: CFTimeInterval) {
        guard let output, let info else { return }
        let itemTime = output.itemTime(forHostTime: hostTime)
        guard output.hasNewPixelBuffer(forItemTime: itemTime),
              let buffer = output.copyPixelBuffer(forItemTime: itemTime, itemTimeForDisplay: nil) else { return }
        onFrame?(buffer, info)
    }

    func togglePlay() { isPlaying ? pause() : play() }

    func play() {
        guard player.currentItem != nil else { return }
        if duration > 0, time >= duration - 0.5 / max(frameRate, 1) {
            seek(to: 0)
        }
        player.play()
        isPlaying = true
    }

    func pause() {
        player.pause()
        isPlaying = false
    }

    /// Frame-exact; while one seek runs, only the latest of the requests made
    /// in the meantime follows it.
    func seek(to seconds: Double) {
        let clamped = min(max(seconds, 0), duration)
        time = clamped
        seekTarget = CMTime(seconds: clamped, preferredTimescale: 600)
        if !isSeeking { performSeek() }
    }

    private func performSeek() {
        guard let target = seekTarget else {
            isSeeking = false
            return
        }
        seekTarget = nil
        isSeeking = true
        player.seek(to: target, toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated { self?.performSeek() }
            }
        }
    }

    func step(_ frames: Int) {
        pause()
        guard let item = player.currentItem else { return }
        item.step(byCount: frames)
        let t = item.currentTime().seconds + Double(frames) / max(frameRate, 1)
        time = min(max(t.isFinite ? t : 0, 0), duration)
    }
}
