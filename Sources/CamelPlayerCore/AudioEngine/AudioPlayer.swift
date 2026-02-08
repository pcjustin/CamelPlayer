import AVFoundation
import CoreAudio
import Foundation

public enum PlaybackState {
    case stopped
    case playing
    case paused
}

public enum AudioPlayerError: Error {
    case fileNotFound
    case unsupportedFormat
    case audioEngineError(String)
    case fileLoadError(String)
}

public class AudioPlayer {
    private let engine = AVAudioEngine()
    private let playerNode = AVAudioPlayerNode()
    private var audioFile: AVAudioFile?
    private let deviceManager: OutputDeviceManager

    public private(set) var state: PlaybackState = .stopped
    public private(set) var currentURL: URL?
    public var bitPerfectMode: Bool = true

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
        return Double(playerTime.sampleTime) / sampleRate
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

    public func play() throws {
        guard let file = audioFile else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        if state == .paused {
            playerNode.play()
            state = .playing
            return
        }

        playerNode.stop()

        let mainMixer = engine.mainMixerNode
        let format = file.processingFormat

        if engine.isRunning {
            engine.stop()
        }

        if bitPerfectMode {
            do {
                let currentDeviceID = try deviceManager.getCurrentOutputDevice()
                let currentSampleRate = try deviceManager.getDeviceSampleRate(deviceID: currentDeviceID)
                let fileSampleRate = format.sampleRate

                if abs(currentSampleRate - fileSampleRate) > 0.1 {
                    try deviceManager.setDeviceSampleRate(deviceID: currentDeviceID, sampleRate: fileSampleRate)
                }
            } catch {
                print("Warning: Failed to set bit-perfect sample rate: \(error.localizedDescription)")
            }
        }

        engine.disconnectNodeOutput(playerNode)
        engine.connect(playerNode, to: mainMixer, format: format)

        playerNode.scheduleFile(file, at: nil) { [weak self] in
            DispatchQueue.main.async {
                self?.state = .stopped
            }
        }

        do {
            try engine.start()
        } catch {
            throw AudioPlayerError.audioEngineError("Failed to start audio engine: \(error.localizedDescription)")
        }

        playerNode.play()
        state = .playing
    }

    public func pause() {
        guard state == .playing else { return }
        playerNode.pause()
        state = .paused
    }

    public func stop() {
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

        playerNode.stop()

        let frameCount = AVAudioFrameCount(file.length - startFrame)

        playerNode.scheduleSegment(file,
                                   startingFrame: startFrame,
                                   frameCount: frameCount,
                                   at: nil) { [weak self] in
            DispatchQueue.main.async {
                self?.state = .stopped
            }
        }

        if wasPlaying {
            playerNode.play()
            state = .playing
        } else {
            state = .stopped
        }
    }

    deinit {
        stop()
        engine.stop()
    }
}
