import ArgumentParser
import CamelPlayerCore
import Foundation

public struct DevicesCommand: ParsableCommand {
    public static let configuration = CommandConfiguration(
        commandName: "devices",
        abstract: "List available audio output devices"
    )

    public init() {}

    public func run() throws {
        let player = try AudioPlayer()
        let devices = try player.listOutputDevices()
        let currentDeviceID = try player.getCurrentOutputDevice()
        let defaultDeviceID = try player.getDefaultOutputDevice()

        print("Available audio output devices:\n")

        for device in devices {
            var marker = "  "
            if device.id == currentDeviceID {
                marker = "* "
            } else if device.id == defaultDeviceID {
                marker = "→ "
            }

            print("\(marker)[\(device.id)] \(device.name)")
        }

        print("\nLegend:")
        print("  * Current device")
        print("  → System default device")
    }
}
