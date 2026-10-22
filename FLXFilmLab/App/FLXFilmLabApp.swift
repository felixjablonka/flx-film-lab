import SwiftUI
import AppKit

@main
struct FLXFilmLabApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    // Both halves' documents live as long as the app, so switching modes
    // keeps the open image and the open clip.
    @StateObject private var stills = DocumentModel()
    @StateObject private var motion = ClipModel()
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

/// The window: one half at a time, with the switch between them in the
/// toolbar's centre. Each half brings its own toolbar items.
struct RootView: View {
    @ObservedObject var stills: DocumentModel
    @ObservedObject var motion: ClipModel
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
