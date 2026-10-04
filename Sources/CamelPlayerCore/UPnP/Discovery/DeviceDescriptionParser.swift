import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

/// Parser for UPnP device description XML
public class DeviceDescriptionParser: NSObject {
    private var currentElement = ""
    private var currentValue = ""

    // Device properties
    private var friendlyName = ""
    private var manufacturer = ""
    private var modelName = ""

    // Service URLs
    private var avTransportControlURL: String?
    private var contentDirectoryControlURL: String?

    // Current service being parsed
    private var currentServiceType = ""
    private var currentControlURL = ""

    // Base URL for relative URLs
    private var baseURL: URL?

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
        baseURL = location

        let parser = XMLParser(data: data)
        parser.shouldProcessNamespaces = true
        parser.delegate = self

        guard parser.parse() else {
            coreLog("DeviceParser: Failed to parse XML")
            return nil
        }

        // Construct device
        guard !friendlyName.isEmpty else {
            coreLog("DeviceParser: Missing friendlyName")
            return nil
        }

        // Resolve relative URLs
        let avTransportURL = resolveURL(avTransportControlURL)
        let contentDirectoryURL = resolveURL(contentDirectoryControlURL)

        return UPnPDevice(
            id: uuid,
            friendlyName: friendlyName,
            manufacturer: manufacturer.isEmpty ? "Unknown" : manufacturer,
            modelName: modelName.isEmpty ? "Unknown" : modelName,
            location: location,
            avTransportURL: avTransportURL,
            contentDirectoryURL: contentDirectoryURL
        )
    }

    /// Resolves a relative URL against the base URL
    private func resolveURL(_ urlString: String?) -> String? {
        guard let urlString = urlString, !urlString.isEmpty else {
            return nil
        }

        guard let url = URL(string: urlString, relativeTo: baseURL)?.absoluteURL, url.isHTTP else { return nil }
        return url.absoluteString
    }

    /// Resets parser state
    private func reset() {
        currentElement = ""
        currentValue = ""
        friendlyName = ""
        manufacturer = ""
        modelName = ""
        avTransportControlURL = nil
        contentDirectoryControlURL = nil
        currentServiceType = ""
        currentControlURL = ""
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

    public func parser(_ parser: XMLParser, foundCDATA CDATABlock: Data) {
        currentValue += String(decoding: CDATABlock, as: UTF8.self)
    }

    public func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        let value = currentValue.trimmingCharacters(in: .whitespacesAndNewlines)

        switch elementName {
        case "URLBase":
            if let url = URL(string: value), url.isHTTP {
                baseURL = url
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
        coreLog("DeviceParser: Parse error: \(parseError)")
    }
}
