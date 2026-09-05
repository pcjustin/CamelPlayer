import XCTest
@testable import CamelPlayerCore

private final class TransportStub: AVTransportService {
    var transportState: TransportState = .playing
    var uri = "http://nas/current.flac"
    var preload: (() async throws -> Void)?

    init() { super.init(controlURL: "http://unused") }
    override func stop() async throws {}
    override func play(speed: String = "1") async throws {}
    override func setAVTransportURI(uri: String, metadata: String = "") async throws { self.uri = uri }
    override func setNextAVTransportURI(uri: String, metadata: String = "") async throws { try await preload?() }
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
}
