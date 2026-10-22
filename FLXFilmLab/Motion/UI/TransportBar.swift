import SwiftUI

/// Play, scrub and step. Space plays and pauses, the arrow keys step a frame.
struct TransportBar: View {
    @ObservedObject var playback: PlaybackController

    var body: some View {
        HStack(spacing: 10) {
            Button { playback.step(-1) } label: { Image(systemName: "backward.frame.fill") }
                .buttonStyle(.plain)
                .help("Back one frame (←)")
            Button { playback.togglePlay() } label: {
                Image(systemName: playback.isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 14)
            }
            .buttonStyle(.plain)
            .help("Play or pause (Space)")
            Button { playback.step(1) } label: { Image(systemName: "forward.frame.fill") }
                .buttonStyle(.plain)
                .help("Forward one frame (→)")

            Text(timecode(playback.time))
                .frame(width: 74, alignment: .trailing)
            Slider(value: Binding(get: { playback.time }, set: { playback.seek(to: $0) }),
                   in: 0...max(playback.duration, 0.001))
                .controlSize(.small)
            Text(timecode(playback.duration))
                .frame(width: 74, alignment: .leading)
                .foregroundStyle(.tertiary)
        }
        .font(.system(size: 11, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.bar)
        .disabled(playback.duration == 0)
    }

    /// Minutes, seconds and frames: what an editor shows.
    private func timecode(_ seconds: Double) -> String {
        let fps = playback.frameRate > 0 ? playback.frameRate : 25
        let total = Int((max(seconds, 0) * fps).rounded(.down))
        let perSecond = Int(fps.rounded())
        let frames = total % max(perSecond, 1)
        let whole = total / max(perSecond, 1)
        return String(format: "%02d:%02d:%02d", whole / 60, whole % 60, frames)
    }
}
