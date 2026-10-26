import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct MotionContentView: View {
    @ObservedObject var model: ClipModel

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                canvas
                TransportBar(playback: model.playback)
            }
            Divider()
            MotionInspector(model: model, onExport: chooseExport, onExportLUT: chooseLUTExport)
        }
        .frame(minWidth: 1080, minHeight: 680)
        .background(Color(nsColor: .windowBackgroundColor))
        .navigationTitle(model.clip?.url.lastPathComponent ?? "FLX Film Lab")
        .toolbar { toolbarContent }
        .background { shortcuts }
    }

    // MARK: Canvas

    private var canvas: some View {
        ZStack {
            PreviewView(model: model)

            if model.clip == nil {
                VStack(spacing: 10) {
                    Image(systemName: "film")
                        .font(.system(size: 42, weight: .ultraLight))
                        .foregroundStyle(.tertiary)
                    Text("Drop a log clip here")
                        .font(.system(size: 14, weight: .medium)).foregroundStyle(.secondary)
                    Text("N-Log, V-Log, Apple Log or Apple Log 2, in any movie macOS can play")
                        .font(.system(size: 11)).foregroundStyle(.tertiary)
                    Button("Open…", action: chooseClip)
                        .buttonStyle(.borderedProminent).controlSize(.small).padding(.top, 6)
                }
                .colorScheme(model.surround == .white ? .light : .dark)
            }

            VStack {
                Spacer()
                if let error = model.errorMessage {
                    Text(error)
                        .font(.system(size: 11))
                        .padding(.horizontal, 12).padding(.vertical, 7)
                        .background(.red.opacity(0.85), in: Capsule())
                        .foregroundStyle(.white)
                        .onTapGesture { model.errorMessage = nil }
                        .padding(.bottom, 6)
                }
                if let progress = model.exportProgress {
                    exportBar(progress)
                } else {
                    statusBar
                }
            }
            .padding(.bottom, 10)
        }
    }

    private var statusBar: some View {
        HStack(spacing: 10) {
            Text(model.probeText ?? model.status)
            if model.drawMilliseconds > 0 && model.clip != nil && model.probeText == nil {
                Text(String(format: "%.0f ms", model.drawMilliseconds)).foregroundStyle(.tertiary)
            }
            if model.showLog {
                Text("log footage").foregroundStyle(.orange.opacity(0.9))
            }
            if model.isOpening {
                ProgressView().controlSize(.small).scaleEffect(0.55).frame(width: 12, height: 12)
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(model.surround == .white ? .light : .dark)
    }

    private func exportBar(_ progress: Double) -> some View {
        HStack(spacing: 10) {
            Text(model.status).font(.system(size: 10, design: .monospaced))
            ProgressView(value: progress).frame(width: 180)
            Text(String(format: "%.0f%%", progress * 100)).font(.system(size: 10, design: .monospaced))
            Button("Cancel") { model.cancelExport() }.controlSize(.small)
        }
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(.dark)
    }

    // MARK: Toolbar and keys

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        // Laid out as the Stills toolbar is: the surround beside Open, the
        // export format beside Export.
        ToolbarItem(placement: .navigation) {
            Button(action: chooseClip) { Label("Open", systemImage: "folder") }
                .help("Open a log clip (⌘O)")
        }
        ToolbarItem(placement: .navigation) {
            Picker("", selection: $model.surround) {
                ForEach(Surround.allCases) { Image(systemName: $0.symbol).tag($0) }
            }
            .pickerStyle(.segmented).labelsHidden().frame(width: 108)
            .help("Surround: dark, middle grey or white. ⌘B cycles.")
        }
        ToolbarItem {
            Toggle(isOn: $model.showLog) { Label("Log footage", systemImage: "square.split.2x1") }
                .help("Show the untouched log footage, as QuickTime shows it (\\)")
        }
        ToolbarItem {
            ToolbarChoice(selection: $model.look.export.codec)
                .help("The clip's format: ProRes 422 HQ for editing and grading further, HEVC 10-bit for sharing.")
        }
        ToolbarItem {
            Button(action: chooseExport) { Label("Export", systemImage: "square.and.arrow.up") }
                .disabled(model.clip == nil || model.isExporting)
                .help("Write the graded clip (⌘E)")
        }
    }

    /// Keyboard shortcuts with no menu item of their own.
    private var shortcuts: some View {
        ZStack {
            Button("") { model.playback.togglePlay() }.keyboardShortcut(.space, modifiers: [])
            Button("") { model.playback.step(-1) }.keyboardShortcut(.leftArrow, modifiers: [])
            Button("") { model.playback.step(1) }.keyboardShortcut(.rightArrow, modifiers: [])
            Button("") { model.showLog.toggle() }.keyboardShortcut("\\", modifiers: [])
            Button("") { model.surround = model.surround.next }.keyboardShortcut("b", modifiers: .command)
            Button("", action: chooseClip).keyboardShortcut("o", modifiers: .command)
            Button("", action: chooseExport).keyboardShortcut("e", modifiers: .command)
        }
        .opacity(0)
        .allowsHitTesting(false)
    }

    // MARK: Panels

    private func chooseClip() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.movie]
        panel.allowsMultipleSelection = false
        panel.message = "Choose a log clip"
        if panel.runModal() == .OK, let url = panel.url { model.open(url) }
    }

    private func chooseExport() {
        guard model.clip != nil, !model.isExporting else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.quickTimeMovie]
        panel.nameFieldStringValue = model.suggestedExportName
        panel.directoryURL = model.clip?.url.deletingLastPathComponent()
        if panel.runModal() == .OK, let url = panel.url { model.export(to: url) }
    }

    private func chooseLUTExport() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "cube") ?? .data]
        panel.nameFieldStringValue = "\(model.filmName) on \(model.paperName).cube"
        if panel.runModal() == .OK, let url = panel.url { model.exportLUT(to: url) }
    }
}
