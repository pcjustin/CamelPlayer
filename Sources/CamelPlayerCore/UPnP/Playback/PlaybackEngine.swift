import Foundation

/// Protocol for abstracting different playback engines (local, UPnP, etc.)
public protocol PlaybackEngine: AnyObject {
    /// Current playback state
    var state: PlaybackState { get }

    /// Current playback URL
    var currentURL: URL? { get }

    /// Duration of current track (nil if unknown)
    var duration: TimeInterval? { get }

    /// Current playback position
    var currentTime: TimeInterval { get }

    /// Volume (0.0 to 1.0)
    var volume: Float { get set }

    /// Callback when playback finishes
    var onPlaybackFinished: (() -> Void)? { get set }

    /// Callback when state changes
    var onStateChanged: ((PlaybackState) -> Void)? { get set }

    /// Loads and plays a file atomically
    func loadAndPlay(url: URL) async throws

    /// Starts or resumes playback
    func play() async throws

    /// Pauses playback
    func pause()

    /// Stops playback
    func stop()

    /// Seeks to a specific time
    func seek(to time: TimeInterval) async throws

    /// Gets the file format description
    func getFileFormat() -> String?
}

// MARK: - Local Playback Engine

/// Adapter for AudioPlayer to conform to PlaybackEngine protocol
public class LocalPlaybackEngine: PlaybackEngine {
    private let audioPlayer: AudioPlayer

    public var state: PlaybackState {
        audioPlayer.state
    }

    public var currentURL: URL? {
        audioPlayer.currentURL
    }

    public var duration: TimeInterval? {
        audioPlayer.duration
    }

    public var currentTime: TimeInterval {
        audioPlayer.currentTime
    }

    public var volume: Float {
        get {
            audioPlayer.mixerNode.outputVolume
        }
        set {
            audioPlayer.mixerNode.outputVolume = newValue
        }
    }

    public var onPlaybackFinished: (() -> Void)? {
        get {
            audioPlayer.onPlaybackFinished
        }
        set {
            audioPlayer.onPlaybackFinished = newValue
        }
    }

    public var onStateChanged: ((PlaybackState) -> Void)?

    public init(audioPlayer: AudioPlayer) {
        self.audioPlayer = audioPlayer
    }

    public func loadAndPlay(url: URL) async throws {
        try audioPlayer.loadAndPlay(url: url)
        onStateChanged?(audioPlayer.state)
    }

    public func play() async throws {
        try audioPlayer.play()
        onStateChanged?(audioPlayer.state)
    }

    public func pause() {
        audioPlayer.pause()
        onStateChanged?(audioPlayer.state)
    }

    public func stop() {
        audioPlayer.stop()
        onStateChanged?(audioPlayer.state)
    }

    public func seek(to time: TimeInterval) async throws {
        try audioPlayer.seek(to: time)
        onStateChanged?(audioPlayer.state)
    }

    public func getFileFormat() -> String? {
        audioPlayer.getFileFormat()
    }

    /// Gets the underlying AudioPlayer for local-specific operations
    public func getAudioPlayer() -> AudioPlayer {
        return audioPlayer
    }
}
