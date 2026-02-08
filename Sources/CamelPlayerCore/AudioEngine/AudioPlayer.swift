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
    private var monitorQueue: DispatchQueue?
    private var isMonitoring = false

    public private(set) var state: PlaybackState = .stopped
    public private(set) var currentURL: URL?
    public var bitPerfectMode: Bool = true
    public var onPlaybackFinished: (() -> Void)?
    private var isManuallyStopped = false

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

    /// 原子地加載並播放文件，避免中間狀態導致的 UI 閃爍
    public func loadAndPlay(url: URL) throws {
        guard FileManager.default.fileExists(atPath: url.path) else {
            throw AudioPlayerError.fileNotFound
        }

        // 立即設置狀態為 playing，避免 UI 讀取到 stopped 狀態
        state = .playing

        do {
            let file = try AVAudioFile(forReading: url)
            audioFile = file
            currentURL = url
            // 不設置 state = .stopped，保持 .playing
        } catch {
            state = .stopped
            throw AudioPlayerError.fileLoadError(error.localizedDescription)
        }

        // 調用內部播放邏輯
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
                print("Warning: Failed to set bit-perfect format: \(error.localizedDescription)")
                do {
                    let currentDeviceID = try deviceManager.getCurrentOutputDevice()
                    let currentSampleRate = try deviceManager.getDeviceSampleRate(deviceID: currentDeviceID)
                    let fileSampleRate = format.sampleRate

                    if abs(currentSampleRate - fileSampleRate) > 0.1 {
                        try deviceManager.setDeviceSampleRate(deviceID: currentDeviceID, sampleRate: fileSampleRate)
                        Thread.sleep(forTimeInterval: 0.15)
                    }
                } catch {
                    print("Warning: Fallback to sample rate only also failed: \(error.localizedDescription)")
                }
            }
        }

        engine.connect(playerNode, to: mainMixer, format: format)

        // 記錄當前文件URL，用於檢查 completion handler 是否對應當前播放
        let scheduledURL = currentURL

        playerNode.scheduleFile(file, at: nil) { [weak self] in
            DispatchQueue.main.async {
                guard let self = self else { return }

                // 只有在 completion handler 對應當前播放的文件時才處理
                // 避免舊文件的 completion handler 干擾新文件的播放狀態
                guard self.currentURL == scheduledURL else {
                    return
                }

                self.state = .stopped
                // 只有在非手動停止時才觸發自動播放下一首
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
        // 確保狀態為 playing（即使之前已設置，也重新確認）
        state = .playing
        startMonitoring()
    }

    public func play() throws {
        guard let _ = audioFile else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        if state == .paused {
            playerNode.play()
            state = .playing
            startMonitoring()
            return
        }

        // 提前設置狀態為 playing，避免 UI 讀取到中間的 stopped 狀態
        state = .playing
        try playInternal()
    }

    public func pause() {
        guard state == .playing else { return }
        playerNode.pause()
        state = .paused
        stopMonitoring()
    }

    public func stop() {
        isManuallyStopped = true
        playerNode.stop()
        state = .stopped
        stopMonitoring()
    }

    private func startMonitoring() {
        stopMonitoring()
        isMonitoring = true

        let queue = DispatchQueue(label: "com.camelplayer.monitor", qos: .userInitiated)
        monitorQueue = queue

        queue.async { [weak self] in
            while self?.isMonitoring == true {
                Thread.sleep(forTimeInterval: 0.1)
                self?.checkPlaybackStatus()
            }
        }
    }

    private func stopMonitoring() {
        isMonitoring = false
        monitorQueue = nil
    }

    private func checkPlaybackStatus() {
        guard state == .playing else {
            return
        }

        guard let duration = duration else {
            return
        }

        let current = currentTime

        if current >= duration - 0.1 {
            // Playback has finished
            isMonitoring = false
            state = .stopped

            // Call the callback directly (not on main thread in CLI apps)
            onPlaybackFinished?()
        }
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
                self?.onPlaybackFinished?()
            }
        }

        if wasPlaying {
            playerNode.play()
            state = .playing
            startMonitoring()
        } else {
            state = .stopped
        }
    }

    deinit {
        stopMonitoring()
        stop()
        engine.stop()
    }
}
