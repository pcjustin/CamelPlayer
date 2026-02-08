import ArgumentParser
import CamelPlayerCore
import Foundation

public struct PlayCommand: ParsableCommand {
    public static let configuration = CommandConfiguration(
        commandName: "play",
        abstract: "Play an audio file directly"
    )

    @Argument(help: "Path to the audio file to play")
    public var filePath: String

    @Option(name: .shortAndLong, help: "Output device ID (use 'devices' command to list)")
    public var device: UInt32?

    @Flag(name: .shortAndLong, help: "Wait for playback to finish")
    public var wait: Bool = false

    public init() {}

    public func run() throws {
        let url = URL(fileURLWithPath: filePath)

        let player = try AudioPlayer()

        if let deviceID = device {
            try player.setOutputDevice(deviceID: deviceID)
            let devices = try player.listOutputDevices()
            if let selectedDevice = devices.first(where: { $0.id == deviceID }) {
                print("Using output device: \(selectedDevice.name)")
            }
        }

        try player.load(url: url)

        print("Playing: \(url.lastPathComponent)")

        try player.play()

        if wait {
            while player.state == .playing {
                let current = player.currentTime
                let total = player.duration ?? 0

                print(String(format: "\rProgress: %.1f / %.1f seconds", current, total), terminator: "")
                fflush(stdout)

                Thread.sleep(forTimeInterval: 0.1)
            }
            print("\nPlayback finished.")
        } else {
            print("Playback started. Press Ctrl+C to exit.")
            RunLoop.main.run()
        }
    }
}
