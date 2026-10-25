import SwiftUI

/// Two names for the same control.
///
/// The engine's parameters are darkroom quantities — print exposure in stops,
/// contrast as an ISO range, density offsets, toe and shoulder — because that
/// is what the physics is. But most people arriving at this app have come from
/// Capture One or Lightroom, where the same operations are called Brightness,
/// Contrast, Shadows, Highlights, Blacks and Whites, and where positive always
/// means brighter.
///
/// So the sliders are presented in that vocabulary and mapped onto the physical
/// parameters here. Nothing is lost: the darkroom name and the actual physical
/// value are always in the tooltip, and switching the vocabulary to Darkroom
/// shows the underlying controls directly.
///
/// The mapping deliberately lives in the UI layer. `Recipe` keeps storing the
/// physical quantities, so recipes, the parity tests and the shaders are all
/// untouched by what the sliders happen to be called.
enum Vocabulary: String, CaseIterable, Identifiable {
    case photographic = "Photographic"
    case darkroom = "Darkroom"
    var id: String { rawValue }
}

/// A presented control: its two names, its range in presentation units, and the
/// conversion to and from the physical parameter.
struct ControlSpec {
    let photographicName: String
    let darkroomName: String
    let range: ClosedRange<Double>
    let decimals: Int
    let unit: String
    let explanation: String
    /// Physical value for a presented value, and back.
    let toPhysical: (Double) -> Double
    let fromPhysical: (Double) -> Double

    func name(_ vocabulary: Vocabulary) -> String {
        vocabulary == .photographic ? photographicName : darkroomName
    }

    /// The tooltip always names the other vocabulary and the physical value, so
    /// the friendly label never hides what is actually happening.
    func help(_ vocabulary: Vocabulary, physical: Double) -> String {
        let other = vocabulary == .photographic ? darkroomName : photographicName
        return "\(explanation)\n\n\(vocabulary == .photographic ? "Darkroom" : "Photographic") name: \(other) "
             + "(currently \(String(format: "%.2f", physical)))."
    }

    func binding(_ physical: Binding<Double>) -> Binding<Double> {
        Binding(get: { fromPhysical(physical.wrappedValue) },
                set: { physical.wrappedValue = toPhysical($0) })
    }
}

/// A control that is the same quantity in both vocabularies, only rescaled and
/// possibly flipped so that "more" means "brighter".
private func linear(_ photographic: String, _ darkroom: String,
                    physicalPerUnit: Double, range: ClosedRange<Double> = -100...100,
                    decimals: Int = 0, unit: String = "", explanation: String) -> ControlSpec {
    ControlSpec(photographicName: photographic, darkroomName: darkroom, range: range,
                decimals: decimals, unit: unit, explanation: explanation,
                toPhysical: { $0 * physicalPerUnit },
                fromPhysical: { physicalPerUnit == 0 ? 0 : $0 / physicalPerUnit })
}

enum Controls {
    /// Midtone brightness, in fractions of the paper's lightness range: ±100
    /// moves mid-grey by a fifth of the distance from paper black to white.
    static let brightness = linear(
        "Brightness", "Midtone density", physicalPerUnit: 0.002,
        explanation: "How light the midtones are. Mid-grey moves most, and the move fades to "
                   + "nothing toward paper black and paper white, so neither end clips or goes "
                   + "milky. For a change that rides the whole curve, use Print exposure.")

    /// Print exposure. More light on the paper is a *darker* print, so the
    /// photographic slider runs the other way.
    static let printExposure = linear(
        "Print exposure", "Density", physicalPerUnit: -0.03,
        explanation: "The enlarger timer: every tone moves, the blacks and whites included, "
                   + "rolling into the paper's toe and shoulder rather than clipping.")

    /// Contrast, from the paper grade. Slope goes as 1/R, so more contrast is a
    /// lower ISO range value. ±100 spans the whole grade range, R180 to R50,
    /// so the slider can show anything Auto Contrast picks.
    static let contrast = contrast(softest: Constants.gradeMax)

