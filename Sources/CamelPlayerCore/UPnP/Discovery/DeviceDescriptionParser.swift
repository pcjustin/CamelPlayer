import Foundation

/// Parser for UPnP device description XML
public class DeviceDescriptionParser: NSObject {
    private var currentElement = ""
    private var currentValue = ""

    // Device properties
    private var friendlyName = ""
    private var manufacturer = ""
    private var modelName = ""
    private var rootDeviceType = ""

    // Service URLs
    private var avTransportControlURL: String?
    private var renderingControlControlURL: String?
    private var contentDirectoryControlURL: String?

    // Current service being parsed
    private var currentServiceType = ""
    private var currentControlURL = ""

    // Base URL for relative URLs
    private var baseURL: URL?

    // Result
    private var parsedDevice: UPnPDevice?

    public override init() {
        super.init()
    }

    /// Parses device description XML data
    /// - Parameters:
    ///   - data: XML data
    ///   - location: Device location URL
    ///   - uuid: Device UUID
    /// - Returns: Parsed UPnPDevice or nil
    public func parse(data: Data, location: URL, uuid: String) async -> UPnPDevice? {
        // Reset state
        reset()
        baseURL = location.deletingLastPathComponent()

        let parser = XMLParser(data: data)
        parser.delegate = self

        guard parser.parse() else {
            print("DeviceParser: Failed to parse XML")
            return nil
        }

        // Construct device
        guard !friendlyName.isEmpty else {
            print("DeviceParser: Missing friendlyName")
            return nil
        }

        // Resolve relative URLs
        let avTransportURL = resolveURL(avTransportControlURL)
        let renderingControlURL = resolveURL(renderingControlControlURL)
        let contentDirectoryURL = resolveURL(contentDirectoryControlURL)

        let deviceType: UPnPDeviceType
        if rootDeviceType.contains("MediaServer") {
            deviceType = .server
        } else if rootDeviceType.contains("MediaRenderer") {
            deviceType = .renderer
        } else {
            deviceType = .other
        }

        let device = UPnPDevice(
            id: uuid,
            friendlyName: friendlyName,
            manufacturer: manufacturer.isEmpty ? "Unknown" : manufacturer,
            modelName: modelName.isEmpty ? "Unknown" : modelName,
            location: location,
            deviceType: deviceType,
            avTransportURL: avTransportURL,
            renderingControlURL: renderingControlURL,
            contentDirectoryURL: contentDirectoryURL
        )

        parsedDevice = device
        return device
    }

    /// Resolves a relative URL against the base URL
    private func resolveURL(_ urlString: String?) -> String? {
        guard let urlString = urlString, !urlString.isEmpty else {
            return nil
        }

        // If it's already an absolute URL, return as-is
        if urlString.hasPrefix("http://") || urlString.hasPrefix("https://") {
            return urlString
        }

        // Resolve relative URL
        guard let baseURL = baseURL else {
            return urlString
        }

        // Construct absolute URL
        if urlString.hasPrefix("/") {
            // Absolute path
            guard let scheme = baseURL.scheme, let host = baseURL.host else {
                return urlString
            }
            let portPart = baseURL.port.map { ":\($0)" } ?? ""
            return "\(scheme)://\(host)\(portPart)\(urlString)"
        } else {
            // Relative path
            return baseURL.appendingPathComponent(urlString).absoluteString
        }
    }

    /// Resets parser state
    private func reset() {
        currentElement = ""
        currentValue = ""
        friendlyName = ""
        manufacturer = ""
        modelName = ""
        rootDeviceType = ""
        avTransportControlURL = nil
        renderingControlControlURL = nil
        contentDirectoryControlURL = nil
        currentServiceType = ""
        currentControlURL = ""
        parsedDevice = nil
    }
}

// MARK: - XMLParserDelegate

extension DeviceDescriptionParser: XMLParserDelegate {
    public func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes attributeDict: [String : String] = [:]) {
        currentElement = elementName
        currentValue = ""
    }

    public func parser(_ parser: XMLParser, foundCharacters string: String) {
        currentValue += string
    }

    public func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        let value = currentValue.trimmingCharacters(in: .whitespacesAndNewlines)

        switch elementName {
        case "deviceType":
            if rootDeviceType.isEmpty { // Root device type, not embedded devices
                rootDeviceType = value
            }
        case "friendlyName":
            if friendlyName.isEmpty { // Only set first occurrence (device, not service)
                friendlyName = value
            }
        case "manufacturer":
            if manufacturer.isEmpty {
                manufacturer = value
            }
        case "modelName":
            if modelName.isEmpty {
                modelName = value
            }
        case "serviceType":
            currentServiceType = value
        case "controlURL":
            currentControlURL = value
        case "service":
            // End of service element - save control URL if it's a service we care about
            if currentServiceType.contains("AVTransport") {
                avTransportControlURL = currentControlURL
            } else if currentServiceType.contains("RenderingControl") {
                renderingControlControlURL = currentControlURL
            } else if currentServiceType.contains("ContentDirectory") {
                contentDirectoryControlURL = currentControlURL
            }
            currentServiceType = ""
            currentControlURL = ""
        default:
            break
        }

        currentElement = ""
        currentValue = ""
    }

    public func parser(_ parser: XMLParser, parseErrorOccurred parseError: Error) {
        print("DeviceParser: Parse error: \(parseError)")
    }
}
