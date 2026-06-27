import XCTest
@testable import CamelPlayerCore

final class DIDLParserTests: XCTestCase {
    private let sample = """
    <DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">
      <container id="album/123" parentID="0" childCount="12">
        <dc:title>Kind of Blue</dc:title>
        <upnp:class>object.container.album.musicAlbum</upnp:class>
        <upnp:artist>Miles Davis</upnp:artist>
      </container>
      <item id="track/456" parentID="album/123">
        <dc:title>So What</dc:title>
        <upnp:class>object.item.audioItem.musicTrack</upnp:class>
        <upnp:artist>Miles Davis</upnp:artist>
        <upnp:album>Kind of Blue</upnp:album>
        <upnp:albumArtURI>http://nas/art.jpg</upnp:albumArtURI>
        <res protocolInfo="http-get:*:audio/flac:*" duration="0:09:22.000">http://nas/track456.flac</res>
      </item>
    </DIDL-Lite>
    """

    func testParsesContainerAndItemInOrder() {
        let objects = DIDLParser().parse(sample)
        XCTAssertEqual(objects.count, 2)
        XCTAssertTrue(objects[0].isContainer)
        XCTAssertFalse(objects[1].isContainer)
    }

    func testContainerFields() {
        let c = DIDLParser().parse(sample)[0]
        XCTAssertEqual(c.id, "album/123")
        XCTAssertEqual(c.parentID, "0")
        XCTAssertEqual(c.title, "Kind of Blue")
        XCTAssertEqual(c.childCount, 12)
        XCTAssertEqual(c.artist, "Miles Davis")
        XCTAssertNil(c.resURL)
    }

    func testItemFields() {
        let i = DIDLParser().parse(sample)[1]
        XCTAssertEqual(i.title, "So What")
        XCTAssertEqual(i.album, "Kind of Blue")
        XCTAssertEqual(i.albumArtURI, "http://nas/art.jpg")
        XCTAssertEqual(i.resURL, "http://nas/track456.flac")
        XCTAssertEqual(i.duration ?? 0, 562, accuracy: 0.001) // 9:22
    }

    func testEmptyAndGarbageReturnEmpty() {
        XCTAssertTrue(DIDLParser().parse("").isEmpty)
        XCTAssertTrue(DIDLParser().parse("<not didl").isEmpty)
    }

    func testDurationParsing() {
        XCTAssertEqual(DIDLParser.parseDuration("1:02:03"), 3723)
        XCTAssertNil(DIDLParser.parseDuration("bad"))
        XCTAssertNil(DIDLParser.parseDuration("1:02"))
    }
}
