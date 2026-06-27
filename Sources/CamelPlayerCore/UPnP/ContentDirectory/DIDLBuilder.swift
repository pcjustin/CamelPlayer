import Foundation

/// Builds DIDL-Lite metadata for a track, sent to a renderer as
/// CurrentURIMetaData so it can display title/artist/album/art.
enum DIDLBuilder {
    static func metadata(for object: MediaObject) -> String? {
        guard let res = object.resURL else { return nil }
        return metadata(
            resURL: res,
            title: object.title,
            artist: object.artist,
            album: object.album,
            albumArtURI: object.albumArtURI,
            duration: object.duration
        )
    }

    static func metadata(
        resURL: String,
        title: String,
        artist: String?,
        album: String?,
        albumArtURI: String?,
        duration: TimeInterval?
    ) -> String {
        var fields = "<dc:title>\(escape(title))</dc:title>"
        fields += "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        if let artist = artist, !artist.isEmpty {
            fields += "<upnp:artist>\(escape(artist))</upnp:artist>"
            fields += "<dc:creator>\(escape(artist))</dc:creator>"
        }
        if let album = album, !album.isEmpty {
            fields += "<upnp:album>\(escape(album))</upnp:album>"
        }
        if let art = albumArtURI, !art.isEmpty {
            fields += "<upnp:albumArtURI>\(escape(art))</upnp:albumArtURI>"
        }
        let durationAttr = duration.map { " duration=\"\(formatDuration($0))\"" } ?? ""
        fields += "<res protocolInfo=\"http-get:*:*:*\"\(durationAttr)>\(escape(resURL))</res>"

        return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
            + " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
            + " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
            + "<item id=\"0\" parentID=\"-1\" restricted=\"1\">\(fields)</item>"
            + "</DIDL-Lite>"
    }

    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
            .replacingOccurrences(of: "\"", with: "&quot;")
            .replacingOccurrences(of: "'", with: "&apos;")
    }

    private static func formatDuration(_ time: TimeInterval) -> String {
        let total = Int(time)
        return String(format: "%d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60)
    }
}
