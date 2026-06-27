import Foundation

/// Manager for UPnP device discovery and management
public class UPnPDeviceManager: SSDPDiscoveryDelegate {
    private let discovery: SSDPDiscovery

    /// Renderers (sinks) — devices we can send audio to via AVTransport.
    private(set) public var availableRenderers: [UPnPDevice] = []

    /// Media servers (sources) — devices we can browse via ContentDirectory.
    private(set) public var availableServers: [UPnPDevice] = []

    /// Called when the renderer list changes.
    public var onRenderersChanged: (() -> Void)?

    /// Called when the media server list changes.
    public var onServersChanged: (() -> Void)?

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
        availableRenderers.removeAll()
        availableServers.removeAll()
    }

    /// Refreshes device list (stops and restarts discovery)
    public func refresh() {
        stopDiscovery()
        // Small delay to ensure cleanup
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.startDiscovery()
        }
    }

    /// Gets a renderer by ID
    public func getRenderer(id: String) -> UPnPDevice? {
        return availableRenderers.first { $0.id == id }
    }

    /// Gets a server by ID
    public func getServer(id: String) -> UPnPDevice? {
        return availableServers.first { $0.id == id }
    }

    // MARK: - SSDPDiscoveryDelegate

    public func ssdpDiscovery(_ discovery: SSDPDiscovery, didDiscoverDevice device: UPnPDevice) {
        // Classify by capability so a device exposing both services lands in
        // both lists, regardless of how its deviceType parsed.
        if device.avTransportURL != nil,
           !availableRenderers.contains(where: { $0.id == device.id }) {
            availableRenderers.append(device)
            onRenderersChanged?()
        }
        if device.contentDirectoryURL != nil,
           !availableServers.contains(where: { $0.id == device.id }) {
            availableServers.append(device)
            onServersChanged?()
        }
    }

    public func ssdpDiscovery(_ discovery: SSDPDiscovery, didRemoveDevice device: UPnPDevice) {
        if availableRenderers.contains(where: { $0.id == device.id }) {
            availableRenderers.removeAll { $0.id == device.id }
            onRenderersChanged?()
        }
        if availableServers.contains(where: { $0.id == device.id }) {
            availableServers.removeAll { $0.id == device.id }
            onServersChanged?()
        }
    }
}
