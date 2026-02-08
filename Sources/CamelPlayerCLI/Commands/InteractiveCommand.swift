import ArgumentParser
import CamelPlayerCore
import Foundation

public struct InteractiveCommand: ParsableCommand {
    public static let configuration = CommandConfiguration(
        commandName: "interactive",
        abstract: "Enter interactive mode for full playback control"
    )

    public init() {}

    public func run() throws {
        let mode = try InteractiveMode()
        mode.run()
    }
}
