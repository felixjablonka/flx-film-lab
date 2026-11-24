import SwiftUI

/// The app's own settings (⌘,): how it looks and works, the same for every
/// picture and clip. What belongs to a picture, its output colour space among
/// it, stays in the inspector and travels with its settings and presets.
/// Three tabs: General; Film & Paper, what a picture opens with and how the
/// paper follows the film; and Films, which the Film menu lists.
struct SettingsView: View {
    @ObservedObject var stills: DocumentModel
    let library: ImageLibrary

    var body: some View {
        TabView {
            GeneralSettings(stills: stills, library: library)
                .tabItem { Label("General", systemImage: "gearshape") }
            FilmAndPaperSettings()
                .tabItem { Label("Film & Paper", systemImage: "camera.filters") }
            FilmSettings()
                .tabItem { Label("Films", systemImage: "film.stack") }
        }
        .preferredColorScheme(.dark)
    }
}

private func note(_ text: String) -> some View {
    Text(text)
        .font(.system(size: 11))
        .foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
}

private struct GeneralSettings: View {
    @ObservedObject var stills: DocumentModel
    let library: ImageLibrary
    @AppStorage(PanelScale.key) private var textSize = PanelScale.standard.rawValue
    @AppStorage("vocabulary") private var vocabularyRaw = Vocabulary.photographic.rawValue
    @AppStorage(ImageLibrary.sidecarsKey) private var savesSidecars = false
    @AppStorage(Exporter.ExportComment.editedWithKey) private var commentEditedWith = true
    @AppStorage(Exporter.ExportComment.filmKey) private var commentFilm = true
    @AppStorage(Exporter.ExportComment.paperKey) private var commentPaper = true
    @AppStorage(Exporter.ExportComment.filterKey) private var commentFilter = true

    /// The comment the ticks would write, on Tri-X through a red filter on
    /// Multigrade.
    private var commentExample: String {
        var recipe = Recipe().loadingFilm("kodak_tri_x_400")
        recipe.lensFilter = "red"
        let comment = Exporter.ExportComment(editedWith: commentEditedWith, film: commentFilm, paper: commentPaper,
                                             filter: commentFilter)
        let said = Exporter.Provenance(recipe: recipe, comment: comment).description
        return said.isEmpty ? "Nothing is written there." : "For example: \(said)"
    }

