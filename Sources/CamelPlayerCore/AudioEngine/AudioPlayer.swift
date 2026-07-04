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
    public var onPlaybackFinished: (() -> Void)?
    /// Fired when playback advanced gaplessly into the preloaded next track.
    public var onAdvancedToNext: (() -> Void)?

    // Gapless: the preloaded next track. Only files matching the current
    // format can be queued on the node; others go through the normal
    // finish path (a sample-rate switch needs a full engine reconnect).
    private var nextFile: AVAudioFile?
    private var nextURL: URL?
    private var nextScheduled = false

    /// Bumped on every (re)schedule and stop. AVAudioPlayerNode fires pending
    /// completion handlers when the node is stopped, so each handler captures
    /// the generation it was scheduled under and ignores the callback if it is
    /// no longer current (our own stop/seek/replay, not a natural finish).
    private var scheduleGeneration = 0

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

    /// Last position computed while the node was rendering; reported while
    /// paused, when playerTime(forNodeTime:) is unavailable.
    private var lastKnownTime: TimeInterval = 0

    public var currentTime: TimeInterval {
        guard let nodeTime = playerNode.lastRenderTime,
              let playerTime = playerNode.playerTime(forNodeTime: nodeTime),
              let file = audioFile else {
            return state == .stopped ? 0 : lastKnownTime
        }

        let sampleRate = file.processingFormat.sampleRate
        lastKnownTime = Double(playerTime.sampleTime + segmentStartFrame) / sampleRate
        return lastKnownTime
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

        // Invalidate any pending completion before stop() fires it.
        scheduleGeneration += 1
        let generation = scheduleGeneration

        playerNode.stop()
        // stop() cleared the node queue, so any preloaded next is gone.
        nextFile = nil
        nextURL = nil
        nextScheduled = false

        let mainMixer = engine.mainMixerNode
        let format = file.processingFormat
        let wasRunning = engine.isRunning

        if wasRunning {
            engine.stop()
        }

        engine.disconnectNodeOutput(playerNode)

        // Bit-perfect: match the device stream format to the file so playback
        // needs no resampling. Always on.
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

        engine.connect(playerNode, to: mainMixer, format: format)

        segmentStartFrame = 0
        lastKnownTime = 0

        playerNode.scheduleFile(file, at: nil) { [weak self] in
            DispatchQueue.main.async {
                self?.handleFileCompleted(generation: generation)
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
        scheduleGeneration += 1
        playerNode.stop()
        nextFile = nil
        nextURL = nil
        nextScheduled = false
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

        // Invalidate the pending completion before stop() fires it.
        scheduleGeneration += 1
        let generation = scheduleGeneration

        playerNode.stop()

        let frameCount = AVAudioFrameCount(file.length - startFrame)
        segmentStartFrame = startFrame
        lastKnownTime = time

        playerNode.scheduleSegment(file,
                                   startingFrame: startFrame,
                                   frameCount: frameCount,
                                   at: nil) { [weak self] in
            DispatchQueue.main.async {
                self?.handleFileCompleted(generation: generation)
            }
        }

        // stop() above cleared the node queue; requeue the preloaded next.
        nextScheduled = false
        scheduleNextIfNeeded()

        // Preserve the prior state: only resume the node if we were playing.
        if wasPlaying {
            if !engine.isRunning {
                try engine.start()
            }
            playerNode.play()
            state = .playing
        }
    }

    // MARK: - Gapless preloading

    /// Preloads the next track. Only local files whose format matches the
    /// current one are queued on the node; anything else clears the preload
    /// so that track goes through the normal finish path instead.
    public func setNextTrack(url: URL?) {
        if url == nextURL { return }

        let mustReanchor = nextScheduled
        // Clear the preload before re-anchoring so seek() doesn't requeue it.
        nextFile = nil
        nextURL = nil
        nextScheduled = false
        if mustReanchor {
            // The old next is already queued on the node and the only way to
            // unqueue it is to stop and re-anchor the current file.
            try? seek(to: currentTime)
        }

        guard let url = url, url.isFileURL,
              let current = audioFile,
              let file = try? AVAudioFile(forReading: url),
              abs(file.processingFormat.sampleRate - current.processingFormat.sampleRate) < 0.1,
              file.processingFormat.channelCount == current.processingFormat.channelCount else {
            return
        }

        nextFile = file
        nextURL = url
        scheduleNextIfNeeded()
    }

    private func scheduleNextIfNeeded() {
        guard !nextScheduled, let file = nextFile, state != .stopped else { return }
        nextScheduled = true
        let generation = scheduleGeneration
        playerNode.scheduleFile(file, at: nil) { [weak self] in
            DispatchQueue.main.async {
                self?.handleFileCompleted(generation: generation)
            }
        }
    }

    /// A scheduled file finished rendering: either hand over to the queued
    /// next track (the node is already playing it) or report the finish.
    private func handleFileCompleted(generation: Int) {
        guard scheduleGeneration == generation else { return }

        if let file = nextFile, let url = nextURL, nextScheduled {
            audioFile = file
            currentURL = url
            nextFile = nil
            nextURL = nil
            nextScheduled = false
            // Re-zero currentTime: playerTime keeps counting across queued
            // files, so offset by what has been rendered so far.
            if let nodeTime = playerNode.lastRenderTime,
               let playerTime = playerNode.playerTime(forNodeTime: nodeTime) {
                segmentStartFrame = -playerTime.sampleTime
            } else {
                segmentStartFrame = 0
            }
            lastKnownTime = 0
            onAdvancedToNext?()
        } else {
            state = .stopped
            onPlaybackFinished?()
        }
    }

    deinit {
        stop()
        engine.stop()
    }
}
