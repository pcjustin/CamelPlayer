import XCTest
@testable import CamelPlayerCore

private final class PlaybackStub: PlaybackEngine {
    var state: PlaybackState = .stopped
    var currentURL: URL?
    var duration: TimeInterval? = 10
    var currentTime: TimeInterval = 0
    var volume: Float = 1
    var onPlaybackFinished: (() -> Void)?
    var onAdvancedToNext: (() -> Void)?
    var nextURL: URL?
    var loaded: ((URL) -> Void)?

    func setNextTrack(url: URL?, metadata: String?) { nextURL = url }
    func loadAndPlay(url: URL, metadata: String?) async throws {
        currentURL = url
        state = .playing
        loaded?(url)
    }
    func play() async throws { state = .playing }
    func pause() { state = .paused }
    func stop() { state = .stopped; nextURL = nil }
    func seek(to time: TimeInterval) async throws { currentTime = time }
    func getFileFormat() -> String? { nil }
}

@MainActor
final class PlaybackControllerTests: XCTestCase {
    private let urls = (0..<3).map { URL(fileURLWithPath: "/test/\($0).wav") }

    func testQueueEditsSynchronizePlaybackAndPreload() async throws {
        let engine = PlaybackStub()
        let controller = PlaybackController(engine: engine)
        controller.addToPlaylist(urls: urls)
        try await controller.play()
        XCTAssertEqual(engine.nextURL, urls[1])
        controller.removeFromPlaylist(at: 1)
        XCTAssertEqual(engine.nextURL, urls[2])
        controller.shuffle = true
        XCTAssertNil(engine.nextURL)
        controller.shuffle = false
        XCTAssertEqual(engine.nextURL, urls[2])
        controller.removeFromPlaylist(at: 0)
        XCTAssertEqual(controller.currentState, .stopped)
        try await controller.play()
        controller.clearPlaylist()
        XCTAssertEqual(controller.currentState, .stopped)
        XCTAssertNil(controller.currentItem)
        XCTAssertNil(engine.nextURL)
    }

    func testShortTrackStillAdvances() async throws {
        let engine = PlaybackStub()
        let controller = PlaybackController(engine: engine)
        controller.addToPlaylist(urls: urls)
        try await controller.play()
        let advanced = expectation(description: "Next track loaded")
        engine.loaded = { [urls] url in
            XCTAssertEqual(url, urls[1])
            advanced.fulfill()
        }
        engine.state = .stopped
        engine.onPlaybackFinished?()
        await fulfillment(of: [advanced], timeout: 1)
    }

    func testLoopOneReplaysAFinishedTrackButNextMovesOn() async throws {
        let engine = PlaybackStub()
        let controller = PlaybackController(engine: engine)
        controller.addToPlaylist(urls: urls)
        controller.loopMode = .one
        try await controller.play()
        try await controller.next()
        XCTAssertEqual(engine.currentURL, urls[1])
        let replayed = expectation(description: "Finished track replays")
        engine.loaded = { [urls] url in
            XCTAssertEqual(url, urls[1])
            replayed.fulfill()
        }
        engine.state = .stopped
        engine.onPlaybackFinished?()
        await fulfillment(of: [replayed], timeout: 1)
    }

    func testClearInvalidatesQueuedCompletion() async throws {
        let engine = PlaybackStub()
        let controller = PlaybackController(engine: engine)
        controller.addToPlaylist(urls: urls)
        try await controller.play()
        engine.loaded = { _ in XCTFail("Cleared queue must not restart") }
        engine.onPlaybackFinished?()
        controller.clearPlaylist()
        for _ in 0..<5 { await Task.yield() }
        XCTAssertEqual(controller.currentState, .stopped)
        XCTAssertEqual(controller.getPlaylistCount(), 0)
    }

    func testNextAndPreviousPastTheQueueEndsDoNothing() async throws {
        let engine = PlaybackStub()
        let controller = PlaybackController(engine: engine)
        controller.addToPlaylist(urls: urls)
        try await controller.previous()
        XCTAssertNil(engine.currentURL)
        try await controller.playItem(at: 2)
        try await controller.next()
        XCTAssertEqual(engine.currentURL, urls[2])
        XCTAssertEqual(controller.getCurrentPosition(), 2)
    }

    func testNetworkLibraryCannotShareLocalOrRelativeURLs() {
        let controller = PlaybackController(engine: PlaybackStub())
        for uri in ["file:///etc/passwd", "relative.flac", "ftp://nas/track.flac"] {
            let item = MediaObject(id: uri, parentID: "0", title: "Track", isContainer: false, resURL: uri)
            XCTAssertFalse(controller.addTrackToPlaylist(item))
        }
        XCTAssertEqual(controller.getPlaylistCount(), 0)
    }
}
