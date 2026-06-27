import Foundation

/// UPnP-based playback engine for controlling remote MediaRenderer devices
public class UPnPPlaybackEngine: PlaybackEngine, @unchecked Sendable {
    private let device: UPnPDevice
    private let mediaServer: LocalMediaServer
    private var avTransport: AVTransportService?
    private var renderingControl: RenderingControlService?

    private var _state: PlaybackState = .stopped
    private var _currentURL: URL?
    private var _duration: TimeInterval?
    private var _currentTime: TimeInterval = 0
    private var _volume: Float = 0.5

    private var pollingTimer: Timer?
    private var sharedFileURL: URL?
    /// Bumped on each loadAndPlay so a poll started before a track switch can't
    /// mistake our own stop() for the previous track finishing.
    private var generation = 0
    /// True once the renderer has confirmed PLAYING since the last loadAndPlay,
    /// so a transient STOPPED right after starting isn't read as a finish.
    private var hasStartedPlaying = false

    public var state: PlaybackState {
        _state
    }

    public var currentURL: URL? {
        _currentURL
    }

    public var duration: TimeInterval? {
        _duration
    }

    public var currentTime: TimeInterval {
        _currentTime
    }

    public var volume: Float {
        get {
            _volume
        }
        set {
            _volume = max(0, min(1, newValue))
            let volumeInt = Int(_volume * 100)
            Task {
                try? await renderingControl?.setVolume(volumeInt)
            }
        }
    }

    public var onPlaybackFinished: (() -> Void)?
    public var onStateChanged: ((PlaybackState) -> Void)?
    public var onAdvancedToNext: (() -> Void)?

    // Gapless: the URI currently set on the renderer, plus the preloaded next.
    private var currentURI: String?
    private var nextURI: String?
    private var nextOriginalURL: URL?

    public init(device: UPnPDevice, mediaServer: LocalMediaServer) {
        self.device = device
        self.mediaServer = mediaServer

        // Initialize services
        if let avTransportURL = device.avTransportURL {
            self.avTransport = AVTransportService(controlURL: avTransportURL)
        }

        if let renderingControlURL = device.renderingControlURL {
            self.renderingControl = RenderingControlService(controlURL: renderingControlURL)
        }
    }

    deinit {
        stopPolling()
    }

    // MARK: - PlaybackEngine Implementation

    public func loadAndPlay(url: URL, metadata: String?) async throws {
        guard let avTransport = avTransport else {
            throw UPnPPlaybackError.serviceNotAvailable
        }

        // Stop current playback if any. Bump the generation first so any poll
        // already in flight ignores the stop() below.
        generation += 1
        hasStartedPlaying = false
        stopPolling()
        try? await avTransport.stop()

        _currentURL = url

        // Local files are served over our own HTTP bridge; remote URLs (e.g. a
        // NAS/MinimServer res URL) are handed to the renderer as-is so it pulls
        // directly from the source.
        let uri: String
        if url.isFileURL {
            sharedFileURL = try mediaServer.shareFile(url)
            guard let httpURL = sharedFileURL else {
                throw UPnPPlaybackError.failedToShareFile
            }
            uri = httpURL.absoluteString
        } else {
            sharedFileURL = nil
            uri = url.absoluteString
        }

        // Set URI and play. DIDL metadata lets the renderer show track info.
        currentURI = uri
        nextURI = nil
        nextOriginalURL = nil
        try await avTransport.setAVTransportURI(uri: uri, metadata: metadata ?? "")
        try await avTransport.play()

        // Update state
        _state = .playing
        onStateChanged?(.playing)

        // Start polling for status
        startPolling()
    }

    public func setNextTrack(url: URL?, metadata: String?) {
        guard let avTransport = avTransport else { return }
        Task {
            guard let url = url else {
                nextURI = nil
                nextOriginalURL = nil
                try? await avTransport.setNextAVTransportURI(uri: "", metadata: "")
                return
            }
            let uri: String
            if url.isFileURL {
                guard let shared = try? mediaServer.shareFile(url) else { return }
                uri = shared.absoluteString
            } else {
                uri = url.absoluteString
            }
            nextURI = uri
            nextOriginalURL = url
            try? await avTransport.setNextAVTransportURI(uri: uri, metadata: metadata ?? "")
        }
    }

