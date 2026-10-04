import XCTest
@testable import CamelPlayerCore

final class LibraryRefsTests: XCTestCase {
    func testAlbumReferencesPreserveSourceServerAndSeparateIdenticalObjectIDs() throws {
        let first = AlbumRef(album: MediaObject(id: "1", parentID: "0", title: "Album A", isContainer: true, serverID: "server-a"))
        let second = AlbumRef(album: MediaObject(id: "1", parentID: "0", title: "Album B", isContainer: true, serverID: "server-b"))
        XCTAssertNotEqual(first.identity, second.identity)
        let restored = try JSONDecoder().decode(AlbumRef.self, from: JSONEncoder().encode(first))
        XCTAssertEqual(restored.serverID, "server-a")
        XCTAssertEqual(restored, first)
    }

    func testTrackReferencesKeepAlbumAndCoverFromServerItemsAndQueue() throws {
        let object = MediaObject(id: "t", parentID: "a", title: "So What", isContainer: false,
                                 album: "Kind of Blue", albumArtURI: "http://nas/art.jpg",
                                 resURL: "http://nas/track.flac")
        let fromServer = try XCTUnwrap(TrackRef(object: object))
        XCTAssertEqual(fromServer.url, "http://nas/track.flac")
        XCTAssertEqual(fromServer.album, "Kind of Blue")
        let item = PlaylistItem(url: URL(string: fromServer.url)!, title: fromServer.title,
                                metadata: fromServer.metadata)
        XCTAssertEqual(TrackRef(item: item), fromServer)
        XCTAssertNil(TrackRef(object: MediaObject(id: "c", parentID: "0", title: "Album", isContainer: true)))
    }

    func testLegacyFavoritesRemainReadable() throws {
        let data = Data(#"{"id":"1","title":"Album","artist":"Artist"}"#.utf8)
        let ref = try JSONDecoder().decode(AlbumRef.self, from: data)
        XCTAssertEqual(ref.id, "1")
        XCTAssertNil(ref.serverID)
    }
}
