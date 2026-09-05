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

    func testShufflePreviousWalksHistory() {
        let urls = (1...5).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        playlist.mode = .shuffle

        let first = playlist.currentItem
        let second = playlist.next()
        _ = playlist.next()

        // previous() retraces the actual play order, not a random jump.
        XCTAssertEqual(playlist.previous()?.id, second?.id)
        XCTAssertEqual(playlist.previous()?.id, first?.id)
        // History exhausted: stay on the current track.
        XCTAssertEqual(playlist.previous()?.id, first?.id)
    }

    func testShuffleSingleItemReturnsItself() {
        let url = URL(fileURLWithPath: "/test/only.mp3")
        playlist.add(url: url)
        playlist.mode = .shuffle

        XCTAssertEqual(playlist.next()?.url, url)
    }

    func testMoveKeepsCurrentTrack() {
        let urls = (1...4).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        _ = playlist.jumpTo(index: 2) // current = song3

        // Move song1 (index 0) to the end.
        playlist.move(fromOffsets: IndexSet(integer: 0), toOffset: 4)

        // Order is now song2, song3, song4, song1; current is still song3.
        XCTAssertEqual(playlist.currentItem?.url, urls[2])
        XCTAssertEqual(playlist.currentPosition, 1)
        XCTAssertEqual(playlist.allItems().map { $0.url }, [urls[1], urls[2], urls[3], urls[0]])
    }

    func testMoveDownByOne() {
        let urls = (1...3).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        // Down button: move index 0 with toOffset index+2.
        playlist.move(fromOffsets: IndexSet(integer: 0), toOffset: 2)
        XCTAssertEqual(playlist.allItems().map { $0.url }, [urls[1], urls[0], urls[2]])
    }

    func testMoveUpByOne() {
        let urls = (1...3).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        // Up button: move index 2 with toOffset index-1.
        playlist.move(fromOffsets: IndexSet(integer: 2), toOffset: 1)
        XCTAssertEqual(playlist.allItems().map { $0.url }, [urls[0], urls[2], urls[1]])
    }

    func testMoveCurrentTrackItself() {
        let urls = (1...3).map { URL(fileURLWithPath: "/test/song\($0).mp3") }
        urls.forEach { playlist.add(url: $0) }
        _ = playlist.jumpTo(index: 0) // current = song1

        // Move the current track (song1) to the end.
        playlist.move(fromOffsets: IndexSet(integer: 0), toOffset: 3)

        XCTAssertEqual(playlist.currentItem?.url, urls[0])
        XCTAssertEqual(playlist.currentPosition, 2)
    }

    func testClear() {
        let url = URL(fileURLWithPath: "/test/song.mp3")
        playlist.add(url: url)
        playlist.clear()

        XCTAssertEqual(playlist.count, 0)
        XCTAssertNil(playlist.currentItem)
    }
    func testShuffleWithoutLoopPlaysEachTrackOnceThenStops() {
        playlist.addAll(urls: (0..<5).map { URL(fileURLWithPath: "/test/\($0).wav") })
        playlist.shuffle = true
        playlist.loopMode = .off
        var ids = Set([playlist.currentItem!.id])
        for _ in 1..<5 {
            guard let item = playlist.next() else { return XCTFail("Shuffle stopped early") }
            XCTAssertTrue(ids.insert(item.id).inserted)
        }
        XCTAssertNil(playlist.next())
        XCTAssertEqual(ids.count, 5)
    }

    func testPeekDoesNotAdvanceAndRespectsModes() {
        let urls = (0..<2).map { URL(fileURLWithPath: "/test/\($0).wav") }
        playlist.addAll(urls: urls)
        XCTAssertEqual(playlist.peekNext()?.url, urls[1])
        XCTAssertEqual(playlist.currentPosition, 0)
        _ = playlist.next()
        XCTAssertNil(playlist.peekNext())
        playlist.loopMode = .all
        XCTAssertEqual(playlist.peekNext()?.url, urls[0])
        playlist.shuffle = true
        XCTAssertNil(playlist.peekNext())
        playlist.shuffle = false
        playlist.loopMode = .one
        XCTAssertNil(playlist.peekNext())
    }

    func testLoopOneRepeatsCurrentWithAndWithoutShuffle() {
        playlist.addAll(urls: (0..<3).map { URL(fileURLWithPath: "/test/\($0).wav") })
        let selected = playlist.jumpTo(index: 1)
        playlist.loopMode = .one
        for shuffle in [false, true] {
            playlist.shuffle = shuffle
            XCTAssertEqual(playlist.next()?.id, selected?.id)
            XCTAssertEqual(playlist.previous()?.id, selected?.id)
        }
    }

}
