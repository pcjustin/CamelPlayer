import XCTest
@testable import CamelPlayerCore

final class AVTransportServiceTests: XCTestCase {
    func testPositionRejectsInvalidNetworkTimes() {
        for value in ["0:00:nan", "inf:00:00", "-1:00:00", "1e308:00:00", "0:60:00", "1:02"] {
            let info = AVTransportService.PositionInfo(duration: value, position: value, uri: "")
            XCTAssertEqual(info.trackDuration, 0, value)
            XCTAssertEqual(info.trackPosition, 0, value)
        }
        let info = AVTransportService.PositionInfo(duration: "1:02:03.5", position: "0:00:05.25", uri: "")
        XCTAssertEqual(info.trackDuration, 3723.5)
        XCTAssertEqual(info.trackPosition, 5.25)
    }

    func testInvalidSeekFailsBeforeNetworkRequest() async {
        let transport = AVTransportService(controlURL: "http://unused")
        for time in [Double.nan, .infinity, -1, Double.greatestFiniteMagnitude] {
            do {
                try await transport.seek(to: time)
                XCTFail("Invalid seek must fail")
            } catch AudioPlayerError.invalidSeekTime {
            } catch {
                XCTFail("Unexpected error: \(error)")
            }
        }
    }
}
