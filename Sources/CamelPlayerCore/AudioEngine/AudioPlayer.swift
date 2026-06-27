import AVFoundation
import CoreAudio
import Foundation

public enum PlaybackState: Sendable {
    case stopped
    case playing
    case paused
}

public enum AudioPlayerError: Error {
    case fileNotFound
    case unsupportedFormat
    case audioEngineError(String)
    case fileLoadError(String)
    case remoteURLRequiresRenderer
}

public class AudioPlayer {
    private let engine = AVAudioEngine()
    private let playerNode = AVAudioPlayerNode()
    private var audioFile: AVAudioFile?
    private let deviceManager: OutputDeviceManager

    public private(set) var state: PlaybackState = .stopped
    public private(set) var currentURL: URL?
    public var bitPerfectMode: Bool = true
    public var onPlaybackFinished: (() -> Void)?
    private var isManuallyStopped = false

    /// Frame offset of the currently scheduled segment, used so currentTime
    /// reflects the absolute position after a seek.
    private var segmentStartFrame: AVAudioFramePosition = 0

    public var mixerNode: AVAudioMixerNode {
        engine.mainMixerNode
    }

    public var duration: TimeInterval? {
        guard let file = audioFile else { return nil }
        let sampleRate = file.processingFormat.sampleRate
        let frameCount = Double(file.length)
        return frameCount / sampleRate
    }

    public var currentTime: TimeInterval {
        guard let nodeTime = playerNode.lastRenderTime,
              let playerTime = playerNode.playerTime(forNodeTime: nodeTime),
              let file = audioFile else {
            return 0
        }

        let sampleRate = file.processingFormat.sampleRate
        return Double(playerTime.sampleTime + segmentStartFrame) / sampleRate
    }

    public init() throws {
        deviceManager = OutputDeviceManager(engine: engine)
        engine.attach(playerNode)
    }

    public func listOutputDevices() throws -> [AudioDevice] {
        try deviceManager.listOutputDevices()
    }

    public func setOutputDevice(deviceID: AudioDeviceID) throws {
        try deviceManager.setOutputDevice(deviceID: deviceID)
    }

    public func getCurrentOutputDevice() throws -> AudioDeviceID {
        try deviceManager.getCurrentOutputDevice()
    }

    public func getDefaultOutputDevice() throws -> AudioDeviceID {
        try deviceManager.getDefaultOutputDevice()
    }

    public func getCurrentDeviceSampleRate() throws -> Float64 {
        try deviceManager.getCurrentDeviceSampleRate()
    }

    public func getFileSampleRate() -> Float64? {
        audioFile?.processingFormat.sampleRate
    }

    public func getFileFormat() -> String? {
        guard let file = audioFile else { return nil }
        let format = file.processingFormat
        let sampleRate = Int(format.sampleRate)
        let bitDepth = format.settings[AVLinearPCMBitDepthKey] as? Int ?? 0
        let channels = Int(format.channelCount)
        return "\(sampleRate) Hz / \(bitDepth) bit / \(channels)ch"
    }

    public func load(url: URL) throws {
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw AudioPlayerError.fileNotFound
        }

