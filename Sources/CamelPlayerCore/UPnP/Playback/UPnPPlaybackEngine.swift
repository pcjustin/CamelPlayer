import Foundation

/// Playback mutations run on the UI thread; asynchronous commands use the main actor.
public class UPnPPlaybackEngine: PlaybackEngine, @unchecked Sendable {
    private let mediaServer: LocalMediaServer
    private let avTransport: AVTransportService?

    public private(set) var state: PlaybackState = .stopped
    public private(set) var currentURL: URL?
    public private(set) var duration: TimeInterval?
    public private(set) var currentTime: TimeInterval = 0

    private var pollingTimer: Timer?
    private var commandTail: Task<Void, Error>?
    private var generation = 0
    private var preloadGeneration = 0
    private var positionGeneration = 0
    private var hasStartedPlaying = false
    private var playAcknowledged = false
    private var isPolling = false
    private var isPreloading = false
    private var stoppedPolls = 0
    private var currentURI: String?
    private var currentMetadata: String?
    private var nextURI: String?
    private var nextOriginalURL: URL?

    public var onPlaybackFinished: (() -> Void)?
    public var onAdvancedToNext: (() -> Void)?

    public init(device: UPnPDevice, mediaServer: LocalMediaServer) {
        self.mediaServer = mediaServer
        avTransport = device.avTransportURL.map { AVTransportService(controlURL: $0) }
    }

    init(device: UPnPDevice, mediaServer: LocalMediaServer, avTransport: AVTransportService) {
        self.mediaServer = mediaServer
        self.avTransport = avTransport
    }

    private func enqueue(_ operation: @escaping @MainActor () async throws -> Void) -> Task<Void, Error> {
        let previous = commandTail
        let task = Task { @MainActor in
            _ = try? await previous?.value
            try await operation()
        }
        commandTail = task
        return task
    }

    private func clearNext() {
        preloadGeneration += 1
        nextURI = nil
        nextOriginalURL = nil
        isPreloading = false
    }

    @MainActor
    public func loadAndPlay(url: URL, metadata: String?) async throws {
        guard let transport = avTransport else { throw UPnPPlaybackError.serviceNotAvailable }
        generation += 1
        let request = generation
        hasStartedPlaying = false
        playAcknowledged = false
        stoppedPolls = 0
        stopPolling()
        clearNext()
        currentURL = url
        currentMetadata = metadata
        currentURI = nil
        currentTime = 0
        duration = nil
        state = .playing

        let command = enqueue { [self] in
            guard request == generation else { throw CancellationError() }
            try? await transport.stop()
            guard request == generation else { throw CancellationError() }
            let uri = try resourceURI(for: url)
            try await transport.setAVTransportURI(uri: uri, metadata: metadata ?? "")
            guard request == generation else { throw CancellationError() }
            currentURI = uri
            try await transport.play()
        }
        do {
            try await command.value
            guard request == generation else { return }
            playAcknowledged = true
            state = .playing
            startPolling()
        } catch {
            guard request == generation else { return }
            state = .stopped
            throw error
        }
    }

    private func resourceURI(for url: URL) throws -> String {
        if url.isFileURL { return try mediaServer.shareFile(url).absoluteString }
        guard url.isHTTP else { throw AudioPlayerError.fileLoadError("Invalid media URL") }
        return url.absoluteString
    }

    public func setNextTrack(url: URL?, metadata: String?) {
        let trackGeneration = generation
        preloadGeneration += 1
        let request = preloadGeneration
        Task { @MainActor [weak self] in
            guard let self = self, trackGeneration == self.generation,
                  request == self.preloadGeneration else { return }
            await self.preloadNextTrack(url: url, metadata: metadata, request: request)
        }
    }

    @MainActor
    func preloadNextTrack(url: URL?, metadata: String?) async {
        preloadGeneration += 1
        await preloadNextTrack(url: url, metadata: metadata, request: preloadGeneration)
    }

    @MainActor
    private func preloadNextTrack(url: URL?, metadata: String?, request: Int) async {
        guard let transport = avTransport else { return }
        let trackGeneration = generation
        nextURI = nil
        nextOriginalURL = nil
        isPreloading = true
        do {
            // Identical URIs cannot identify a gapless transition in position
            // reports. Use normal completion for repeat-one and duplicate files.
            let preloadURL = url == currentURL ? nil : url
            let uri = try preloadURL.map(resourceURI) ?? ""
            nextURI = preloadURL == nil ? nil : uri
            nextOriginalURL = preloadURL
            try await enqueue { [self] in
                guard trackGeneration == generation, request == preloadGeneration else { return }
                try await transport.setNextAVTransportURI(uri: uri, metadata: preloadURL == nil ? "" : metadata ?? "")
            }.value
            guard trackGeneration == generation, request == preloadGeneration else { return }
            isPreloading = false
        } catch {
            guard trackGeneration == generation, request == preloadGeneration else { return }
            clearNext()
            coreLog("UPnP: Preloading failed; falling back to normal track advance: \(error)")
        }
    }

