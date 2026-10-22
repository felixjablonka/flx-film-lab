import Foundation

/// Layout shared with flx.metal. Changing anything here means changing both.
enum Constants {
    /// 380..780 nm at 5 nm — agx `config.SPECTRAL_SHAPE`.
    static let wavelengths = 81
    /// Samples on the log-exposure axis — agx `config.LOG_EXPOSURE`.
    static let curveSamples = 256
    /// The pre-coupler curves and the edge-gain scale are sampled this many
    /// times finer (`FilmMath.oversampled`): inverting strong couplers on a
    /// steep curve squeezes its axis, and agx's own grid then undersamples the
    /// curve it inverts to.
    static let couplerOversampling = 16
    static let couplerCurveSamples = (curveSamples - 1) * couplerOversampling + 1

    // Offsets into the packed spectral-table buffer handed to `print_and_scan`.
    static let offFilmChannelDensity = 0
    static let offFilmBaseDensity = 243
    static let offFilmValid = 324
    static let offPrintIlluminant = 405
    static let offPaperSensitivity = 486
    static let offPrintChannelDensity = 729
    static let offPrintBaseDensity = 972
    static let offPrintValid = 1053
    static let offViewingIlluminant = 1134
    static let offCMFs = 1215
    static let spectralTableFloats = 1458

    /// Resolution of the paper transfer LUT: log-exposure x local grade.
    static let toneLutWidth = 512
    static let toneLutHeight = 17
    /// Local-grade span in ISO-R points, matching NegPy's `LocalMask.grade` range.
    static let localGradeMin: Float = -40
    static let localGradeMax: Float = 40

    /// ISO-R contrast, NegPy's paper-grade scale. R110 is roughly grade 2.
    static let gradeReference: Float = 115
    static let gradeMin: Float = 50
    static let gradeMax: Float = 180

    /// Graduated exposure masks the film-exposure kernel takes at once.
    static let maxExposureGradients = 8
}
