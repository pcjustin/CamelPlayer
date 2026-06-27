import XCTest
@testable import CamelPlayerCore

final class DIDLBuilderTests: XCTestCase {
    func testBuildsFullMetadata() {
        let object = MediaObject(
            id: "t1", parentID: "a1", title: "So What", isContainer: false,
            artist: "Miles Davis", album: "Kind of Blue",
            albumArtURI: "http://nas/art.jpg", duration: 562,
            resURL: "http://nas/track.flac"
        )
        let didl = DIDLBuilder.metadata(for: object)!
        XCTAssertTrue(didl.contains("<dc:title>So What</dc:title>"))
        XCTAssertTrue(didl.contains("<upnp:artist>Miles Davis</upnp:artist>"))
        XCTAssertTrue(didl.contains("<upnp:album>Kind of Blue</upnp:album>"))
        XCTAssertTrue(didl.contains("<upnp:albumArtURI>http://nas/art.jpg</upnp:albumArtURI>"))
        XCTAssertTrue(didl.contains("object.item.audioItem.musicTrack"))
        XCTAssertTrue(didl.contains("duration=\"0:09:22\""))
        XCTAssertTrue(didl.contains(">http://nas/track.flac</res>"))
        XCTAssertTrue(didl.hasPrefix("<DIDL-Lite"))
    }

    func testEscapesSpecialCharacters() {
        let object = MediaObject(
            id: "t", parentID: "0", title: "Rock & \"Roll\" <mix>", isContainer: false,
            resURL: "http://nas/a.flac"
        )
        let didl = DIDLBuilder.metadata(for: object)!
        XCTAssertTrue(didl.contains("Rock &amp; &quot;Roll&quot; &lt;mix&gt;"))
        XCTAssertFalse(didl.contains("<mix>"))
    }

    func testNilWithoutResURL() {
        let object = MediaObject(id: "c", parentID: "0", title: "Album", isContainer: true)
        XCTAssertNil(DIDLBuilder.metadata(for: object))
    }

    func testOmitsEmptyOptionalFields() {
        let object = MediaObject(
            id: "t", parentID: "0", title: "Track", isContainer: false,
            resURL: "http://nas/a.flac"
        )
        let didl = DIDLBuilder.metadata(for: object)!
        XCTAssertFalse(didl.contains("<upnp:artist>"))
        XCTAssertFalse(didl.contains("<upnp:album>"))
        XCTAssertFalse(didl.contains("albumArtURI"))
        XCTAssertFalse(didl.contains("duration="))
    }
}
