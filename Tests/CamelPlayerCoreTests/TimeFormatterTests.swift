import XCTest
@testable import CamelPlayerCore

final class TimeFormatterTests: XCTestCase {
    func testUnderOneHour() {
        XCTAssertEqual(TimeFormatter.formatTime(0), "0:00")
        XCTAssertEqual(TimeFormatter.formatTime(5), "0:05")
        XCTAssertEqual(TimeFormatter.formatTime(65), "1:05")
        XCTAssertEqual(TimeFormatter.formatTime(599), "9:59")
    }

    func testHourBoundary() {
        XCTAssertEqual(TimeFormatter.formatTime(3600), "1:00:00")
        XCTAssertEqual(TimeFormatter.formatTime(3661), "1:01:01")
        XCTAssertEqual(TimeFormatter.formatTime(36000), "10:00:00")
    }

    func testTruncatesFraction() {
        XCTAssertEqual(TimeFormatter.formatTime(65.9), "1:05")
    }

    func testNonFiniteReturnsZero() {
        XCTAssertEqual(TimeFormatter.formatTime(.nan), "0:00")
        XCTAssertEqual(TimeFormatter.formatTime(.infinity), "0:00")
    }
}
