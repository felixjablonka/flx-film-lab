import Foundation
import Metal

/// Each of the renderer's passes timed on the GPU, for profiling: with
/// `FLX_PASS_TIMES=1`, the GPU takes a timestamp as each pass's encoder starts
/// and ends (stage-boundary sampling, which Apple's GPUs have), and the times
/// add up by kernel name across frames until `report()`. Off, it costs nothing.
/// The passes Metal Performance Shaders encodes (the Gaussian blurs) have
/// encoders of their own and are not timed: they are what the frame's GPU time
/// leaves over the timed passes.
final class PassTimer: @unchecked Sendable {
    private let buffer: MTLCounterSampleBuffer
    private let device: MTLDevice
    private var names: [String] = []
    private var calibration: (cpu: MTLTimestamp, gpu: MTLTimestamp) = (0, 0)

    private static let lock = NSLock()
    nonisolated(unsafe) private static var totals: [String: (seconds: Double, count: Int)] = [:]
    nonisolated(unsafe) private static var frames = 0
    nonisolated(unsafe) private static var frameSeconds = 0.0

    init?(device: MTLDevice) {
        guard ProcessInfo.processInfo.environment["FLX_PASS_TIMES"] == "1",
              device.supportsCounterSampling(.atStageBoundary),
              let set = device.counterSets?.first(where: { $0.name == MTLCommonCounterSet.timestamp.rawValue })
        else { return nil }
        let descriptor = MTLCounterSampleBufferDescriptor()
        descriptor.counterSet = set
        descriptor.storageMode = .shared
        descriptor.sampleCount = 1024
        guard let buffer = try? device.makeCounterSampleBuffer(descriptor: descriptor) else { return nil }
        self.buffer = buffer
        self.device = device
    }

    /// A frame's passes start: what is timed from here belongs to it.
    func begin() {
        names.removeAll()
        calibration = device.sampleTimestamps()
    }

    /// The next pass's descriptor, `name`'s, sampling at its start and end.
    func pass(_ name: String) -> MTLComputePassDescriptor? {
        guard names.count * 2 + 2 <= buffer.sampleCount else { return nil }
        let pass = MTLComputePassDescriptor()
        guard let attachment = pass.sampleBufferAttachments[0] else { return nil }
        attachment.sampleBuffer = buffer
        attachment.startOfEncoderSampleIndex = names.count * 2
        attachment.endOfEncoderSampleIndex = names.count * 2 + 1
        names.append(name)
        return pass
    }

    /// The frame's command buffer has completed: its passes' times, added up.
    func end(_ cb: MTLCommandBuffer) {
        defer { names.removeAll() }
        guard !names.isEmpty, let data = try? buffer.resolveCounterRange(0..<names.count * 2) else { return }
        let now = device.sampleTimestamps()
        // GPU ticks to seconds, through the CPU's clock (in nanoseconds) over
        // the same span
        let cpuSeconds = Double(now.cpu &- calibration.cpu) / 1e9
        let gpuTicks = Double(now.gpu &- calibration.gpu)
        guard gpuTicks > 0 else { return }
        let secondsPerTick = cpuSeconds / gpuTicks
        let stamps = data.withUnsafeBytes { Array($0.bindMemory(to: MTLCounterResultTimestamp.self)) }
        Self.lock.lock()
        defer { Self.lock.unlock() }
        for (i, name) in names.enumerated() where 2 * i + 1 < stamps.count {
            let start = stamps[2 * i].timestamp, end = stamps[2 * i + 1].timestamp
            guard start != MTLCounterErrorValue, end != MTLCounterErrorValue, end >= start else { continue }
            Self.totals[name, default: (0, 0)].seconds += Double(end - start) * secondsPerTick
            Self.totals[name, default: (0, 0)].count += 1
        }
        Self.frames += 1
        Self.frameSeconds += cb.gpuEndTime - cb.gpuStartTime
    }

    /// The passes' mean times per frame since the last report, slowest first,
    /// and what the frames' GPU time leaves over them; then starts again.
    static func report() -> String {
        lock.lock()
        defer { totals = [:]; frames = 0; frameSeconds = 0; lock.unlock() }
        guard frames > 0 else { return "no frames timed" }
        let n = Double(frames)
        let timed = totals.values.reduce(0) { $0 + $1.seconds }
        var lines = [String(format: "%d frames: %.2f ms on the GPU each, %.2f of it in the timed passes",
                            frames, frameSeconds / n * 1000, timed / n * 1000)]
        for (name, value) in totals.sorted(by: { $0.value.seconds > $1.value.seconds }) {
            lines.append(String(format: "  %7.2f ms  %4.1f×  %@", value.seconds / n * 1000,
                                Double(value.count) / n, name))
        }
        return lines.joined(separator: "\n")
    }
}
