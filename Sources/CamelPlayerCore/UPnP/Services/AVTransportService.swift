import Foundation

/// UPnP AVTransport service for controlling media playback
public class AVTransportService {
    private let controlURL: String
    private let serviceType: String
    private let soapClient: SOAPClient
    private let instanceID: String

    /// Transport state as reported by the device
    public enum TransportState: String {
        case stopped = "STOPPED"
        case playing = "PLAYING"
        case paused = "PAUSED_PLAYBACK"
        case transitioning = "TRANSITIONING"
        case noMediaPresent = "NO_MEDIA_PRESENT"
        case unknown

        public init(rawValue: String) {
            switch rawValue.uppercased() {
            case "STOPPED": self = .stopped
            case "PLAYING": self = .playing
            case "PAUSED_PLAYBACK", "PAUSED": self = .paused
            case "TRANSITIONING": self = .transitioning
            case "NO_MEDIA_PRESENT": self = .noMediaPresent
            default: self = .unknown
            }
        }
    }

    /// Position information returned by GetPositionInfo
    public struct PositionInfo {
        public let trackDuration: TimeInterval
        public let trackPosition: TimeInterval
        public let trackURI: String

        init(duration: String, position: String, uri: String) {
            self.trackDuration = Self.parseTime(duration)
            self.trackPosition = Self.parseTime(position)
            self.trackURI = uri
        }

        /// Parses time in format "H:MM:SS" or "H:MM:SS.mmm"
        private static func parseTime(_ timeString: String) -> TimeInterval {
            let components = timeString.split(separator: ":")
            guard components.count >= 2 else { return 0 }

            let hours = Double(components[0]) ?? 0
            let minutes = Double(components[1]) ?? 0
            let seconds: Double
            if components.count >= 3 {
                seconds = Double(components[2]) ?? 0
            } else {
                seconds = 0
            }

            return hours * 3600 + minutes * 60 + seconds
        }
    }

    public init(controlURL: String, serviceType: String = "urn:schemas-upnp-org:service:AVTransport:1", instanceID: String = "0") {
        self.controlURL = controlURL
        self.serviceType = serviceType
        self.instanceID = instanceID
        self.soapClient = SOAPClient()
    }

    // MARK: - Playback Control

    /// Sets the URI of the media to play
    /// - Parameters:
    ///   - uri: The URI of the media file (HTTP URL)
    ///   - metadata: Optional DIDL-Lite metadata
    public func setAVTransportURI(uri: String, metadata: String = "") async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "SetAVTransportURI",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "CurrentURI": uri,
                "CurrentURIMetaData": metadata
            ]
        )
    }

    /// Starts playback
    /// - Parameter speed: Playback speed (usually "1")
    public func play(speed: String = "1") async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "Play",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Speed": speed
            ]
        )
    }

    /// Pauses playback
    public func pause() async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "Pause",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID
            ]
        )
    }

    /// Stops playback
    public func stop() async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "Stop",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID
            ]
        )
    }

    /// Seeks to a specific position
    /// - Parameter target: Position in format "H:MM:SS" or "H:MM:SS.mmm"
    public func seek(target: String) async throws {
        _ = try await soapClient.call(
            controlURL: controlURL,
            action: "Seek",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID,
                "Unit": "REL_TIME",
                "Target": target
            ]
        )
    }

    /// Seeks to a specific time interval
    /// - Parameter time: Time in seconds
    public func seek(to time: TimeInterval) async throws {
        let hours = Int(time) / 3600
        let minutes = (Int(time) % 3600) / 60
        let seconds = Int(time) % 60
        let target = String(format: "%d:%02d:%02d", hours, minutes, seconds)
        try await seek(target: target)
    }

    // MARK: - Status Query

    /// Gets transport information (state, status, speed)
    /// - Returns: Dictionary containing CurrentTransportState, CurrentTransportStatus, CurrentSpeed
    public func getTransportInfo() async throws -> [String: String] {
        return try await soapClient.call(
            controlURL: controlURL,
            action: "GetTransportInfo",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID
            ]
        )
    }

    /// Gets the current transport state
    /// - Returns: The transport state
    public func getTransportState() async throws -> TransportState {
        let info = try await getTransportInfo()
        let stateString = info["CurrentTransportState"] ?? "UNKNOWN"
        return TransportState(rawValue: stateString)
    }

    /// Gets position information (track duration, position, URI)
    /// - Returns: Dictionary containing TrackDuration, RelTime, TrackURI, etc.
    public func getPositionInfo() async throws -> [String: String] {
        return try await soapClient.call(
            controlURL: controlURL,
            action: "GetPositionInfo",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID
            ]
        )
    }

    /// Gets the current position information in a structured format
    /// - Returns: PositionInfo object
    public func getCurrentPosition() async throws -> PositionInfo {
        let info = try await getPositionInfo()
        return PositionInfo(
            duration: info["TrackDuration"] ?? "0:00:00",
            position: info["RelTime"] ?? "0:00:00",
            uri: info["TrackURI"] ?? ""
        )
    }

    /// Gets media information
    /// - Returns: Dictionary containing NrTracks, MediaDuration, CurrentURI, etc.
    public func getMediaInfo() async throws -> [String: String] {
        return try await soapClient.call(
            controlURL: controlURL,
            action: "GetMediaInfo",
            serviceType: serviceType,
            arguments: [
                "InstanceID": instanceID
            ]
        )
    }
}