    @MainActor
    public func play() async throws {
        guard let transport = avTransport else { throw UPnPPlaybackError.serviceNotAvailable }
        // A pause may invalidate a load before SetAVTransportURI completes.
        if currentURI == nil, let url = currentURL {
            try await loadAndPlay(url: url, metadata: currentMetadata)
            return
        }
        generation += 1
        let request = generation
        isPreloading = false
        try await enqueue { [self] in
            guard request == generation else { return }
            try await transport.play()
        }.value
        guard request == generation else { return }
        stoppedPolls = 0
        playAcknowledged = true
        state = .playing
        startPolling()
    }

    public func pause() {
        guard let transport = avTransport, state == .playing else { return }
        generation += 1
        let request = generation
        isPreloading = false
        stopPolling()
        state = .paused
        _ = enqueue { [self] in
            guard request == generation else { return }
            do { try await transport.pause() }
            catch {
                guard request == generation else { return }
                startPolling()
                coreLog("UPnP: Pause failed: \(error)")
            }
        }
    }

    public func stop() {
        generation += 1
        let request = generation
        hasStartedPlaying = false
        playAcknowledged = false
        stopPolling()
        clearNext()
        currentTime = 0
        state = .stopped
        guard let transport = avTransport else { return }
        _ = enqueue { [self] in
            guard request == generation else { return }
            try await transport.stop()
        }
    }

    @MainActor
    public func seek(to time: TimeInterval) async throws {
        guard let transport = avTransport else { throw UPnPPlaybackError.serviceNotAvailable }
        guard time.isFinite, time >= 0, Int(exactly: time.rounded(.down)) != nil else {
            throw AudioPlayerError.invalidSeekTime
        }
        let target = duration.map { min(time, $0) } ?? time
        positionGeneration += 1
        let seekRequest = positionGeneration
        let request = generation
        try await enqueue { [self] in
            guard request == generation else { return }
            try await transport.seek(to: target)
        }.value
        guard request == generation, seekRequest == positionGeneration else { return }
        currentTime = target
    }

    public func getFileFormat() -> String? {
        currentURL.map { "\($0.pathExtension.uppercased()) (via UPnP)" }
    }

    @MainActor
    private func startPolling() {
        stopPolling()
        pollingTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.updateStatus() }
        }
    }

    private func stopPolling() {
        pollingTimer?.invalidate()
        pollingTimer = nil
    }

    @MainActor
    func updateStatus() async {
        guard let transport = avTransport, !isPolling else { return }
        isPolling = true
        defer { isPolling = false }
        let request = generation
        let positionRequest = positionGeneration
        do {
            let transportState = try await transport.getTransportState()
            guard request == generation else { return }
            let newState: PlaybackState
            switch transportState {
            case .playing:
                hasStartedPlaying = true
                newState = .playing
            case .paused: newState = .paused
            case .transitioning: return
            case .unknown: return
            case .stopped, .noMediaPresent: newState = .stopped
            }
            stoppedPolls = newState == .stopped ? stoppedPolls + 1 : 0
            // Allow one transient STOPPED between gapless tracks. A renderer
            // may accept SetNextAVTransportURI without ever using it.
            // Very short tracks can finish between polls without ever reporting
            // PLAYING. Two STOPPED polls after an acknowledged Play cover that case.
            let playbackBegan = hasStartedPlaying || (playAcknowledged && stoppedPolls >= 2)
            let finished = playbackBegan && newState == .stopped && !isPreloading
                && (nextURI == nil || stoppedPolls >= 2)
            state = newState
            guard request == generation else { return }
            if finished {
                hasStartedPlaying = false
                playAcknowledged = false
                clearNext()
                stopPolling()
                onPlaybackFinished?()
                return
            }

            if newState == .playing || newState == .paused {
                let position = try await transport.getCurrentPosition()
                guard request == generation, positionRequest == positionGeneration else { return }
                var advanced = false
                if let next = nextURI, next != currentURI, position.trackURI == next {
                    currentURI = next
                    currentURL = nextOriginalURL ?? currentURL
                    clearNext()
                    advanced = true
                }
                currentTime = position.trackPosition
                duration = position.trackDuration > 0 ? position.trackDuration : nil
                if advanced { onAdvancedToNext?() }
            }
        } catch {
            coreLog("UPnP: Failed to update status: \(error)")
        }
    }

    deinit { pollingTimer?.invalidate() }
}

public enum UPnPPlaybackError: LocalizedError {
    case serviceNotAvailable

    public var errorDescription: String? { "This renderer does not accept playback commands" }
}
