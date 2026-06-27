import Foundation

/// Role of a discovered UPnP device.
public enum UPnPDeviceType: Sendable {
    case renderer
    case server
    case other
}

/// Represents a UPnP device discovered on the network
public struct UPnPDevice: Identifiable, Hashable, Sendable {
    public let id: String // UUID or UDN
    public let friendlyName: String
    public let manufacturer: String
    public let modelName: String
    public let location: URL
    public let deviceType: UPnPDeviceType
    public let avTransportURL: String?
    public let renderingControlURL: String?
    public let contentDirectoryURL: String?

    public init(
        id: String,
        friendlyName: String,
        manufacturer: String,
        modelName: String,
        location: URL,
        deviceType: UPnPDeviceType = .other,
        avTransportURL: String? = nil,
        renderingControlURL: String? = nil,
        contentDirectoryURL: String? = nil
    ) {
        self.id = id
        self.friendlyName = friendlyName
        self.manufacturer = manufacturer
        self.modelName = modelName
        self.location = location
        self.deviceType = deviceType
        self.avTransportURL = avTransportURL
        self.renderingControlURL = renderingControlURL
        self.contentDirectoryURL = contentDirectoryURL
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    public static func == (lhs: UPnPDevice, rhs: UPnPDevice) -> Bool {
        return lhs.id == rhs.id
    }
}
