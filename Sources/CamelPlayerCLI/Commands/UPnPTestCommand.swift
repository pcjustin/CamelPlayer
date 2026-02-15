import ArgumentParser
import CamelPlayerCore
import Foundation

public struct UPnPTestCommand: ParsableCommand {
    public static let configuration = CommandConfiguration(
        commandName: "upnp-test",
        abstract: "Test UPnP device discovery"
    )

    @Option(name: .shortAndLong, help: "Discovery timeout in seconds")
    public var timeout: Int = 10

    public init() {}

    public func run() throws {
        print("Starting UPnP device discovery test...")
        print("Timeout: \(timeout) seconds")
        print("Looking for MediaRenderer devices on the network...")
        print()

        let manager = UPnPDeviceManager()
        var foundDevices = false

        manager.onDeviceAdded = { device in
            foundDevices = true
            print("✅ Found device:")
            print("   Name: \(device.friendlyName)")
            print("   Manufacturer: \(device.manufacturer)")
            print("   Model: \(device.modelName)")
            print("   Location: \(device.location)")
            print("   AVTransport: \(device.avTransportURL ?? "N/A")")
            print("   RenderingControl: \(device.renderingControlURL ?? "N/A")")
            print()
        }

        manager.startDiscovery()

        // Wait for the specified timeout
        print("Scanning network for \(timeout) seconds...")

        // Use RunLoop instead of Thread.sleep to allow callbacks to execute
        let endTime = Date().addingTimeInterval(TimeInterval(timeout))
        while Date() < endTime {
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        }

        // Give a bit more time for async callbacks to complete
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))

        print()
        print("Discovery complete!")
        print("Total devices found: \(manager.availableDevices.count)")

        if !foundDevices {
            print()
            print("⚠️  No UPnP MediaRenderer devices found.")
            print()
            print("Troubleshooting tips:")
            print("1. Make sure your device is on the same network")
            print("2. Enable UPnP/DLNA on your device")
            print("3. Check your firewall settings")
            print("4. See UPNP_DEBUG.md for detailed debugging steps")
            print()
            print("Testing with VLC:")
            print("  1. Open VLC")
            print("  2. Go to View > Renderer")
            print("  3. Enable 'Local renderer discovery'")
            print("  4. Run this test again")
        }
    }
}