    /// Contrast for a film whose softest grade is its own
    /// (`ProfilePack.softestGrade`): the hard half as for every film, the soft
    /// half spread from R115 to that grade. At R180 it is `contrast`.
    static func contrast(softest: Float) -> ControlSpec {
        let reference = Double(Constants.gradeReference)
        let soft = (Double(softest) - reference) / 100
        return ControlSpec(
            photographicName: "Contrast", darkroomName: "Grade",
            range: -100...100, decimals: 0, unit: "",
            explanation: "Overall contrast, set by the paper's contrast grade. "
                       + "R115 is neutral; harder grades compress the scale into fewer stops.",
            toPhysical: { reference - $0 * ($0 < 0 ? soft : 0.65) },
            fromPhysical: { reference - $0 < 0 ? (reference - $0) / soft : (reference - $0) / 0.65 })
    }

    static let shadowContrast = linear(
        "Shadow contrast", "Shadow grade", physicalPerUnit: -0.4,
        explanation: "Contrast in the shadows alone — the hard-filter half of a split-grade print.")

    static let highlightContrast = linear(
        "Highlight contrast", "Highlight grade", physicalPerUnit: -0.4,
        explanation: "Contrast in the highlights alone — the soft-filter half of a split-grade print.")

    static let shadows = linear(
        "Shadows", "Shadow density", physicalPerUnit: 0.01,
        explanation: "Lightens or darkens the dark half of the print, most in the deep shadows "
                   + "just above black. It is gone by mid-grey and leaves paper black where it "
                   + "is — Blacks sets that. Lifting has more room than darkening: there is "
                   + "little space left between a deep shadow and black.")

    static let highlights = linear(
        "Highlights", "Highlight density", physicalPerUnit: 0.01,
        explanation: "The same for the light half: most in the bright highlights just below "
                   + "white, gone by mid-grey, and paper white stays put — Whites sets that. "
                   + "Holding highlights back has more room than pushing them toward white.")

    static let blacks = linear(
        "Blacks", "Toe", physicalPerUnit: 0.01,
        explanation: "How the deepest tones roll off. Positive opens the blacks and keeps "
                   + "shadow detail; negative sharpens the knee for a harder black.")

    static let whites = linear(
        "Whites", "Shoulder", physicalPerUnit: -0.01,
        explanation: "How the brightest tones roll off. Negative holds highlights back from "
                   + "paper white; positive lets them run to white sooner.")

    static let blacksRange = linear(
        "Blacks range", "Toe reach", physicalPerUnit: 0.01, range: 10...300, decimals: 0, unit: "%",
        explanation: "How far the black roll-off reaches up into the tonal scale.")

    static let whitesRange = linear(
        "Whites range", "Shoulder reach", physicalPerUnit: 0.01, range: 10...300, decimals: 0, unit: "%",
        explanation: "How far the white roll-off reaches down into the tonal scale.")

    static let midtoneContrast = linear(
        "Midtone contrast", "Snap", physicalPerUnit: 0.005,
        explanation: "Contrast in the midtones only, anchored so mid-grey does not move and "
                   + "easing to nothing at both ends.")

    /// The enlarger's yellow filter runs along the blue-yellow axis, which is
    /// what a temperature slider controls — inverted, because adding yellow
    /// filtration to the printing light makes the print cooler.
    static let temperature = linear(
        "Temperature", "Yellow filter", physicalPerUnit: -0.4,
        explanation: "Warm/cool balance, applied by winding the enlarger's real yellow dichroic "
                   + "filter — a change to the printing light, not a tint on the output.")

    static let tint = linear(
        "Tint", "Magenta filter", physicalPerUnit: -0.4,
        explanation: "Green/magenta balance, applied with the enlarger's magenta filter.")

    static let highlightProtection = linear(
        "Highlight protection", "Pre-flash", physicalPerUnit: 0.003, range: 0...100, decimals: 0, unit: "%",
        explanation: "A uniform pre-exposure of the paper. The darkroom way to hold highlights: "
                   + "it lifts the paper off its toe so faint highlight detail starts to register.")

    static let maskExposure = linear(
        "Brightness", "Exposure", physicalPerUnit: -0.02, range: -100...100,
        explanation: "Positive brightens this area (dodge), negative darkens it (burn).")

    static let maskContrast = linear(
        "Contrast", "Local grade", physicalPerUnit: -0.3, range: -100...100,
        explanation: "Prints this area through a different contrast filter — what a hard-filter "
                   + "burn is on variable-contrast paper. It changes the area's contrast without "
                   + "moving its brightness.")
}
