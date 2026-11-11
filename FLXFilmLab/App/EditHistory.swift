import Foundation
import AppKit

/// Undo and redo (⌘Z and ⇧⌘Z) for the edits of the picture or the clip on
/// screen.
///
/// A step is one gesture, however many values it passes through: a slider
/// dragged from press to release, a handle, a crop edge or a curve point
/// dragged, a click on a switch, a choice from a menu, a reset, a paste or a
/// preset. While the mouse button is down, every change joins the step that is
/// open, so a drag is one step. A change made without it, by a click, a key or
/// a menu, joins only those made by the same action. A quick repeat of the same
/// change joins a step made with the button down: the second click of the
/// double-click that resets a slider, so one ⌘Z takes the slider back to where
/// it stood before both clicks. Two quick turns, or two masks added, stay two.
///
/// Each step holds the edits as they were before it and after it, so undoing a
/// step puts back exactly what it started from, whatever it touched.
@MainActor
final class EditHistory<State: Equatable>: ObservableObject {
    struct Step {
        var name: String
        var before: State
        var after: State
        fileprivate var id = UUID()
    }

    /// The steps that can be undone, oldest first, and the ones undone since,
    /// the most recently undone last.
    struct Stacks {
        var undo: [Step] = []
        var redo: [Step] = []
    }

    @Published private(set) var stacks = Stacks()

    /// What Undo and Redo would take back or do again, as the Edit menu names
    /// it: "Exposure", "Add Mask", "Paste Settings".
    var undoName: String? { stacks.undo.last?.name }
    var redoName: String? { stacks.redo.last?.name }

    /// How many steps a picture or a clip keeps.
    let limit = 200

    /// Whether the mouse button is down: while it is, a step stays open.
    var mouseIsDown: () -> Bool = { NSEvent.pressedMouseButtons & 1 != 0 }
    /// How soon a repeat of the same change has to start to join the step
    /// before it: the double-click interval set in System Settings.
    var joinInterval: TimeInterval = NSEvent.doubleClickInterval

    private let read: () -> State
    private let write: (State) -> Void
    private let describe: (State, State) -> String

    /// The step being made: the edits as they were when it opened, and
    /// whether the mouse button was down then.
    private struct Open {
        var before: State
        var name: String?
        var at: Date
        var pressed: Bool
    }
    private var open: Open?
    /// Whether a step is open, waiting for its action or gesture to end.
    var isStepOpen: Bool { open != nil }
    private var closer: Task<Void, Never>?
    private var recording = true
    private var nextName: String?
    /// The step a quick repeat of the same change may join, and until when:
    /// one made with the mouse button down.
    private var joinable: (id: UUID, until: Date)?

    /// `read` gives the edits as they stand, `write` puts a set back, and
    /// `describe` names the change from one set to another.
    init(read: @escaping () -> State, write: @escaping (State) -> Void,
         describe: @escaping (State, State) -> String) {
        self.read = read
        self.write = write
        self.describe = describe
    }

    /// The edits are changing: `before` gives them as they were. The first
    /// change opens a step; it closes once the action that made it is over,
    /// or, while the mouse button is down, once the button comes up.
    func note(_ before: () -> State) {
        guard recording, open == nil else { return }
        open = Open(before: before(), name: nextName, at: Date(), pressed: mouseIsDown())
        closer = Task { [weak self] in
            // The task first runs once the action that made the change has
            // finished, so everything that action changed is in the step.
            while self?.mouseIsDown() == true {
                while self?.mouseIsDown() == true {
                    try? await Task.sleep(for: .milliseconds(30))
                    if Task.isCancelled { return }
                }
                // What the button coming up sets belongs to the gesture: the
                // reset at the end of a double-click.
                try? await Task.sleep(for: .milliseconds(50))
                if Task.isCancelled { return }
            }
            self?.finish()
        }
    }

