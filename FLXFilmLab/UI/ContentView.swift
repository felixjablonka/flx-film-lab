import SwiftUI
import UniformTypeIdentifiers

/// What surrounds the image on the canvas.
///
/// Brightness and colour are judged relative to their surround, not in
/// isolation — the same print reads dark against white and washed against
/// black. Being able to flip between the three standard surrounds is how you
/// tell a genuinely heavy print from one that only looks heavy.
///
/// This is a viewing preference. It is deliberately not part of `Recipe`, so it
/// never lands in a sidecar and can never change what is rendered.
enum CanvasBackground: String, CaseIterable, Identifiable {
    case dark = "Dark"
    case middleGrey = "Middle grey"
    case white = "White"

    var id: String { rawValue }

    var color: Color {
        switch self {
        // Not pure black: an absolute black surround makes everything in front
        // of it look milky.
        case .dark: return Color(white: 0.07)
        // L* 50 — the perceptual middle, and the neutral a print is normally
        // assessed against.
        case .middleGrey: return Color(white: 0.4663)
        case .white: return Color(white: 1.0)
        }
    }

    var isLight: Bool { self == .white }

    var symbol: String {
        switch self {
        case .dark: return "circle.fill"
        case .middleGrey: return "circle.lefthalf.filled"
        case .white: return "circle"
        }
    }

    var next: CanvasBackground {
        let all = CanvasBackground.allCases
        return all[(all.firstIndex(of: self)! + 1) % all.count]
    }
}

struct ContentView: View {
    @ObservedObject var model: DocumentModel
    @State private var selectedMask: UUID?
    @State private var isImporting = false
    @State private var isExporting = false
    @State private var exportFormat: Exporter.Format = .tiff16
    @State private var isNamingPreset = false
    @State private var presetName = ""
    @AppStorage("canvasBackground") private var backgroundRaw = CanvasBackground.dark.rawValue

    private var background: CanvasBackground {
        CanvasBackground(rawValue: backgroundRaw) ?? .dark
    }

