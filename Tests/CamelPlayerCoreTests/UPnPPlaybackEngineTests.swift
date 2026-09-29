import XCTest
@testable import CamelPlayerCore

private final class TransportStub: AVTransportService {
    var transportState: TransportState = .playing
    var uri = "http://nas/current.flac"
    var preload: (() async throws -> Void)?
    var setURI: (() async throws -> Void)?
    var pauseAction: (() async throws -> Void)?
    var playCount = 0
    var nextURI: String?
    var metadata: String?

    init() { super.init(controlURL: "http://unused") }
    override func stop() async throws {}
    override func play(speed: String = "1") async throws { playCount += 1 }
    override func pause() async throws { try await pauseAction?() }
    override func setAVTransportURI(uri: String, metadata: String = "") async throws {
        try await setURI?()
        self.uri = uri
        self.metadata = metadata
    }
    override func setNextAVTransportURI(uri: String, metadata: String = "") async throws {
        try await preload?()
        nextURI = uri
    }
    override func getTransportState() async throws -> TransportState { transportState }
    override func getCurrentPosition() async throws -> PositionInfo {
        PositionInfo(duration: "0:03:00", position: "0:00:01", uri: uri)
    }
}

@MainActor
final class UPnPPlaybackEngineTests: XCTestCase {
    private let current = URL(string: "http://nas/current.flac")!
    private let next = URL(string: "http://nas/next.flac")!

    private func engine(_ transport: TransportStub) -> UPnPPlaybackEngine {
        let device = UPnPDevice(id: "test", friendlyName: "Test", manufacturer: "Test", modelName: "Test",
                                location: URL(string: "http://unused")!)
        return UPnPPlaybackEngine(device: device, mediaServer: LocalMediaServer(), avTransport: transport)
    }

    func testRejectedPreloadFallsBackToFinishExactlyOnce() async throws {
        let transport = TransportStub()
        transport.preload = { throw SOAPError.soapFault("Unsupported") }
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        await engine.preloadNextTrack(url: next, metadata: nil)
        let finished = expectation(description: "Fallback finish")
        finished.assertForOverFulfill = true
        engine.onPlaybackFinished = { finished.fulfill() }
        transport.transportState = .stopped
        await engine.updateStatus()
        await engine.updateStatus()
        await fulfillment(of: [finished], timeout: 1)
    }