    /// Close the open step, keeping it if it changed anything.
    func finish() {
        closer?.cancel()
        closer = nil
        guard let open else { return }
        self.open = nil
        let after = read()
        guard after != open.before else { return }
        let name = open.name ?? describe(open.before, after)
        var undo = stacks.undo
        var joined = false
        if open.name == nil, let last = undo.last, let joinable, joinable.id == last.id,
           open.at < joinable.until, last.name == name, last.after == open.before {
            // A quick repeat of the same change belongs to the step before it,
            // and a repeat that undoes that step leaves nothing to undo.
            undo.removeLast()
            if last.before != after { undo.append(Step(name: name, before: last.before, after: after, id: last.id)) }
            joined = true
        } else {
            undo.append(Step(name: name, before: open.before, after: after))
            if undo.count > limit { undo.removeFirst(undo.count - limit) }
        }
        // A named step (a reset, a paste) stands alone, and so does one made
        // by a key or a click on a button.
        joinable = open.name == nil && (open.pressed || joined)
            ? undo.last.map { ($0.id, Date().addingTimeInterval(joinInterval)) } : nil
        stacks = Stacks(undo: undo, redo: [])
    }

    func undo() {
        finish()
        guard let step = stacks.undo.last else { return }
        var s = stacks
        s.undo.removeLast()
        s.redo.append(step)
        stacks = s
        joinable = nil
        unrecorded { write(step.before) }
    }

    func redo() {
        finish()
        guard let step = stacks.redo.last else { return }
        var s = stacks
        s.redo.removeLast()
        s.undo.append(step)
        stacks = s
        joinable = nil
        unrecorded { write(step.after) }
    }

    /// Make whatever `change` changes one step of its own, called `name`:
    /// "Reset Tone", "Paste Settings".
    func named(_ name: String, _ change: () -> Void) {
        finish()
        nextName = name
        change()
        nextName = nil
        finish()
    }

    /// Change the edits without a step: another picture's coming on screen.
    func unrecorded(_ change: () -> Void) {
        let was = recording
        recording = false
        change()
        recording = was
    }

    /// The steps of the picture or clip coming on screen, for those of the one
    /// going, which are given back. A step still open is the one going's.
    func swap(for incoming: Stacks) -> Stacks {
        finish()
        let outgoing = stacks
        stacks = incoming
        joinable = nil
        return outgoing
    }

    /// Where the history stands, to rewind to.
    struct Mark {
        fileprivate let count: Int
        fileprivate let top: UUID?
        fileprivate let redo: [Step]
    }

    func mark() -> Mark {
        finish()
        return Mark(count: stacks.undo.count, top: stacks.undo.last?.id, redo: stacks.redo)
    }

    /// Take the steps since `mark` off as if they had never been taken, and
    /// let what could be redone then be redone again: when every one of them
    /// changed only what `allowed` allows. Putting the edits back is the
    /// caller's. False, with nothing changed, when a step since changed
    /// something else, or the history was undone past `mark`.
    func rewind(to mark: Mark, ifEach allowed: (_ before: State, _ after: State) -> Bool) -> Bool {
        finish()
        let undo = stacks.undo
        guard undo.count >= mark.count, mark.count == 0 || undo[mark.count - 1].id == mark.top,
              undo[mark.count...].allSatisfy({ allowed($0.before, $0.after) }) else { return false }
        stacks = Stacks(undo: Array(undo[..<mark.count]), redo: mark.redo)
        joinable = nil
        return true
    }
}

/// What the Edit menu calls a change of a recipe, in the words of the control
/// that made it: "Undo Exposure", "Redo Add Mask".
enum EditNames {
    /// The recipe's stored settings that differ between `a` and `b`, by name.
    static func changes<T>(_ a: T, _ b: T) -> [String] {
        zip(Mirror(reflecting: a).children, Mirror(reflecting: b).children).compactMap { x, y in
            guard let label = x.label, !same(x.value, y.value) else { return nil }
            return label
        }
    }

    private static func same(_ a: Any, _ b: Any) -> Bool {
        guard let a = a as? any Equatable else { return false }
        return a.isEqual(to: b)
    }

