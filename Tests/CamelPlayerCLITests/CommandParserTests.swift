import XCTest
import CamelPlayerCore
@testable import CamelPlayerCLI

final class CommandParserTests: XCTestCase {
    private let parser = CommandParser()

    // MARK: - play

    func testPlayNoArgs() {
        guard case .play(let index) = parser.parse("play") else { return XCTFail() }
        XCTAssertNil(index)
    }

    func testPlayWithIndex() {
        guard case .play(let index) = parser.parse("play 3") else { return XCTFail() }
        XCTAssertEqual(index, 3)
    }

    func testPlayWithNonNumericFallsBackToAdd() {
        // "play song.mp3" is treated as an add, not a play-by-index.
        guard case .add(let paths) = parser.parse("play song.mp3") else { return XCTFail() }
        XCTAssertEqual(paths, ["song.mp3"])
    }

    func testPlayAlias() {
        guard case .play = parser.parse("p") else { return XCTFail() }
    }

    // MARK: - seek

    func testSeekSeconds() {
        guard case .seek(let t) = parser.parse("seek 90") else { return XCTFail() }
        XCTAssertEqual(t, 90, accuracy: 0.0001)
    }

    func testSeekMinutesSeconds() {
        guard case .seek(let t) = parser.parse("seek 1:30") else { return XCTFail() }
        XCTAssertEqual(t, 90, accuracy: 0.0001)
    }

    func testSeekInvalid() {
        guard case .unknown = parser.parse("seek abc") else { return XCTFail() }
    }

    func testSeekTooManyParts() {
        guard case .unknown = parser.parse("seek 1:2:3") else { return XCTFail() }
    }

    // MARK: - volume

    func testVolumeScaledToFraction() {
        guard case .volume(let level) = parser.parse("volume 50") else { return XCTFail() }
        XCTAssertEqual(level, 0.5, accuracy: 0.0001)
    }

    func testVolumeInvalid() {
        guard case .unknown = parser.parse("vol loud") else { return XCTFail() }
    }

    // MARK: - mode

    func testModeAliases() {
        let cases: [(String, PlaybackMode)] = [
            ("mode seq", .sequential),
            ("mode sequential", .sequential),
            ("mode loop", .loop),
            ("mode one", .loopOne),
            ("mode loopone", .loopOne),
            ("mode sh", .shuffle),
            ("mode shuffle", .shuffle)
        ]
        for (input, expected) in cases {
            guard case .mode(let mode) = parser.parse(input) else { return XCTFail(input) }
            XCTAssertEqual(mode, expected, input)
        }
    }

    func testModeInvalid() {
        guard case .unknown = parser.parse("mode backwards") else { return XCTFail() }
    }

    // MARK: - bitperfect

    func testBitPerfectNoArgToggles() {
        guard case .bitPerfect(let enabled) = parser.parse("bp") else { return XCTFail() }
        XCTAssertNil(enabled)
    }

    func testBitPerfectOn() {
        guard case .bitPerfect(let enabled) = parser.parse("bitperfect on") else { return XCTFail() }
        XCTAssertEqual(enabled, true)
    }

    func testBitPerfectOff() {
        guard case .bitPerfect(let enabled) = parser.parse("bp disable") else { return XCTFail() }
        XCTAssertEqual(enabled, false)
    }

    func testBitPerfectInvalid() {
        guard case .unknown = parser.parse("bp maybe") else { return XCTFail() }
    }

    // MARK: - remove / device

    func testRemoveValid() {
        guard case .remove(let index) = parser.parse("rm 2") else { return XCTFail() }
        XCTAssertEqual(index, 2)
    }

    func testRemoveInvalid() {
        guard case .unknown = parser.parse("remove x") else { return XCTFail() }
    }

    func testDeviceNoArg() {
        guard case .device(let id) = parser.parse("device") else { return XCTFail() }
        XCTAssertNil(id)
    }

    func testDeviceWithID() {
        guard case .device(let id) = parser.parse("dev 42") else { return XCTFail() }
        XCTAssertEqual(id, 42)
    }

    // MARK: - add path parsing

    func testAddSinglePath() {
        guard case .add(let paths) = parser.parse("add song.mp3") else { return XCTFail() }
        XCTAssertEqual(paths, ["song.mp3"])
    }

    func testAddMultiplePaths() {
        guard case .add(let paths) = parser.parse("add a.mp3 b.mp3") else { return XCTFail() }
        XCTAssertEqual(paths, ["a.mp3", "b.mp3"])
    }

    func testAddQuotedPathWithSpaces() {
        guard case .add(let paths) = parser.parse("add \"My Song.mp3\"") else { return XCTFail() }
        XCTAssertEqual(paths, ["My Song.mp3"])
    }

    func testAddEscapedSpace() {
        guard case .add(let paths) = parser.parse("add My\\ Song.mp3") else { return XCTFail() }
        XCTAssertEqual(paths, ["My Song.mp3"])
    }

    // MARK: - misc

    func testEmptyInput() {
        guard case .unknown(let text) = parser.parse("   ") else { return XCTFail() }
        XCTAssertEqual(text, "")
    }

    func testUnknownCommand() {
        guard case .unknown = parser.parse("frobnicate") else { return XCTFail() }
    }

    func testQuitAliases() {
        for input in ["quit", "q", "exit"] {
            guard case .quit = parser.parse(input) else { return XCTFail(input) }
        }
    }

    func testCaseInsensitiveCommand() {
        guard case .stop = parser.parse("STOP") else { return XCTFail() }
    }
}