    var body: some View {
        HStack(spacing: 0) {
            canvas
            Divider()
            InspectorView(model: model, selectedMask: $selectedMask)
        }
        .frame(minWidth: 1080, minHeight: 700)
        .background(Color(nsColor: .windowBackgroundColor))
        .navigationTitle(model.fileURL?.lastPathComponent ?? "FLX Film Lab")
        .toolbar { toolbarContent }
        // A mask selected on one image means nothing on the next.
        .onChange(of: model.fileURL) { selectedMask = nil }
        // A preset file may have been copied in, or taken away, in Finder.
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.reloadPresets()
        }
        .sheet(isPresented: $isNamingPreset) {
            PresetNameSheet(name: $presetName, taken: Set(model.presets.map(\.name))) { save in
                if save { model.savePreset(named: presetName) }
                isNamingPreset = false
            }
        }
        .fileImporter(isPresented: $isImporting,
                      allowedContentTypes: [.rawImage, .image, UTType(filenameExtension: "dng") ?? .image],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first {
                _ = url.startAccessingSecurityScopedResource()
                model.open(url: url)
            }
        }
        .fileExporter(isPresented: $isExporting,
                      document: PlaceholderDocument(),
                      contentType: exportFormat.utType,
                      defaultFilename: exportFilename) { result in
            if case .success(let url) = result {
                Task { await model.export(to: url, format: exportFormat) }
            }
        }
        .background {
            // Invisible shortcut host: ⌘B cycles the surround, ⌘[ and ⌘] turn
            // the picture a quarter.
            Group {
                Button("") { backgroundRaw = background.next.rawValue }
                    .keyboardShortcut("b", modifiers: .command)
                Button("") { model.turn(clockwise: false) }
                    .keyboardShortcut("[", modifiers: .command)
                Button("") { model.turn(clockwise: true) }
                    .keyboardShortcut("]", modifiers: .command)
            }
            .opacity(0)
        }
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                Task { @MainActor in model.open(url: url) }
            }
            return true
        }
    }

    private var exportFilename: String {
        let stem = model.fileURL?.deletingPathExtension().lastPathComponent ?? "FLX Film Lab"
        let film = model.filmPack?.name.replacingOccurrences(of: " ", with: "") ?? "film"
        return "\(stem)_\(film)"
    }

    // MARK: Canvas

    private var canvas: some View {
        ZStack {
            background.color
                .animation(.easeInOut(duration: 0.18), value: backgroundRaw)

            if let image = model.displayImage {
                GeometryReader { geo in
                    let fitted = fit(imageSize: CGSize(width: image.width, height: image.height),
                                     into: geo.size)
                    ZStack {
                        Image(decorative: image, scale: 1, orientation: .up)
                            .resizable()
                            .interpolation(.high)
                            .frame(width: fitted.width, height: fitted.height)
                            // Lighter contact shadow on a light surround, so the
                            // frame reads as an edge rather than a vignette.
                            .shadow(color: .black.opacity(background.isLight ? 0.22 : 0.5),
                                    radius: background.isLight ? 10 : 18, y: 5)

                        if model.isCropping {
                            CropOverlay(model: model, size: fitted, onLightBackground: background.isLight)
                        } else {
                            MaskOverlay(masks: $model.recipe.masks, gradients: $model.recipe.gradients,
                                        selected: $selectedMask,
                                        frame: CGRect(origin: .zero, size: fitted),
                                        map: model.displayMap,
                                        onLightBackground: background.isLight)
                                .frame(width: fitted.width, height: fitted.height)
                        }
                    }
                    .frame(width: geo.size.width, height: geo.size.height)
                }
                .padding(28)
                .overlay(alignment: .top) { if model.isCropping { cropBar } }
            } else {
                VStack(spacing: 10) {
                    Image(systemName: "camera.aperture")
                        .font(.system(size: 42, weight: .ultraLight))
                        .foregroundStyle(.tertiary)
                    Text("Drop a RAW file here")
                        .font(.system(size: 14, weight: .medium)).foregroundStyle(.secondary)
                    Text("Nikon NEF, Leica DNG, and the other formats macOS reads")
                        .font(.system(size: 11)).foregroundStyle(.tertiary)
                    Button("Open…") { isImporting = true }
                        .buttonStyle(.borderedProminent).controlSize(.small).padding(.top, 6)
                }
                .colorScheme(background.isLight ? .light : .dark)
            }

            if let error = model.errorMessage {
                VStack {
                    Spacer()
                    Text(error)
                        .font(.system(size: 11))
                        .padding(.horizontal, 12).padding(.vertical, 7)
                        .background(.red.opacity(0.85), in: Capsule())
                        .foregroundStyle(.white)
                        .padding(.bottom, 14)
                }
            }
        }
        .overlay(alignment: .bottom) { statusBar }
    }

    /// While cropping: what the drags do, and the way out.
    private var cropBar: some View {
        HStack(spacing: 10) {
            Text(model.isLevelling
                 ? "Draw a line along something that should be level or upright"
                 : "Drag a corner or an edge · drag inside to move")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Toggle(isOn: $model.isLevelling) { Label("Level", systemImage: "level") }
                .toggleStyle(.button).controlSize(.small)
                .help("Draw a line on the picture along a horizon, or along an upright, and the picture is levelled to it.")
            Button("Cancel") { model.endCropping(keep: false) }
                .keyboardShortcut(.cancelAction).controlSize(.small)
            Button("Done") { model.endCropping(keep: true) }
                .keyboardShortcut(.defaultAction).controlSize(.small)
        }
        .padding(.horizontal, 12).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(background.isLight ? .light : .dark)
        .padding(.top, 8)
    }

    private var statusBar: some View {
        HStack(spacing: 10) {
            Text(model.status)
            if model.lastRenderMilliseconds > 0 {
                Text(String(format: "%.0f ms", model.lastRenderMilliseconds))
                    .foregroundStyle(.tertiary)
            }
            if model.isDecoding {
                Text("decoding…").foregroundStyle(.orange.opacity(0.9))
            }
            if model.isRendering || model.isDecoding {
                ProgressView().controlSize(.small).scaleEffect(0.55).frame(width: 12, height: 12)
            }
        }
        .font(.system(size: 10, design: .monospaced))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(.ultraThinMaterial, in: Capsule())
        .colorScheme(background.isLight ? .light : .dark)
        .padding(.bottom, 10)
    }

    private func fit(imageSize: CGSize, into container: CGSize) -> CGSize {
        guard imageSize.width > 0, imageSize.height > 0 else { return .zero }
        let scale = min(container.width / imageSize.width, container.height / imageSize.height)
        return CGSize(width: imageSize.width * scale, height: imageSize.height * scale)
    }

    // MARK: Toolbar

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
            Button { isImporting = true } label: { Label("Open", systemImage: "folder") }
                .help("Open a RAW file")
        }
        ToolbarItem(placement: .navigation) {
            Picker("", selection: $backgroundRaw) {
                ForEach(CanvasBackground.allCases) { option in
                    Image(systemName: option.symbol).tag(option.rawValue)
                }
            }
            .pickerStyle(.segmented).labelsHidden().frame(width: 108)
            .help("Surround: dark, middle grey or white. Brightness and colour are judged against their surround — the same print reads heavy against white and washed against black. ⌘B cycles.")
        }
        // One group, so the presets always sit with Copy and Paste.
        ToolbarItemGroup {
            ControlGroup {
                Button { model.copySettings() } label: { Label("Copy Settings", systemImage: "doc.on.doc") }
                    .keyboardShortcut("c", modifiers: [.command, .shift])
                    .disabled(!model.canCopySettings)
                    .help("Copy this image's settings to paste onto another (⇧⌘C): film, paper, filter, every slider and auto, and white balance.")
                Button { model.pasteSettings() } label: { Label("Paste Settings", systemImage: "doc.on.clipboard") }
                    .keyboardShortcut("v", modifiers: [.command, .shift])
                    .disabled(!model.canPasteSettings)
                    .help(model.copiedSettings.map {
                        "Paste the settings copied from \($0.source) (⇧⌘V). This image keeps its own masks, gradients and crop."
                    } ?? "Copy an image's settings first (⇧⌘C), then paste them onto another.")
            }
            presetsMenu
        }
        ToolbarItem {
            Toggle(isOn: $model.isCropping) { Label("Crop", systemImage: "crop") }
                .keyboardShortcut("r", modifiers: .command)
                .disabled(!model.hasImage)
                .help("Crop, turn and level the print (⌘R): the whole frame shows with the crop on it. Return keeps the crop, Escape goes back.")
        }
        ToolbarItem {
            ToolbarChoice(selection: $exportFormat)
                .help("The format Export writes.")
        }
        ToolbarItem {
            Button { isExporting = true } label: { Label("Export", systemImage: "square.and.arrow.up") }
                .disabled(!model.canExport)
                .help("Render at full resolution and write to disk. Waits until the image on screen has finished loading.")
        }
    }

    /// Saved settings: choose one to put it on the image on screen.
    private var presetsMenu: some View {
        Menu {
            ForEach(model.presets) { preset in
                Button(preset.name) { model.applyPreset(preset) }
                    .disabled(!model.canCopySettings)
            }
            if model.presets.isEmpty {
                Text("No presets yet")
            }
            Divider()
            Button("Save Settings as Preset…") {
                presetName = model.filmPack?.name ?? ""
                isNamingPreset = true
            }
            .disabled(!model.canCopySettings)
            if !model.presets.isEmpty {
                Menu("Delete Preset") {
                    ForEach(model.presets) { preset in
                        Button(preset.name) { model.deletePreset(preset) }
                    }
                }
            }
            Button("Show Presets in Finder") {
                if let folder = model.presetsFolder() { NSWorkspace.shared.open(folder) }
            }
        } label: {
            Label("Presets", systemImage: "film.stack")
        }
        .help("Presets: settings saved by name, to put on any image later, in this session or another. A preset holds what Copy Settings copies; the image it is put on keeps its own masks, gradients and crop.")
    }
}

