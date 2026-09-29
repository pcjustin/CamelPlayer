import XCTest
import Swifter
@testable import CamelPlayerCore

private final class BodyWriter: HttpResponseBodyWriter {
    var data = Data()
    func write(_ file: String.File) throws { XCTFail("Unexpected file writer") }
    func write(_ data: [UInt8]) throws { self.data.append(contentsOf: data) }
    func write(_ data: ArraySlice<UInt8>) throws { self.data.append(contentsOf: data) }
    func write(_ data: NSData) throws { self.data.append(data as Data) }
    func write(_ data: Data) throws { self.data.append(data) }
}

final class LocalMediaServerTests: XCTestCase {
    func testByteRangesAndHeadMatchTheServedBody() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".wav")
        try Data("0123456789".utf8).write(to: file)
        defer { try? FileManager.default.removeItem(at: file) }
        let server = LocalMediaServer()
        for (range, expected) in [("bytes=2-4", "234"), ("bytes=7-", "789"),
                                  ("bytes=-3", "789"), ("bytes=-99", "0123456789"),
                                  ("bytes=8-99999999999999999999", "89")] {
            let response = server.response(for: file, range: range)
            XCTAssertEqual(response.statusCode, 206, range)
            let writer = BodyWriter()
            if case .raw(_, _, _, let body) = response { try body?(writer) }
            XCTAssertEqual(String(decoding: writer.data, as: UTF8.self), expected, range)
            XCTAssertEqual(response.headers()["Content-Length"], String(expected.count))
        }
        let head = server.response(for: file, method: "HEAD", range: "bytes=2-4")
        XCTAssertEqual(head.statusCode, 200)
        XCTAssertEqual(head.headers()["Content-Length"], "10")
        if case .raw(_, _, _, let body) = head { XCTAssertNil(body) }
        for range in ["bytes=10-", "bytes=9-2", "bytes=-0", "bytes=99999999999999999999-"] {
            let response = server.response(for: file, range: range)
            XCTAssertEqual(response.statusCode, 416, range)
            XCTAssertEqual(response.headers()["Content-Range"], "bytes */10")
        }
        XCTAssertEqual(server.response(for: file, range: "invalid bytes=2-4").statusCode, 200)
        XCTAssertEqual(server.response(for: file, range: "bytes=0-1,4-5").statusCode, 200)
        XCTAssertEqual(server.response(for: file, method: "POST").statusCode, 405)
    }

    func testEmptyFileAndDirectory() throws {
        let file = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try Data().write(to: file)
        defer { try? FileManager.default.removeItem(at: file) }
        let server = LocalMediaServer()
        XCTAssertEqual(server.response(for: file).headers()["Content-Length"], "0")
        XCTAssertEqual(server.response(for: file, range: "bytes=0-").statusCode, 416)
        XCTAssertEqual(server.response(for: file.deletingLastPathComponent()).statusCode, 404)
    }
}
