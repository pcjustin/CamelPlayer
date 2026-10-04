import XCTest
#if os(macOS)
import CoreAudio
#endif
@testable import CamelPlayerCore

final class AudioFormatDescriptionTests: XCTestCase {
    func testLossyCodecsOmitTheBitDepth() {
        XCTAssertEqual(audioFormatDescription(sampleRate: 96000, bitDepth: 24, channels: 2), "96000 Hz / 24 bit / 2ch")
        XCTAssertEqual(audioFormatDescription(sampleRate: 44100, bitDepth: 0, channels: 2), "44100 Hz / 2ch")
    }

    #if os(macOS)
    func testLosslessCodecsReportTheirSourceBitDepth() {
        func format(_ id: AudioFormatID, flags: AudioFormatFlags = 0, bits: UInt32 = 0) -> AudioStreamBasicDescription {
            var format = AudioStreamBasicDescription()
            format.mFormatID = id
            format.mFormatFlags = flags
            format.mBitsPerChannel = bits
            return format
        }
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatFLAC, flags: 1)), 16)
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatFLAC, flags: 3)), 24)
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatAppleLossless, flags: 4)), 32)
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatLinearPCM, flags: 12, bits: 24)), 24)
        // AAC keeps its MPEG-4 object type in the flags; that is not a bit depth.
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatMPEG4AAC, flags: 2)), 0)
        XCTAssertEqual(AudioPlayer.sourceBitDepth(format(kAudioFormatMPEGLayer3)), 0)
    }
    #endif
}