/// The name for a new preset, and whether it replaces one, as it is typed.
private struct PresetNameSheet: View {
    @Binding var name: String
    /// The presets' names.
    let taken: Set<String>
    let done: (_ save: Bool) -> Void

    private var trimmed: String { name.trimmingCharacters(in: .whitespacesAndNewlines) }
    private var isEmpty: Bool { trimmed.isEmpty }
    private var replaces: Bool { taken.contains(trimmed) }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Save Settings as Preset").font(.headline)
            Text("Film, paper, print or light table, camera filter, every slider and auto, and white balance, to put on any image later. Masks, gradients and the crop stay with their image.")
                .font(.system(size: 11)).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            TextField("Name", text: $name)
                .textFieldStyle(.roundedBorder)
                .onSubmit { if !isEmpty { done(true) } }
            Text(replaces ? "Replaces the preset of that name." : " ")
                .font(.system(size: 10)).foregroundStyle(.orange)
            HStack {
                Spacer()
                Button("Cancel") { done(false) }
                    .keyboardShortcut(.cancelAction)
                Button(replaces ? "Replace" : "Save") { done(true) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(isEmpty)
            }
        }
        .padding(20)
        .frame(width: 360)
    }
}

/// `fileExporter` needs a document type; the real write happens in the
/// completion handler at full resolution, so this carries no data.
private struct PlaceholderDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.tiff, .jpeg, .png] }
    init() {}
    init(configuration: ReadConfiguration) throws {}
    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data())
    }
}
