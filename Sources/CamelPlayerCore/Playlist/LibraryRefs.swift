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

    /// A server track; nil when it has no resource to play.
    public init?(object: MediaObject) {
        guard let res = object.resURL else { return nil }
        self.init(url: res, title: object.title, album: object.album,
                  albumArtURI: object.albumArtURI, metadata: DIDLBuilder.metadata(for: object))
    }

    /// A queued track; album and cover come back out of its DIDL metadata.
    public init(item: PlaylistItem) {
        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        self.init(url: item.url.absoluteString, title: item.title, album: parsed?.album,
                  albumArtURI: parsed?.albumArtURI, metadata: item.metadata)
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