    public func play() async throws {
        guard let avTransport = avTransport else {
            throw UPnPPlaybackError.serviceNotAvailable
        }

        try await avTransport.play()
        _state = .playing
        onStateChanged?(.playing)
        startPolling()
    }

    public func pause() {
        guard let avTransport = avTransport else { return }

        Task {
            try? await avTransport.pause()
            _state = .paused
            onStateChanged?(.paused)
            stopPolling()
        }
    }

    public func stop() {
        guard let avTransport = avTransport else { return }

        Task {
            try? await avTransport.stop()
            _state = .stopped
            onStateChanged?(.stopped)
            stopPolling()
        }
    }

    public func seek(to time: TimeInterval) async throws {
        guard let avTransport = avTransport else {
            throw UPnPPlaybackError.serviceNotAvailable
        }

        try await avTransport.seek(to: time)
        _currentTime = time
    }

    public func getFileFormat() -> String? {
        // UPnP doesn't provide detailed format info easily
        guard let url = currentURL else { return nil }
        let ext = url.pathExtension.uppercased()
        return "\(ext) (via UPnP)"
    }

    // MARK: - Status Polling

    private func startPolling() {
        stopPolling()

        // Poll every second. Schedule on the main run loop so the timer fires
        // regardless of which thread loadAndPlay/play was invoked from.
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.pollingTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
                Task {
                    await self?.updateStatus()
                }
            }
        }
    }

    private func stopPolling() {
        pollingTimer?.invalidate()
        pollingTimer = nil
    }

    private func updateStatus() async {
        guard let avTransport = avTransport else { return }

        let myGeneration = generation
        do {
            // Get transport state
            let transportState = try await avTransport.getTransportState()
            // A track switch happened while this poll was in flight; its result
            // is stale (our own stop() looks like a finish), so ignore it.
            guard myGeneration == generation else { return }
            let newState = convertTransportState(transportState)
            if newState == .playing { hasStartedPlaying = true }

            let previousState = _state
            if newState != previousState {
                _state = newState

                // A finish is playing -> stopped, but only once the renderer has
                // actually started this track (otherwise a transient STOPPED at
                // start would be misread as the track finishing). When a gapless
                // next is queued, a stop is part of the transition, not a finish.
                let didFinish = hasStartedPlaying && previousState == .playing
                    && newState == .stopped && nextURI == nil
                if didFinish {
                    stopPolling()
                }

                DispatchQueue.main.async { [weak self] in
                    guard let self = self else { return }
                    self.onStateChanged?(newState)
                    if didFinish {
                        self.onPlaybackFinished?()
                    }
                }
            }

            // Get position info if playing
            if newState == .playing || newState == .paused {
                let positionInfo = try await avTransport.getCurrentPosition()
                guard myGeneration == generation else { return }

                // Gapless: the renderer moved on to the preloaded next track.
                if let next = nextURI, next != currentURI,
                   !positionInfo.trackURI.isEmpty, positionInfo.trackURI == next {
                    currentURI = next
                    _currentURL = nextOriginalURL ?? _currentURL
                    nextURI = nil
                    nextOriginalURL = nil
                    DispatchQueue.main.async { [weak self] in self?.onAdvancedToNext?() }
                }

                _currentTime = positionInfo.trackPosition
                _duration = positionInfo.trackDuration > 0 ? positionInfo.trackDuration : nil
            }

            // Get volume if available
            if let renderingControl = renderingControl {
                let volumeInt = try? await renderingControl.getVolume()
                if let volumeInt = volumeInt {
                    _volume = Float(volumeInt) / 100.0
                }
            }

        } catch {
            coreLog("UPnP: Failed to update status: \(error)")
        }
    }

    private func convertTransportState(_ transportState: AVTransportService.TransportState) -> PlaybackState {
        switch transportState {
        case .playing:
            return .playing
        case .paused:
            return .paused
        case .transitioning:
            // Transitioning is an active, in-progress state (e.g. buffering at
            // start or while seeking); treat it as playing so it is not mistaken
            // for the track having finished.
            return .playing
        case .stopped, .noMediaPresent, .unknown:
            return .stopped
        }
    }
}

// MARK: - UPnP Playback Errors

public enum UPnPPlaybackError: Error {
    case serviceNotAvailable
    case failedToShareFile
    case deviceNotResponding
}
