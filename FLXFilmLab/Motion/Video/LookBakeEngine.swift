import Foundation

/// Bakes print looks off the main thread, one at a time.
///
/// A bake is one render of the lattice through the engine — about 10 ms for
/// 65 points a side — so a slider drag re-bakes on every tick.
actor LookBakeEngine {
    private var baker: LookBaker?

    private func ready() throws -> LookBaker {
        if let baker { return baker }
        let made = try LookBaker()
        baker = made
        return made
    }

    /// The look as the frame pipeline reads it: the clip's log format in
    /// (`LogFormat.lutInput`), a 2.4 power out.
    func bake(_ recipe: Recipe, input: LookLUT.Input = .nlogCode, size: Int = 65) throws -> LookLUT {
        try ready().bake(recipe: recipe, size: size, input: input, output: .bt1886)
    }

    /// The look as a .cube file for other software.
    func cube(_ recipe: Recipe, size: Int, input: LookLUT.Input, output: LookLUT.Output,
              title: String, comments: [String]) throws -> String {
        try ready().bake(recipe: recipe, size: size, input: input, output: output)
            .cubeText(title: title, comments: comments)
    }
}
