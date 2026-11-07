import SwiftUI
import AppKit

@main
struct FLXFilmLabApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    // Both halves' documents live as long as the app, so switching modes
    // keeps the open image and the open clip. They are not state of the app's
    // own: as that, every change of theirs, a frame or a slider tick, made
    // the app rebuild its window, toolbar and menus.
    private var stills: DocumentModel { Documents.stills }
    private var motion: ClipModel { Documents.motion }
    @AppStorage("mode") private var modeRaw = AppMode.stills.rawValue

    init() { Self.takeOverSettings() }

    /// Settings saved before the app took its name live under its previous
    /// identifier. The first launch under this one takes them over — the mode,
    /// which autos are on, the window and where the Open panel was — along with
    /// the clip look the standalone Motion app kept, if there is no newer one.
    /// The old copies are left as they were.
    private static func takeOverSettings() {
        let defaults = UserDefaults.standard
        guard let current = Bundle.main.bundleIdentifier,
              defaults.persistentDomain(forName: current)?.isEmpty ?? true else { return }
        var settings = defaults.persistentDomain(forName: "studio.agx.AgXStudio") ?? [:]
        if settings[ClipModel.lookDefaultsKey] == nil,
           let look = defaults.persistentDomain(forName: "studio.agx.AgXMotion")?[ClipModel.lookDefaultsKey] {
            settings[ClipModel.lookDefaultsKey] = look
        }
        guard !settings.isEmpty else { return }
        defaults.setPersistentDomain(settings, forName: current)
    }

    private var mode: Binding<AppMode> {
        Binding(get: { AppMode(rawValue: modeRaw) ?? .stills }, set: { modeRaw = $0.rawValue })
    }

    var body: some Scene {
        Window("FLX Film Lab", id: "main") {
            RootView(stills: stills, motion: motion, mode: mode)
                .preferredColorScheme(.dark)
                .onAppear { appDelegate.attach(stills: stills, motion: motion, mode: mode) }
        }
        .windowToolbarStyle(.unified(showsTitle: true))
        .commands {
            CommandGroup(replacing: .newItem) {}
            UndoCommands(stills: stills.history, motion: motion.history, mode: mode.wrappedValue)
            CommandGroup(after: .toolbar) {
                Button("Stills") { mode.wrappedValue = .stills }
                    .keyboardShortcut("1", modifiers: .command)
                Button("Motion") { mode.wrappedValue = .motion }
                    .keyboardShortcut("2", modifiers: .command)
                Divider()
            }
            CommandGroup(after: .appInfo) {
                Link("agx-emulsion / spektrafilm", destination: URL(string: "https://github.com/andreavolpato/spektrafilm")!)
                Link("NegPy", destination: URL(string: "https://github.com/marcinz606/NegPy")!)
            }
        }
    }
}

/// Undo and Redo in the Edit menu (⌘Z and ⇧⌘Z), for the half on screen: each
/// names what it would take back or do again. A text field being typed in
/// keeps its own, as everywhere on the Mac, while it has typing to undo, and
/// so does a sheet.
struct UndoCommands: Commands {
    @ObservedObject var stills: EditHistory<DocumentModel.ImageEdits>
    @ObservedObject var motion: EditHistory<MotionLook>
    let mode: AppMode

    var body: some Commands {
        CommandGroup(replacing: .undoRedo) {
            let undoName = mode == .stills ? stills.undoName : motion.undoName
            let redoName = mode == .stills ? stills.redoName : motion.redoName
            Button(undoName.map { "Undo \($0)" } ?? "Undo") {
                if Self.textTakes(\.canUndo) {
                    NSApp.sendAction(Selector(("undo:")), to: nil, from: nil)
                } else if mode == .stills { stills.undo() } else { motion.undo() }
            }
            .keyboardShortcut("z", modifiers: .command)
            .disabled(undoName == nil)
            Button(redoName.map { "Redo \($0)" } ?? "Redo") {
                if Self.textTakes(\.canRedo) {
                    NSApp.sendAction(Selector(("redo:")), to: nil, from: nil)
                } else if mode == .stills { stills.redo() } else { motion.redo() }
            }
            .keyboardShortcut("z", modifiers: [.command, .shift])
            .disabled(redoName == nil)
        }
    }

    /// The key window's own undo has it: a sheet's, or a text field's while
    /// it has typing to undo (or redo).
    @MainActor
    private static func textTakes(_ can: KeyPath<UndoManager, Bool>) -> Bool {
        guard let window = NSApp.keyWindow else { return false }
        if window.sheetParent != nil { return true }
        guard let text = window.firstResponder as? NSText else { return false }
        return text.undoManager?[keyPath: can] ?? false
    }
}

/// The two halves' documents, made once, on first use.
@MainActor
private enum Documents {
    static let stills = DocumentModel()
    static let motion = ClipModel()
}

/// The window: one half at a time, with the switch between them in the
/// toolbar's centre. Each half brings its own toolbar items.
struct RootView: View {
    // Each half watches its own document; the window around them need not.
    let stills: DocumentModel
    let motion: ClipModel
    @Binding var mode: AppMode

    var body: some View {
        Group {
            switch mode {
            case .stills: ContentView(model: stills)
            case .motion: MotionContentView(model: motion)
            }
        }
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("Mode", selection: $mode) {
                    ForEach(AppMode.allCases) { m in
                        Label(m.rawValue, systemImage: m.symbol).tag(m)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .help("Stills: a RAW photo through film and paper. Motion: a log clip, with playback, clip export and LUTs. ⌘1 and ⌘2 switch.")
            }
        }
        // A clip keeps playing behind the stills half otherwise.
        .onChange(of: mode) { if mode != .motion { motion.playback.pause() } }
    }
}

/// Files opened from Finder ("Open With", or dropped on the Dock icon): a
/// movie opens in motion, anything else in stills, and the window switches
/// to that half.
final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor private var stills: DocumentModel?
    @MainActor private var motion: ClipModel?
    @MainActor private var mode: Binding<AppMode>?
    @MainActor private var pending: [URL] = []

    @MainActor
    func attach(stills: DocumentModel, motion: ClipModel, mode: Binding<AppMode>) {
        self.stills = stills
        self.motion = motion
        self.mode = mode
        pending.forEach(open)
        pending = []
    }

    @MainActor
    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first else { return }
        if stills == nil { pending = [url] } else { open(url) }
    }

    @MainActor
    private func open(_ url: URL) {
        let target = AppMode.opening(url)
        mode?.wrappedValue = target
        switch target {
        case .stills: stills?.open(url: url)
        case .motion: motion?.open(url)
        }
    }
}
