import XCTest
@testable import CamelPlayerCore

final class PCMConversionTests: XCTestCase {
    func testFloatingPointAudioKeepsItsLevel() {
        XCTAssertEqual(PCMConversion.int32(0), 0)
        XCTAssertEqual(PCMConversion.int32(0.5), 1_073_741_824)
        XCTAssertEqual(PCMConversion.int32(-0.5), -1_073_741_824)
        XCTAssertEqual(PCMConversion.int32(1), .max)
        XCTAssertEqual(PCMConversion.int32(-1), .min)
    }

    func testOverrangeAndNonfiniteSamplesDoNotTrap() {
        XCTAssertEqual(PCMConversion.int32(2), .max)
        XCTAssertEqual(PCMConversion.int32(-2), .min)
        for sample in [Double.nan, .infinity, -.infinity] {
            XCTAssertEqual(PCMConversion.int32(sample), 0)
        }
    }
}
