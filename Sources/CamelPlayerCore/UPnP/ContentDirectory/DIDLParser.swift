import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

/// A single entry in a UPnP ContentDirectory result: either a container
/// (folder/album/artist) or a playable item (track).
public struct MediaObject: Identifiable, Sendable {
    public let id: String
    public let parentID: String
    public let title: String
    public let isContainer: Bool
    public let artist: String?
    public let album: String?
    public let albumArtURI: String?
    public let duration: TimeInterval?
    /// HTTP URL of the media resource (items only).
    public let resURL: String?
    public let childCount: Int?

    public init(
        id: String,
        parentID: String,
        title: String,
        isContainer: Bool,
        artist: String? = nil,
        album: String? = nil,
        albumArtURI: String? = nil,
        duration: TimeInterval? = nil,
        resURL: String? = nil,
        childCount: Int? = nil
    ) {
        self.id = id
        self.parentID = parentID
        self.title = title
        self.isContainer = isContainer
        self.artist = artist
        self.album = album
        self.albumArtURI = albumArtURI
        self.duration = duration
        self.resURL = resURL
        self.childCount = childCount
    }
}

/// Parses DIDL-Lite XML (the ContentDirectory Browse result) into MediaObjects.
public final class DIDLParser: NSObject {
    private var objects: [MediaObject] = []
    private var value = ""

    private var inObject = false
    private var isContainer = false
    private var id = ""
    private var parentID = ""
    private var childCount: Int?
    private var title = ""
    private var artist: String?
    private var album: String?
    private var albumArtURI: String?
    private var resURL: String?
    private var duration: TimeInterval?

    public override init() {
        super.init()
    }

    /// Parses a DIDL-Lite document. Returns the contained objects in document order.
    public func parse(_ didl: String) -> [MediaObject] {
        objects = []
        guard let data = didl.data(using: .utf8) else { return [] }
        let parser = XMLParser(data: data)
        parser.delegate = self
        parser.parse()
        return objects
    }

    private func resetObject() {
        isContainer = false
        id = ""
        parentID = ""
        childCount = nil
        title = ""
        artist = nil
        album = nil
        albumArtURI = nil
        resURL = nil
        duration = nil
    }

    static func parseDuration(_ s: String) -> TimeInterval? {
        let parts = s.split(separator: ":")
        guard parts.count == 3,
              let h = Double(parts[0]),
              let m = Double(parts[1]),
              let sec = Double(parts[2]),
              h.isFinite, h >= 0, h.rounded(.down) == h,
              m.isFinite, m >= 0, m < 60, m.rounded(.down) == m,
              sec.isFinite, sec >= 0, sec < 60 else { return nil }
        let total = h * 3600 + m * 60 + sec
        guard total.isFinite, Int(exactly: total.rounded(.towardZero)) != nil else { return nil }
        return total
    }
}

extension DIDLParser: XMLParserDelegate {
    public func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes attributeDict: [String: String] = [:]) {
        value = ""
        switch elementName {
        case "container", "item":
            resetObject()
            inObject = true
            isContainer = (elementName == "container")
            id = attributeDict["id"] ?? ""
            parentID = attributeDict["parentID"] ?? ""
            if let cc = attributeDict["childCount"] { childCount = Int(cc) }
        case "res":
            if let d = attributeDict["duration"] {
                duration = DIDLParser.parseDuration(d)
            }
        default:
            break
        }
    }

    public func parser(_ parser: XMLParser, foundCharacters string: String) {
        value += string
    }

    public func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        switch elementName {
        case "dc:title":
            title = trimmed
        case "upnp:artist", "dc:creator":
            if artist == nil || artist?.isEmpty == true { artist = trimmed }
        case "upnp:album":
            album = trimmed
        case "upnp:albumArtURI":
            albumArtURI = trimmed
        case "res":
            if resURL == nil, !trimmed.isEmpty { resURL = trimmed }
        case "container", "item":
            objects.append(MediaObject(
                id: id,
                parentID: parentID,
                title: title,
                isContainer: isContainer,
                artist: artist,
                album: album,
                albumArtURI: albumArtURI,
                duration: duration,
                resURL: resURL,
                childCount: childCount
            ))
            inObject = false
        default:
            break
        }
        value = ""
    }
}
