import Foundation

/// A persistable reference to a favorite track. Identity is the URL string.
public struct TrackRef: Codable, Identifiable, Equatable {
    public let url: String
    public let title: String
    public let album: String?
    public let albumArtURI: String?
    public let metadata: String?

    public var id: String { url }

    public init(url: String, title: String, album: String?, albumArtURI: String?, metadata: String?) {
        self.url = url
        self.title = title
        self.album = album
        self.albumArtURI = albumArtURI
        self.metadata = metadata
    }
}

/// A persistable reference to a favorite album (a server container).
public struct AlbumRef: Codable, Identifiable, Equatable {
    public let id: String
    public let title: String
    public let artist: String?
    public let serverID: String?
    public var identity: String { "\(serverID ?? "")|\(id)" }

    public init(id: String, title: String, artist: String?, serverID: String? = nil) {
        self.id = id
        self.title = title
        self.artist = artist
        self.serverID = serverID
    }

    public init(album: MediaObject) {
        self.id = album.id
        self.title = album.title
        self.artist = album.artist
        self.serverID = album.serverID
    }
}
