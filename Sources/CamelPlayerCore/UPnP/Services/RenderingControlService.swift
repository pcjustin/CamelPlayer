import Foundation

/// UPnP RenderingControl service for controlling volume and other rendering parameters
public class RenderingControlService {
    private let controlURL: String
    private let serviceType: String
    private let soapClient: SOAPClient
    private let instanceID: String

    public init(controlURL: String, serviceType: String = "urn:schemas-upnp-org:service:RenderingControl:1", instanceID: String = "0") {
        self.controlURL = controlURL
        self.serviceType = serviceType
        self.instanceID = instanceID
        self.soapClient = SOAPClient()
    }

    // MARK: - Volume Control

    /// Sets the volume level
    /// - Parameters:
    ///   - volume: Volume level (0-100)
    ///   - channel: Audio channel (default "Master")
    public func setVolume(_ volume: Int, channel: String = "Master") async throws {
        let clampedVolume = max(0, min(100, volume))
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "SetVolume",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Channel": channel,
                "DesiredVolume": String(clampedVolume)
            ]
        )
    }

    /// Gets the current volume level
    /// - Parameter channel: Audio channel (default "Master")
    /// - Returns: Volume level (0-100)
    public func getVolume(channel: String = "Master") async throws -> Int {
        let response = try await soapClient.call(
            controlURL: controlURL,
            action: "GetVolume",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Channel": channel
            ]
        )

        guard let volumeString = response["CurrentVolume"],
              let volume = Int(volumeString) else {
            throw SOAPError.parsingError("Invalid volume response")
        }

        return volume
    }

    // MARK: - Mute Control

    /// Sets the mute state
    /// - Parameters:
    ///   - mute: true to mute, false to unmute
    ///   - channel: Audio channel (default "Master")
    public func setMute(_ mute: Bool, channel: String = "Master") async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "SetMute",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Channel": channel,
                "DesiredMute": mute ? "1" : "0"
            ]
        )
    }

    /// Gets the current mute state
    /// - Parameter channel: Audio channel (default "Master")
    /// - Returns: true if muted, false otherwise
    public func getMute(channel: String = "Master") async throws -> Bool {
        let response = try await soapClient.call(
            controlURL: controlURL,
            action: "GetMute",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Channel": channel
            ]
        )

        guard let muteString = response["CurrentMute"] else {
            throw SOAPError.parsingError("Invalid mute response")
        }

        return muteString == "1" || muteString.lowercased() == "true"
    }
}
