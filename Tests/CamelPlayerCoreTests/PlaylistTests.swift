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

    func testRemoveBeforeCurrentKeepsCurrentItem() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")
        let url3 = URL(fileURLWithPath: "/test/song3.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)
        playlist.add(url: url3)

        // Move current to index 2 (song3), then remove an earlier item.
        _ = playlist.jumpTo(index: 2)
        playlist.remove(at: 0)

        // The current item must still be song3, now at index 1.
        XCTAssertEqual(playlist.currentItem?.url, url3)
        XCTAssertEqual(playlist.currentPosition, 1)
    }

    func testRemoveCurrentItemClampsPosition() {
        let url1 = URL(fileURLWithPath: "/test/song1.mp3")
        let url2 = URL(fileURLWithPath: "/test/song2.mp3")

        playlist.add(url: url1)
        playlist.add(url: url2)

        _ = playlist.jumpTo(index: 1)
        playlist.remove(at: 1)

        XCTAssertEqual(playlist.count, 1)
        XCTAssertEqual(playlist.currentPosition, 0)
        XCTAssertEqual(playlist.currentItem?.url, url1)
    }

    func testShuffleAvoidsImmediateRepeat() {
        let urls = (1...5).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        playlist.mode = .shuffle

        // With more than one item, next() must never return the current track.
        for _ in 0..<100 {
            let before = playlist.currentPosition
            _ = playlist.next()
            XCTAssertNotEqual(playlist.currentPosition, before)
        }
    }

    func testShuffleSingleItemReturnsItself() {
        let url = URL(fileURLWithPath: "/test/only.mp3")
        playlist.add(url: url)
        playlist.mode = .shuffle

        XCTAssertEqual(playlist.next()?.url, url)
    }

    func testClear() {
        let url = URL(fileURLWithPath: "/test/song.mp3")
        playlist.add(url: url)
        playlist.clear()

        XCTAssertEqual(playlist.count, 0)
        XCTAssertNil(playlist.currentItem)
    }
}
