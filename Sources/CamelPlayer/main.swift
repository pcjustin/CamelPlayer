import ArgumentParser
import CamelPlayerCLI

@main
struct CamelPlayer: ParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "camelplayer",
        abstract: "A Swift CLI audio player for macOS with independent output device control",
        version: "0.1.0",
        subcommands: [InteractiveCommand.self, PlayCommand.self, DevicesCommand.self],
        defaultSubcommand: InteractiveCommand.self
    )
}
