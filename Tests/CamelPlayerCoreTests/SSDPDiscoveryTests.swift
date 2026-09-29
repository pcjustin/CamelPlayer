import XCTest
@testable import CamelPlayerCore

final class SSDPDiscoveryTests: XCTestCase {
    private let response = "HTTP/1.1 200 OK\r\nLOCATION: http://nas/desc.xml\r\nUSN: uuid:test::device\r\nST: MediaRenderer\r\n\r\n"
    private let deviceXML = Data("""
    <root><device><friendlyName>Test</friendlyName><serviceList><service>
    <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
    <controlURL>/control</controlURL></service></serviceList></device></root>
    """.utf8)

    func testDuplicateResponsesUseOneFetchAndStopDiscardsPendingResult() async {
        let pending = expectation(description: "Description requested once")
        pending.assertForOverFulfill = true
        let finished = expectation(description: "Fetch resumed")
        let lock = NSLock()
        var reply: CheckedContinuation<Data, Never>?
        let discovery = SSDPDiscovery { _ in
            let result = await withCheckedContinuation { continuation in
                lock.lock(); reply = continuation; lock.unlock()
                pending.fulfill()
            }
            finished.fulfill()
            return result
        }
        discovery.receive(response)
        discovery.receive(response)
        await fulfillment(of: [pending], timeout: 1)
        discovery.stopDiscovery()
        let continuation = lockedContinuation(lock, &reply)
        continuation?.resume(returning: deviceXML)
        await fulfillment(of: [finished], timeout: 1)
        for _ in 0..<5 { await Task.yield() }
        XCTAssertTrue(discovery.getDiscoveredDevices().isEmpty)
    }

    private func lockedContinuation(_ lock: NSLock, _ reply: inout CheckedContinuation<Data, Never>?) -> CheckedContinuation<Data, Never>? {
        lock.lock()
        defer { lock.unlock() }
        return reply
    }
}