    var body: some View {
        Form {
            SwiftUI.Section("Interface") {
                Picker("Text size", selection: $textSize) {
                    ForEach(PanelScale.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }
                .pickerStyle(.segmented)
                note("The inspector and the sidebar, larger as a whole: their text, controls and width. The toolbar keeps the system's size.")
                Picker("Control names", selection: $vocabularyRaw) {
                    ForEach(Vocabulary.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }
                note("The Stills sliders. Photographic uses the names Capture One and Lightroom use, where positive always means brighter. Darkroom shows the underlying print controls. Either way the tooltip names the other.")
            }
            SwiftUI.Section("Preview") {
                Picker("Preview size", selection: $stills.proxyMaxDimension) {
                    ForEach(DocumentModel.previewSizes, id: \.self) { Text(verbatim: "\($0) px").tag($0) }
                }
                note("The Stills preview's long side. Larger is sharper on screen and slower to redraw: on an M4 Pro a print control redraws 2560 px in about 16 ms and 4096 px in about 37. A file smaller than this is shown at its own size, never enlarged. Export always prints at full size, whatever the preview's.")
            }
            SwiftUI.Section("Library") {
                Toggle("Save settings beside images", isOn: $savesSidecars)
                note("Each picture's settings, or clip's look, and its flag also go into a small file beside it, DSC_0871.NEF.flx, which travels with the folder to another drive, another Mac or the Windows app. They are always kept in the app's library too; of the two, the newer counts.")
            }
            SwiftUI.Section("Comment in Exported Files") {
                Toggle("Edited with FLX Film Lab", isOn: $commentEditedWith)
                Toggle("Film profile", isOn: $commentFilm)
                Toggle("Paper or print film", isOn: $commentPaper)
                Toggle("Camera filter", isOn: $commentFilter)
                note("What an exported picture's EXIF comment says, and its description where the camera left none, and an exported clip's description. A slide and an instant print, seen as they are, name no paper. With none ticked, nothing is written there; a camera's own comment and description stay either way. \(commentExample)")
            }
        }
        .formStyle(.grouped)
        .frame(width: 500)
        .fixedSize(horizontal: false, vertical: true)
        // turned on, the records already kept are written beside their files too
        .onChange(of: savesSidecars) { _, on in
            if on { library.savesSidecars = true }
        }
    }
}

/// What a picture with no settings of its own opens with, in Stills, and how
/// the paper follows a change of film, in Stills and Motion
/// (`NewPictures`, `PaperRule`).
struct FilmAndPaperSettings: View {
    @AppStorage(NewPictures.key) private var newPictures = NewPictures.carryLook.rawValue
    @AppStorage(NewPictures.filmKey) private var defaultFilmStored = ""
    @AppStorage(NewPictures.paperKey) private var defaultPaperStored = ""
    @AppStorage(PaperRule.key) private var paperRule = PaperRule.staysWithKind.rawValue
    @AppStorage(FilmMenu.hiddenKey) private var hiddenFilms = ""

    var body: some View {
        Form {
            SwiftUI.Section("New Pictures") {
                Picker("Open with", selection: $newPictures) {
                    Text("The look of the picture before").tag(NewPictures.carryLook.rawValue)
                    Text("The default film and paper").tag(NewPictures.defaultStocks.rawValue)
                }
                .pickerStyle(.radioGroup)
                note("For a picture with no settings of its own. The look of the picture before is its film, paper and film character, with the exposure, tone and colour fresh; the first picture after launch has none before it and opens with the default film and paper. A picture with settings of its own always opens with them.")
                Picker("Default film", selection: defaultFilm) {
                    stocks(FilmMenu.shown(PackLibrary.shared.filmStocks, hidden: hiddenFilms,
                                          inUse: defaultFilm.wrappedValue), films: true)
                }
                if printsOnPaper(defaultFilm.wrappedValue) {
                    Picker("Default paper", selection: defaultPaper) {
                        stocks(PackLibrary.shared.printStocks, films: false)
                    }
                } else {
                    note("A slide is seen on the light table, and an instant print is the print: neither is printed on paper.")
                }
            }
            SwiftUI.Section("Changing the Film") {
                Picker("The paper", selection: $paperRule) {
                    Text("Follows the film").tag(PaperRule.followsFilm.rawValue)
                    Text("Stays with the kind of film").tag(PaperRule.staysWithKind.rawValue)
                }
                .pickerStyle(.radioGroup)
                note("Follows the film: each film brings the paper it is printed on by reference, and a motion picture film its own print film. Stays with the kind of film: the paper stays while you change from one colour negative to another, as do the print film of motion picture negatives and the paper of black-and-white ones; a film of another kind brings back the paper last used with that kind, the first time its own. In Stills and Motion.")
            }
        }
        .formStyle(.grouped)
        .frame(width: 500)
        .fixedSize(horizontal: false, vertical: true)
    }

    /// The default film; a new one brings its own paper, which the paper
    /// picker can then change.
    private var defaultFilm: Binding<String> {
        Binding(get: { NewPictures.film(.standard) },
                set: { film in
                    defaultFilmStored = film
                    if let own = NewPictures.ownPaper(of: film) { defaultPaperStored = own }
                })
    }

    private var defaultPaper: Binding<String> {
        Binding(get: { NewPictures.paper(.standard) }, set: { defaultPaperStored = $0 })
    }

    /// Whether `film` is printed on paper when a picture opens with it: not
    /// a slide, seen on the light table, nor an instant print, the print itself.
    private func printsOnPaper(_ film: String) -> Bool {
        guard let pack = try? PackLibrary.shared.pack(film) else { return true }
        return !pack.isViewedDirectly(scanFilm: nil)
    }

    /// `entries` in the stock pickers' sections.
    @ViewBuilder
    private func stocks(_ entries: [PackIndexEntry], films: Bool) -> some View {
        ForEach(PackIndexEntry.groups(entries, films: films), id: \.title) { group in
            SwiftUI.Section(group.title) {
                ForEach(group.entries) { entry in
                    Text(entry.name).tag(entry.stock)
                }
            }
        }
    }
}

/// Which films the Film menu lists, in Stills and Motion: each film ticked or
/// not, in the menu's own groups and order. A film left out still works: a
/// picture, clip or preset that uses it opens with it, and it stays in the
/// menu while in use (`FilmMenu.shown`).
struct FilmSettings: View {
    @AppStorage(FilmMenu.hiddenKey) private var hiddenStored = ""
    private let groups = PackIndexEntry.groups(PackLibrary.shared.filmStocks, films: true)

    private var hidden: Set<String> { FilmMenu.hidden(hiddenStored) }

    var body: some View {
        let all = groups.flatMap(\.entries)
        let shown = all.filter { !hidden.contains($0.stock) }.count
        Form {
            SwiftUI.Section {
                note("The films the Film menu lists, in Stills and Motion. A film left out keeps working: a picture, clip or preset that uses it opens with it, and the menu shows it while it is in use.")
                HStack {
                    Text("\(shown) of \(all.count) films shown").foregroundStyle(.secondary)
                    Spacer()
                    Button("Show All") { hiddenStored = "" }
                        .disabled(hidden.isEmpty)
                }
            }
            ForEach(groups, id: \.title) { group in
                SwiftUI.Section {
                    // two columns, in the menu's order down each
                    let half = (group.entries.count + 1) / 2
                    HStack(alignment: .top, spacing: 16) {
                        ForEach([Array(group.entries.prefix(half)), Array(group.entries.dropFirst(half))], id: \.first?.stock) {
                            column in
                            VStack(alignment: .leading, spacing: 7) {
                                ForEach(column) { entry in
                                    Toggle(entry.name, isOn: isShown(entry.stock))
                                        .toggleStyle(.checkbox)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(.vertical, 2)
                } header: {
                    HStack {
                        Text(group.title)
                        Spacer()
                        let every = group.entries.allSatisfy { !hidden.contains($0.stock) }
                        Button(every ? "Hide All" : "Show All") { setShown(group.entries, !every) }
                            .buttonStyle(.link)
                            .font(.system(size: 11))
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 600, height: 600)
    }

    private func isShown(_ stock: String) -> Binding<Bool> {
        Binding(get: { !hidden.contains(stock) },
                set: { show in
                    var set = hidden
                    if show { set.remove(stock) } else { set.insert(stock) }
                    hiddenStored = FilmMenu.stored(set)
                })
    }

    private func setShown(_ entries: [PackIndexEntry], _ show: Bool) {
        var set = hidden
        for entry in entries {
            if show { set.remove(entry.stock) } else { set.insert(entry.stock) }
        }
        hiddenStored = FilmMenu.stored(set)
    }
}