        do {
            let file = try AVAudioFile(forReading: url)
            audioFile = file
            currentURL = url
            state = .stopped
        } catch {
            throw AudioPlayerError.fileLoadError(error.localizedDescription)
        }
    }

    /// Loads and plays a file atomically, avoiding an intermediate stopped
    /// state that would make the UI flicker.
    public func loadAndPlay(url: URL) throws {
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw AudioPlayerError.fileNotFound
        }

        // Set playing up front so the UI never observes a stopped state.
        state = .playing

        do {
            let file = try AVAudioFile(forReading: url)
            audioFile = file
            currentURL = url
        } catch {
            state = .stopped
            throw AudioPlayerError.fileLoadError(error.localizedDescription)
        }

        try playInternal()
    }

    private func playInternal() throws {
        guard let file = audioFile else {
            state = .stopped
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        playerNode.stop()

        let mainMixer = engine.mainMixerNode
        let format = file.processingFormat
        let wasRunning = engine.isRunning

        if wasRunning {
            engine.stop()
        }

        engine.disconnectNodeOutput(playerNode)

        if bitPerfectMode {
            do {
                let currentDeviceID = try deviceManager.getCurrentOutputDevice()

                let currentFormat = try deviceManager.getDeviceStreamFormat(deviceID: currentDeviceID)
                let fileSampleRate = format.sampleRate

                let needsFormatChange = abs(currentFormat.mSampleRate - fileSampleRate) > 0.1

                if needsFormatChange {
                    try deviceManager.setDeviceStreamFormat(deviceID: currentDeviceID, format: format)
                    Thread.sleep(forTimeInterval: 0.15)
                }
            } catch {
                coreLog("Warning: Failed to set bit-perfect format: \(error.localizedDescription)")
                do {
                    let currentDeviceID = try deviceManager.getCurrentOutputDevice()
                    let currentSampleRate = try deviceManager.getDeviceSampleRate(deviceID: currentDeviceID)
                    let fileSampleRate = format.sampleRate

                    if abs(currentSampleRate - fileSampleRate) > 0.1 {
                        try deviceManager.setDeviceSampleRate(deviceID: currentDeviceID, sampleRate: fileSampleRate)
                        Thread.sleep(forTimeInterval: 0.15)
                    }
                } catch {
                    coreLog("Warning: Fallback to sample rate only also failed: \(error.localizedDescription)")
                }
            }
        }

        engine.connect(playerNode, to: mainMixer, format: format)

        segmentStartFrame = 0

        // Capture the URL so a stale completion handler from a previous file
        // can't clobber the state of the current playback.
        let scheduledURL = currentURL

        playerNode.scheduleFile(file, at: nil) { [weak self] in
            DispatchQueue.main.async {
                guard let self = self else { return }

                guard self.currentURL == scheduledURL else {
                    return
                }

                self.state = .stopped
                // Only auto-advance when playback ended on its own.
                if !self.isManuallyStopped {
                    self.onPlaybackFinished?()
                }
                self.isManuallyStopped = false
            }
        }

        do {
            try engine.start()
        } catch {
            state = .stopped
            throw AudioPlayerError.audioEngineError("Failed to start audio engine: \(error.localizedDescription)")
        }

        playerNode.play()
        state = .playing
    }

    public func play() throws {
        guard let _ = audioFile else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        if state == .paused {
            playerNode.play()
            state = .playing
            return
        }

        // Set playing up front so the UI never observes a stopped state.
        state = .playing
        try playInternal()
    }

    public func pause() {
        guard state == .playing else { return }
        playerNode.pause()
        state = .paused
    }

    public func stop() {
        isManuallyStopped = true
        playerNode.stop()
        state = .stopped
    }

    public func seek(to time: TimeInterval) throws {
        guard let file = audioFile else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        let sampleRate = file.processingFormat.sampleRate
        let startFrame = AVAudioFramePosition(time * sampleRate)

        guard startFrame >= 0 && startFrame < file.length else {
            return
        }

        let wasPlaying = state == .playing
        let scheduledURL = currentURL

        playerNode.stop()

        let frameCount = AVAudioFrameCount(file.length - startFrame)
        segmentStartFrame = startFrame

        playerNode.scheduleSegment(file,
                                   startingFrame: startFrame,
                                   frameCount: frameCount,
                                   at: nil) { [weak self] in
            DispatchQueue.main.async {
                guard let self = self else { return }
                // Ignore completions from a segment that is no longer current.
                guard self.currentURL == scheduledURL else { return }

                self.state = .stopped
                if !self.isManuallyStopped {
                    self.onPlaybackFinished?()
                }
                self.isManuallyStopped = false
            }
        }

        // Preserve the prior state: only resume the node if we were playing.
        if wasPlaying {
            if !engine.isRunning {
                try engine.start()
            }
            playerNode.play()
            state = .playing
        }
    }

    deinit {
        stop()
        engine.stop()
    }
}