    /// The names of what changed from `a` to `b`, each once.
    static func recipe(_ a: Recipe, _ b: Recipe, vocabulary v: Vocabulary) -> [String] {
        let changed = changes(a, b)
        // A new film can bring its own paper and filter with it.
        if changed.contains("filmStock") { return ["Film"] }
        let set = Set(changed)
        let silver = (try? PackLibrary.shared.pack(b.filmStock))?.isMonochrome ?? false
        return changed.compactMap { field -> String? in
            switch field {
            case "printStock": return "Paper"
            case "exposureEV": return "Exposure"
            // Moving a slider whose auto is on turns the auto off.
            case "autoExposure": return set.contains("exposureEV") ? "Exposure" : "Auto Exposure"
            case "lensFilter": return "Filter"
            case "exposureGradients":
                // The one added, deleted or changed names the step: an oval or a gradient.
                let one = b.gradients.first { !a.gradients.contains($0) } ?? a.gradients.first { !b.gradients.contains($0) }
                return list(a.gradients, b.gradients, one?.isOval == true ? "Oval" : "Gradient")
            case "frameGeometry": return shape(a.geometry, b.geometry)
            case "brightness": return Controls.brightness.name(v)
            case "autoDensity": return set.contains("brightness") ? Controls.brightness.name(v) : "Auto Brightness"
            case "density": return Controls.printExposure.name(v)
            case "grade": return Controls.contrast.name(v)
            case "autoGrade": return set.contains("grade") ? Controls.contrast.name(v) : "Auto Contrast"
            case "shadowGrade": return Controls.shadowContrast.name(v)
            case "highlightGrade": return Controls.highlightContrast.name(v)
            case "shadows": return Controls.shadows.name(v)
            case "highlights": return Controls.highlights.name(v)
            case "toe": return Controls.blacks.name(v)
            case "shoulder": return Controls.whites.name(v)
            case "toeWidth": return Controls.blacksRange.name(v)
            case "shoulderWidth": return Controls.whitesRange.name(v)
            case "snap": return Controls.midtoneContrast.name(v)
            case "gradeTrim", "toeTrim", "shoulderTrim", "snapTrim": return "Colour Crossover"
            case "filterShiftY": return Controls.temperature.name(v)
            case "filterShiftM": return Controls.tint.name(v)
            case "shadowCMY": return "Shadow Colour"
            case "highlightCMY": return "Highlight Colour"
            case "preflashExposure", "preflashY", "preflashM": return Controls.highlightProtection.name(v)
            case "densityCurveGamma": return "Curve Gamma"
            case "couplersEnabled", "couplersAmount": return silver ? "Edge Effects" : "Couplers"
            case "couplersDiffusion": return silver ? "Edge Width" : "Coupler Diffusion"
            case "halationEnabled", "halationAmount": return "Halation"
            case "halationScale": return "Halation Size"
            case "scatterAmount", "scatterScale": return "Scatter"
            case "cameraSharpness": return "Sharpness"
            case "grainEnabled", "grainAmount": return "Grain"
            case "grainScale": return "Grain Size"
            case "glareEnabled", "glareAmount": return "Glare"
            case "masks": return list(a.masks, b.masks, "Mask")
            case "effects": return "Bloom & Glow"
            case "curves": return "Curves"
            case "scanFilm": return "Print on Paper"
            case "normalizeToBase": return "Base White"
            case "outputColorSpace": return "Colour Space"
            case "gamutCompress": return "Gamut Compression"
            case "filmFormat": return "Film Format"
            case "processingTemperature": return "Processing Temperature"
            default: return nil
            }
        }.map(titled)
    }

    /// One name for a step: what changed, or "Changes" when it was several
    /// things at once.
    static func title(_ names: [String]) -> String {
        var unique: [String] = []
        for name in names where !unique.contains(name) { unique.append(name) }
        return unique.count == 1 ? unique[0] : "Changes"
    }

    private static func list<T>(_ a: [T], _ b: [T], _ noun: String) -> String {
        b.count > a.count ? "Add \(noun)" : (b.count < a.count ? "Delete \(noun)" : noun)
    }

    private static func shape(_ a: FrameGeometry, _ b: FrameGeometry) -> String {
        if a.quarterTurns != b.quarterTurns { return "Turn" }
        if a.mirrored != b.mirrored { return "Mirror" }
        if a.straighten != b.straighten { return "Straighten" }
        return "Crop"
    }

    /// A control's name as a menu item puts it: "Print exposure" is
    /// "Print Exposure".
    private static func titled(_ name: String) -> String {
        let minor: Set<String> = ["and", "or", "of", "on", "the", "to", "in", "a"]
        return name.split(separator: " ").enumerated().map { i, word in
            i > 0 && minor.contains(String(word)) ? String(word) : word.prefix(1).uppercased() + word.dropFirst()
        }.joined(separator: " ")
    }
}

private extension Equatable {
    func isEqual(to other: Any) -> Bool { (other as? Self) == self }
}
