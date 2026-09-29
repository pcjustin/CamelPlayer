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

    func testLegacyFavoritesRemainReadable() throws {
        let data = Data(#"{"id":"1","title":"Album","artist":"Artist"}"#.utf8)
        let ref = try JSONDecoder().decode(AlbumRef.self, from: data)
        XCTAssertEqual(ref.id, "1")
        XCTAssertNil(ref.serverID)
    }
}
