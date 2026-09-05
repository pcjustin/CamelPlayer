import Foundation
#if os(macOS)
import AVFoundation
import CoreAudio
#else
import CAlsa
import CSndFile
#endif

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

#if os(macOS)

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

        playerNode.scheduleFile(file, at: nil, completionCallbackType: .dataPlayedBack) { [weak self] _ in
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
                                   at: nil, completionCallbackType: .dataPlayedBack) { [weak self] _ in
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
        playerNode.scheduleFile(file, at: nil, completionCallbackType: .dataPlayedBack) { [weak self] _ in
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

#else

// ALSA + libsndfile playback. Bit-perfect: the PCM device is opened at the
// file's sample rate with software resampling disabled, S32_LE preferred.
// libsndfile covers WAV/FLAC/MP3; M4A/ALAC files fail to open.
// Gapless: a preloaded next track with the same sample rate and channel
// count is swapped in at EOF without draining the PCM; format changes go
// through the normal finish path.
public class AudioPlayer {
    private let cond = NSCondition()
    /// Bumped to signal the feeder thread to exit. Guarded by cond, as are
    /// state, feederRunning and framesPlayed.
    private var generation = 0
    private var feederRunning = false

    private var file: OpaquePointer?
    private var fileInfo = SF_INFO()
    private var pcm: OpaquePointer?
    private var useS16 = false
    /// File frames handed to ALSA; currentTime subtracts the driver delay.
    private var framesPlayed: sf_count_t = 0

    /// Preloaded next track (gapless). Guarded by cond.
    private var nextFile: OpaquePointer?
    private var nextInfo = SF_INFO()
    private var nextURL: URL?

    private var pcmNames = ["default"]
    private var currentDevice: AudioDeviceID = 0
    private var configuredRate: Float64 = 0

    public private(set) var state: PlaybackState = .stopped
    public private(set) var currentURL: URL?
    public var onPlaybackFinished: (() -> Void)?
    public var onAdvancedToNext: (() -> Void)?
    public var volume: Float = 1.0

    public init() throws {}

    public var duration: TimeInterval? {
        guard file != nil, fileInfo.samplerate > 0 else { return nil }
        return Double(fileInfo.frames) / Double(fileInfo.samplerate)
    }

    public var currentTime: TimeInterval {
        cond.lock()
        defer { cond.unlock() }
        guard fileInfo.samplerate > 0 else { return 0 }
        var played = framesPlayed
        if state == .playing, let pcm = pcm {
            var delay: snd_pcm_sframes_t = 0
            if snd_pcm_delay(pcm, &delay) == 0 && delay > 0 {
                played -= sf_count_t(delay)
            }
        }
        return max(0, Double(played) / Double(fileInfo.samplerate))
    }

    // MARK: - Devices

    public func listOutputDevices() throws -> [AudioDevice] {
        var names = ["default"]
        var labels = ["System default"]
        var hints: UnsafeMutablePointer<UnsafeMutableRawPointer?>? = nil
        if snd_device_name_hint(-1, "pcm", &hints) == 0, let hints = hints {
            defer { snd_device_name_free_hint(hints) }
            var i = 0
            while let hint = hints[i] {
                i += 1
                guard let cName = snd_device_name_get_hint(hint, "NAME") else { continue }
                let name = String(cString: cName)
                free(cName)
                guard name.hasPrefix("hw:") else { continue }
                if let cIoid = snd_device_name_get_hint(hint, "IOID") {
                    let ioid = String(cString: cIoid)
                    free(cIoid)
                    if ioid == "Input" { continue }
                }
                var label = name
                if let cDesc = snd_device_name_get_hint(hint, "DESC") {
                    label = String(cString: cDesc).components(separatedBy: "\n").first ?? name
                    free(cDesc)
                }
                names.append(name)
                labels.append(label)
            }
        }
        cond.lock()
        pcmNames = names
        cond.unlock()
        return labels.indices.map { AudioDevice(id: AudioDeviceID($0), name: labels[$0], isOutput: true) }
    }

    public func setOutputDevice(deviceID: AudioDeviceID) throws {
        cond.lock()
        defer { cond.unlock() }
        guard Int(deviceID) < pcmNames.count else {
            throw OutputDeviceError.deviceNotFound
        }
        currentDevice = deviceID
    }

    public func getCurrentOutputDevice() throws -> AudioDeviceID { currentDevice }

    public func getDefaultOutputDevice() throws -> AudioDeviceID { 0 }

    public func getCurrentDeviceSampleRate() throws -> Float64 { configuredRate }

    public func getFileSampleRate() -> Float64? {
        file != nil ? Float64(fileInfo.samplerate) : nil
    }

    public func getFileFormat() -> String? {
        guard file != nil else { return nil }
        let bits: Int
        switch Int(fileInfo.format) & Int(SF_FORMAT_SUBMASK) {
        case Int(SF_FORMAT_PCM_S8), Int(SF_FORMAT_PCM_U8): bits = 8
        case Int(SF_FORMAT_PCM_16): bits = 16
        case Int(SF_FORMAT_PCM_24): bits = 24
        case Int(SF_FORMAT_PCM_32), Int(SF_FORMAT_FLOAT): bits = 32
        default: bits = 0
        }
        return "\(fileInfo.samplerate) Hz / \(bits) bit / \(fileInfo.channels)ch"
    }

    // MARK: - Transport

    public func load(url: URL) throws {
        stop()
        try openFile(url: url)
    }

    public func loadAndPlay(url: URL) throws {
        stop()
        // Set playing up front so the UI never observes a stopped state.
        cond.lock(); state = .playing; cond.unlock()
        do {
            try openFile(url: url)
            try openPCM()
        } catch {
            cond.lock(); state = .stopped; cond.unlock()
            throw error
        }
        startFeeder()
    }

    public func play() throws {
        if state == .paused {
            guard let pcm = pcm else {
                throw AudioPlayerError.audioEngineError("No PCM device open")
            }
            snd_pcm_prepare(pcm)
            cond.lock(); state = .playing; cond.unlock()
            startFeeder()
            return
        }

        guard let file = file else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }

        // Stopped: restart the loaded track from the beginning.
        cond.lock(); state = .playing; cond.unlock()
        do {
            if pcm == nil {
                try openPCM()
            } else {
                snd_pcm_prepare(pcm)
            }
        } catch {
            cond.lock(); state = .stopped; cond.unlock()
            throw error
        }
        sf_seek(file, 0, SEEK_SET)
        cond.lock(); framesPlayed = 0; cond.unlock()
        startFeeder()
    }

    public func pause() {
        cond.lock()
        guard state == .playing else { cond.unlock(); return }
        state = .paused
        generation += 1
        cond.broadcast()
        while feederRunning { cond.wait() }
        cond.unlock()

        // Rewind the file position by what ALSA had buffered but not played,
        // so resume continues from the audible position.
        if let pcm = pcm {
            var delay: snd_pcm_sframes_t = 0
            snd_pcm_delay(pcm, &delay)
            snd_pcm_drop(pcm)
            if delay > 0 { framesPlayed = max(0, framesPlayed - sf_count_t(delay)) }
        }
        if let file = file { sf_seek(file, framesPlayed, SEEK_SET) }
    }

    public func stop() {
        cond.lock()
        generation += 1
        state = .stopped
        cond.broadcast()
        while feederRunning { cond.wait() }
        if let next = nextFile { sf_close(next) }
        nextFile = nil
        nextURL = nil
        cond.unlock()
        if let pcm = pcm { snd_pcm_drop(pcm) }
        if let file = file { sf_seek(file, 0, SEEK_SET) }
        framesPlayed = 0
    }

    public func seek(to time: TimeInterval) throws {
        guard let file = file else {
            throw AudioPlayerError.fileLoadError("No audio file loaded")
        }
        let target = sf_count_t(time * Double(fileInfo.samplerate))
        guard target >= 0 && target < fileInfo.frames else { return }

        let wasPlaying = state == .playing
        cond.lock()
        generation += 1
        cond.broadcast()
        while feederRunning { cond.wait() }
        cond.unlock()

        if let pcm = pcm {
            snd_pcm_drop(pcm)
            if wasPlaying { snd_pcm_prepare(pcm) }
        }
        sf_seek(file, target, SEEK_SET)
        cond.lock(); framesPlayed = target; cond.unlock()
        if wasPlaying { startFeeder() }
    }

    public func setNextTrack(url: URL?) {
        cond.lock()
        defer { cond.unlock() }
        if url == nextURL { return }
        if let next = nextFile { sf_close(next) }
        nextFile = nil
        nextURL = nil
        guard let url = url, url.isFileURL, file != nil else { return }
        var info = SF_INFO()
        guard let handle = sf_open(url.path, Int32(SFM_READ), &info) else { return }
        if info.samplerate == fileInfo.samplerate && info.channels == fileInfo.channels {
            nextFile = handle
            nextInfo = info
            nextURL = url
        } else {
            sf_close(handle)
        }
    }

    // MARK: - Internals

    private func openFile(url: URL) throws {
        guard url.isFileURL, FileManager.default.fileExists(atPath: url.path) else {
            throw AudioPlayerError.fileNotFound
        }
        closeFile()
        var info = SF_INFO()
        guard let handle = sf_open(url.path, Int32(SFM_READ), &info) else {
            throw AudioPlayerError.fileLoadError(String(cString: sf_strerror(nil)))
        }
        file = handle
        fileInfo = info
        currentURL = url
        framesPlayed = 0
    }

    private func openPCM() throws {
        closePCM()
        cond.lock()
        let device = Int(currentDevice) < pcmNames.count ? pcmNames[Int(currentDevice)] : "default"
        cond.unlock()

        var handle: OpaquePointer?
        let rc = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)
        guard rc >= 0, let handle = handle else {
            throw AudioPlayerError.audioEngineError(
                "snd_pcm_open(\(device)): \(String(cString: snd_strerror(rc)))")
        }

        let rate = UInt32(fileInfo.samplerate)
        let channels = UInt32(fileInfo.channels)
        var status = snd_pcm_set_params(
            handle, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
            channels, rate, 0, 200_000)
        useS16 = status < 0
        if status < 0 {
            status = snd_pcm_set_params(
                handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                channels, rate, 0, 200_000)
        }
        guard status >= 0 else {
            snd_pcm_close(handle)
            throw AudioPlayerError.audioEngineError(
                "Device \(device) rejected \(rate) Hz: \(String(cString: snd_strerror(status)))")
        }
        pcm = handle
        configuredRate = Float64(rate)
    }

    private func closeFile() {
        if let file = file { sf_close(file) }
        file = nil
        fileInfo = SF_INFO()
        currentURL = nil
    }

    private func closePCM() {
        if let pcm = pcm { snd_pcm_close(pcm) }
        pcm = nil
        configuredRate = 0
    }

    private func startFeeder() {
        cond.lock()
        let gen = generation
        feederRunning = true
        cond.unlock()
        let thread = Thread { [weak self] in self?.feederLoop(gen) }
        thread.name = "alsa-feeder"
        thread.start()
    }

    /// Reads file chunks and writes them to ALSA until the track ends or the
    /// generation changes. Owns the file and pcm handles while running;
    /// control methods kill it (generation bump + wait) before touching them.
    private func feederLoop(_ gen: Int) {
        defer {
            cond.lock()
            feederRunning = false
            cond.broadcast()
            cond.unlock()
        }
        guard var currentFile = file, let pcm = pcm else { return }

        let chunkFrames = 4096
        let channels = Int(fileInfo.channels)
        var buffer = [Int32](repeating: 0, count: chunkFrames * channels)
        var out16 = [Int16](repeating: 0, count: useS16 ? chunkFrames * channels : 0)

        while true {
            cond.lock()
            let live = gen == generation && state == .playing
            let vol = volume
            cond.unlock()
            guard live else { return }

            let frames = sf_readf_int(currentFile, &buffer, sf_count_t(chunkFrames))
            if frames <= 0 {
                // Gapless: swap in the preloaded next track without draining,
                // so ALSA keeps rendering continuously.
                cond.lock()
                if gen == generation, let next = nextFile {
                    sf_close(currentFile)
                    file = next
                    fileInfo = nextInfo
                    currentURL = nextURL
                    nextFile = nil
                    nextURL = nil
                    framesPlayed = 0
                    currentFile = next
                    cond.unlock()
                    DispatchQueue.main.async { [weak self] in self?.onAdvancedToNext?() }
                    continue
                }
                cond.unlock()

                snd_pcm_drain(pcm)
                var finished = false
                cond.lock()
                if gen == generation {
                    state = .stopped
                    finished = true
                }
                cond.unlock()
                if finished {
                    DispatchQueue.main.async { [weak self] in self?.onPlaybackFinished?() }
                }
                return
            }

            let sampleCount = Int(frames) * channels
            if vol < 0.999 {
                let factor = Double(vol)
                for i in 0..<sampleCount {
                    buffer[i] = Int32(clamping: Int64(Double(buffer[i]) * factor))
                }
            }
            if useS16 {
                for i in 0..<sampleCount {
                    out16[i] = Int16(truncatingIfNeeded: buffer[i] >> 16)
                }
            }

            var offset = 0
            while offset < Int(frames) {
                cond.lock()
                let stillLive = gen == generation && state == .playing
                cond.unlock()
                guard stillLive else { return }

                let remaining = snd_pcm_uframes_t(Int(frames) - offset)
                let rc: snd_pcm_sframes_t
                if useS16 {
                    rc = out16.withUnsafeBytes {
                        snd_pcm_writei(pcm, $0.baseAddress!.advanced(by: offset * channels * 2), remaining)
                    }
                } else {
                    rc = buffer.withUnsafeBytes {
                        snd_pcm_writei(pcm, $0.baseAddress!.advanced(by: offset * channels * 4), remaining)
                    }
                }
                if rc == -snd_pcm_sframes_t(EPIPE) {
                    snd_pcm_prepare(pcm)
                    continue
                }
                if rc < 0 {
                    coreLog("ALSA: write failed: \(String(cString: snd_strerror(Int32(rc))))")
                    cond.lock()
                    if gen == generation { state = .stopped }
                    cond.unlock()
                    return
                }
                offset += Int(rc)
            }

            cond.lock()
            framesPlayed += frames
            cond.unlock()
        }
    }

    deinit {
        stop()
        closePCM()
        closeFile()
    }
}

#endif
