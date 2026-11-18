import Foundation

// How films and papers are chosen for a picture without the photographer
// choosing them (Settings, General): what a picture with no settings of its
// own opens with, and how the paper follows a change of film.

/// What a picture with no settings of its own opens with, in Stills: the look
/// of the picture on screen, or the default film and paper. The first picture
/// after launch, with none on screen, takes the defaults either way.
enum NewPictures: String, CaseIterable, Identifiable {
    /// The film, paper and the rest of the look of the picture on screen, its
    /// exposure, tone and colour fresh (`Recipe.carryingLook`).
    case carryLook = "carry"
    /// The default film and paper, everything else at its default.
    case defaultStocks = "default"

    var id: String { rawValue }
    static let key = "newPictures"
    static let filmKey = "defaultFilm"
    static let paperKey = "defaultPaper"

    static func current(_ defaults: UserDefaults) -> NewPictures {
        NewPictures(rawValue: defaults.string(forKey: key) ?? "") ?? .carryLook
    }

    /// The default film: the one set, if it is still a film, else Portra 400.
    static func film(_ defaults: UserDefaults, library: PackLibrary = .shared) -> String {
        if let film = defaults.string(forKey: filmKey), library.filmStocks.contains(where: { $0.stock == film }) {
            return film
        }
        return Recipe().filmStock
    }

    /// The default paper: the one set, if it is still a paper, else the
    /// default film's own.
    static func paper(_ defaults: UserDefaults, library: PackLibrary = .shared) -> String {
        if let paper = defaults.string(forKey: paperKey), library.printStocks.contains(where: { $0.stock == paper }) {
            return paper
        }
        return ownPaper(of: film(defaults, library: library), library: library) ?? Recipe().printStock
    }

    /// The paper `film` is printed on by reference, if it is one of the papers.
    static func ownPaper(of film: String, library: PackLibrary = .shared) -> String? {
        guard let target = (try? library.pack(film))?.targetPrint,
              library.printStocks.contains(where: { $0.stock == target }) else { return nil }
        return target
    }

    /// The recipe a picture starts afresh with: the default film loaded as
    /// the Film menu loads one, on the default paper, everything else at its
    /// default.
    static func starting(_ defaults: UserDefaults, library: PackLibrary = .shared) -> Recipe {
        var r = Recipe()
        let stock = film(defaults, library: library)
        if stock != r.filmStock { r = r.loadingFilm(stock, papers: .followsFilm, library: library) }
        r.grade = r.defaultGrade
        r.printStock = paper(defaults, library: library)
        return r
    }
}

/// How the paper follows a change of film, in Stills and Motion.
enum PaperRule: String, CaseIterable, Identifiable {
    /// Each film brings the paper it is printed on by reference, and a
    /// motion picture film its own print film.
    case followsFilm = "film"
    /// The paper stays while the film changes within its kind (`FilmKind`);
    /// another kind brings back the paper last used with it, or the first
    /// time the new film's own.
    case staysWithKind = "kind"

    var id: String { rawValue }
    static let key = "paperRule"

    static func current(_ defaults: UserDefaults) -> PaperRule {
        PaperRule(rawValue: defaults.string(forKey: key) ?? "") ?? .staysWithKind
    }
}

/// The kinds of film a paper is chosen for, each printed on its own kind: a
/// colour negative on colour paper, a motion picture negative on print film,
/// a black-and-white one on silver paper, or silver print film. Slides and
/// instant prints are seen as they are, and have none.
enum FilmKind: String, CaseIterable {
    case colour, colourMotionPicture, blackAndWhite, blackAndWhiteMotionPicture

    init?(_ film: ProfilePack) {
        if film.isPositive || film.isInstantPrint { return nil }
        switch (film.isMonochrome, film.isMotionPicture) {
        case (false, false): self = .colour
        case (false, true): self = .colourMotionPicture
        case (true, false): self = .blackAndWhite
        case (true, true): self = .blackAndWhiteMotionPicture
        }
    }

    init?(stock: String, library: PackLibrary = .shared) {
        guard let film = try? library.pack(stock) else { return nil }
        self.init(film)
    }
}

/// The paper last used with each kind of film (`PaperRule.staysWithKind`),
/// kept between launches and shared by Stills and Motion: the paper a film
/// was on when another film replaced it, or the one picked for it.
enum PaperMemory {
    static let key = "papersByKind"

    static func papers(_ defaults: UserDefaults) -> [FilmKind: String] {
        let stored = defaults.dictionary(forKey: key) as? [String: String] ?? [:]
        var papers: [FilmKind: String] = [:]
        for (name, paper) in stored {
            if let kind = FilmKind(rawValue: name) { papers[kind] = paper }
        }
        return papers
    }

    /// `paper` as the one last used with `film`'s kind.
    static func remember(_ paper: String, with film: String, _ defaults: UserDefaults) {
        guard let kind = FilmKind(stock: film) else { return }
        var stored = defaults.dictionary(forKey: key) as? [String: String] ?? [:]
        guard stored[kind.rawValue] != paper else { return }
        stored[kind.rawValue] = paper
        defaults.set(stored, forKey: key)
    }
}

extension Recipe {
    /// This recipe with another film picked, as the Film menu picks one in
    /// Stills and Motion: the paper it was on kept as the one last used with
    /// the old film's kind, and the paper following as Settings say
    /// (`PaperRule`).
    func pickingFilm(_ stock: String, _ defaults: UserDefaults) -> Recipe {
        PaperMemory.remember(printStock, with: filmStock, defaults)
        return loadingFilm(stock, papers: PaperRule.current(defaults), remembered: PaperMemory.papers(defaults))
    }
}
