import Foundation

/// Manager for UPnP device discovery and management
public class UPnPDeviceManager: SSDPDiscoveryDelegate {
    private let discovery: SSDPDiscovery
    private(set) public var availableDevices: [UPnPDevice] = []

    /// Callback when a device is added
    public var onDeviceAdded: ((UPnPDevice) -> Void)?

    /// Callback when a device is removed
    public var onDeviceRemoved: ((UPnPDevice) -> Void)?

    public init() {
        self.discovery = SSDPDiscovery()
        self.discovery.delegate = self
    }

    /// Starts device discovery
    public func startDiscovery() {
        discovery.startDiscovery()
    }

    /// Stops device discovery
    public func stopDiscovery() {
        discovery.stopDiscovery()
        availableDevices.removeAll()
    }

    /// Refreshes device list (stops and restarts discovery)
    public func refresh() {
        stopDiscovery()
        // Small delay to ensure cleanup
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.startDiscovery()
        }
    }

    /// Gets a device by ID
    public func getDevice(id: String) -> UPnPDevice? {
        return availableDevices.first { $0.id == id }
    }

    // MARK: - SSDPDiscoveryDelegate

    public func ssdpDiscovery(_ discovery: SSDPDiscovery, didDiscoverDevice device: UPnPDevice) {
        // Avoid duplicates
        guard !availableDevices.contains(where: { $0.id == device.id }) else {
            return
        }

        availableDevices.append(device)
        onDeviceAdded?(device)
    }

    public func ssdpDiscovery(_ discovery: SSDPDiscovery, didRemoveDevice device: UPnPDevice) {
        availableDevices.removeAll { $0.id == device.id }
        onDeviceRemoved?(device)
    }
}
