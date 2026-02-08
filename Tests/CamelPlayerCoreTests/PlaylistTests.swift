import XCTest
@testable import CamelPlayerCore

final class PlaylistTests: XCTestCase {
    var playlist: Playlist!

    override func setUp() {
        super.setUp()
        playlist = Playlist()
    }

    func testAddItem() {
        let url = URL(fileURLWithPath: "/test/song.mp3")
        playlist.add(url: url)

        XCTAssertEqual(playlist.count, 1)
        XCTAssertNotNil(playlist.currentItem)
    }

    func testNextSequential() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)
        playlist.mode = .sequential

        let nextItem = playlist.next()
        XCTAssertNotNil(nextItem)
        XCTAssertEqual(nextItem?.url, url2)
    }

    func testPreviousSequential() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)
        playlist.mode = .sequential

        _ = playlist.next()
        let prevItem = playlist.previous()
        XCTAssertNotNil(prevItem)
        XCTAssertEqual(prevItem?.url, url1)
    }

    func testLoopMode() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)
        playlist.mode = .loop

        _ = playlist.next()
        let loopItem = playlist.next()
        XCTAssertNotNil(loopItem)
        XCTAssertEqual(loopItem?.url, url1)
    }

    func testRemoveItem() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)
        playlist.remove(at: 0)

        XCTAssertEqual(playlist.count, 1)
    }

    func testClear() {
        let url = URL(fileURLWithPath: "/test/song.mp3")
        playlist.add(url: url)
        playlist.clear()

        XCTAssertEqual(playlist.count, 0)
        XCTAssertNil(playlist.currentItem)
    }
}