    func testStopObservedDuringPendingPreloadStillFinishesAfterFailure() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        let pending = expectation(description: "Request pending")
        var reply: CheckedContinuation<Void, Error>?
        transport.preload = {
            try await withCheckedThrowingContinuation { continuation in
                reply = continuation
                pending.fulfill()
            }
        }
        let request = Task { await engine.preloadNextTrack(url: next, metadata: nil) }
        await fulfillment(of: [pending], timeout: 1)
        let finished = expectation(description: "Finish after failure")
        engine.onPlaybackFinished = { finished.fulfill() }
        transport.transportState = .stopped
        await engine.updateStatus()
        reply?.resume(throwing: SOAPError.soapFault("Rejected"))
        await request.value
        await engine.updateStatus()
        await fulfillment(of: [finished], timeout: 1)
    }

    func testSuccessfulPreloadAdvancesWithoutFinish() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        await engine.preloadNextTrack(url: next, metadata: nil)
        engine.onPlaybackFinished = { XCTFail("Gapless transition must not finish") }
        let advanced = expectation(description: "Gapless advance")
        engine.onAdvancedToNext = { advanced.fulfill() }
        transport.transportState = .stopped
        await engine.updateStatus()
        transport.transportState = .playing
        transport.uri = next.absoluteString
        await engine.updateStatus()
        await fulfillment(of: [advanced], timeout: 1)
        XCTAssertEqual(engine.currentURL, next)
    }

    func testStopDuringLoadCannotRestartPlayback() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        let pending = expectation(description: "URI request pending")
        var reply: CheckedContinuation<Void, Never>?
        transport.setURI = {
            await withCheckedContinuation { reply = $0; pending.fulfill() }
        }
        let load = Task { try await engine.loadAndPlay(url: current, metadata: nil) }
        await fulfillment(of: [pending], timeout: 1)
        engine.stop()
        XCTAssertEqual(engine.state, .stopped)
        reply?.resume()
        try await load.value
        XCTAssertEqual(engine.state, .stopped)
        XCTAssertEqual(transport.playCount, 0)
    }

    func testSupersededLoadDoesNotPlayOrOverwriteNewTrack() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        let pending = expectation(description: "First URI pending")
        var reply: CheckedContinuation<Void, Never>?
        transport.setURI = {
            await withCheckedContinuation { reply = $0; pending.fulfill() }
        }
        let first = Task { try await engine.loadAndPlay(url: current, metadata: nil) }
        await fulfillment(of: [pending], timeout: 1)
        transport.setURI = nil
        let secondStarted = expectation(description: "Second load started")
        let second = Task {
            secondStarted.fulfill()
            try await engine.loadAndPlay(url: next, metadata: nil)
        }
        await fulfillment(of: [secondStarted], timeout: 1)
        reply?.resume()
        try await first.value
        try await second.value
        XCTAssertEqual(engine.currentURL, next)
        XCTAssertEqual(transport.uri, next.absoluteString)
        XCTAssertEqual(transport.playCount, 1)
        XCTAssertEqual(engine.state, .playing)
    }

    func testAcceptedButUnusedPreloadEventuallyFinishes() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        await engine.preloadNextTrack(url: next, metadata: nil)
        var finishes = 0
        engine.onPlaybackFinished = { finishes += 1 }
        transport.transportState = .stopped
        await engine.updateStatus()
        XCTAssertEqual(finishes, 0)
        await engine.updateStatus()
        await engine.updateStatus()
        XCTAssertEqual(finishes, 1)
    }

    func testTransitioningAndUnknownDoNotReportNaturalCompletion() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        engine.onPlaybackFinished = { XCTFail("Playback has not started") }
        transport.transportState = .transitioning
        await engine.updateStatus()
        transport.transportState = .stopped
        await engine.updateStatus()
        transport.transportState = .unknown
        await engine.updateStatus()
    }

    func testLoadFailureStopsAndClearsOldPosition() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        XCTAssertEqual(engine.duration, 180)
        transport.setURI = { throw SOAPError.soapFault("Rejected") }
        do {
            try await engine.loadAndPlay(url: next, metadata: nil)
            XCTFail("Expected renderer failure")
        } catch {}
        XCTAssertEqual(engine.state, .stopped)
        XCTAssertEqual(engine.currentTime, 0)
        XCTAssertNil(engine.duration)
    }

    func testResumeAfterPauseDuringLoadRestoresRequestedTrack() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        let pending = expectation(description: "URI request pending")
        var reply: CheckedContinuation<Void, Never>?
        transport.setURI = {
            await withCheckedContinuation { reply = $0; pending.fulfill() }
        }
        let load = Task { try await engine.loadAndPlay(url: next, metadata: "track metadata") }
        await fulfillment(of: [pending], timeout: 1)
        engine.pause()
        transport.setURI = nil
        reply?.resume()
        try await load.value
        XCTAssertEqual(engine.state, .paused)
        XCTAssertEqual(transport.playCount, 0)
        try await engine.play()
        XCTAssertEqual(engine.currentURL, next)
        XCTAssertEqual(transport.uri, next.absoluteString)
        XCTAssertEqual(transport.metadata, "track metadata")
        XCTAssertEqual(transport.playCount, 1)
    }

    func testIdenticalNextURIUsesNormalCompletion() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        await engine.updateStatus()
        await engine.preloadNextTrack(url: current, metadata: nil)
        XCTAssertEqual(transport.nextURI, "")
        var finishes = 0
        engine.onPlaybackFinished = { finishes += 1 }
        transport.transportState = .stopped
        await engine.updateStatus()
        XCTAssertEqual(finishes, 1)
    }

    func testTrackEndingBeforeFirstPollStillFinishesOnce() async throws {
        let transport = TransportStub()
        let engine = engine(transport)
        try await engine.loadAndPlay(url: current, metadata: nil)
        var finishes = 0
        engine.onPlaybackFinished = { finishes += 1 }
        transport.transportState = .stopped
        await engine.updateStatus()
        XCTAssertEqual(finishes, 0)
        await engine.updateStatus()
        await engine.updateStatus()
        XCTAssertEqual(finishes, 1)
    }
}
