import Foundation

/// UPnP-based playback engine for controlling remote MediaRenderer devices
public class UPnPPlaybackEngine: PlaybackEngine {
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

    public func loadAndPlay(url: URL) async throws {
        guard let avTransport = avTransport else {
            throw UPnPPlaybackError.serviceNotAvailable
        }

        // Stop current playback if any
        stopPolling()
        try? await avTransport.stop()

        // Share the file via HTTP
        sharedFileURL = try mediaServer.shareFile(url)
        _currentURL = url

        guard let httpURL = sharedFileURL else {
            throw UPnPPlaybackError.failedToShareFile
        }

        // Set URI and play
        try await avTransport.setAVTransportURI(uri: httpURL.absoluteString, metadata: "")
        try await avTransport.play()

        // Update state
        _state = .playing
        onStateChanged?(.playing)

        // Start polling for status
        startPolling()
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

        // Poll every second
        pollingTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task {
                await self?.updateStatus()
            }
        }
    }

    private func stopPolling() {
        pollingTimer?.invalidate()
        pollingTimer = nil
    }

    private func updateStatus() async {
        guard let avTransport = avTransport else { return }

        do {
            // Get transport state
            let transportState = try await avTransport.getTransportState()
            let newState = convertTransportState(transportState)

            if newState != _state {
                _state = newState
                DispatchQueue.main.async { [weak self] in
                    guard let self = self else { return }
                    self.onStateChanged?(newState)

                    // Check if playback finished
                    if newState == .stopped && self._state != .stopped {
                        self.onPlaybackFinished?()
                    }
                }
            }

            // Get position info if playing
            if newState == .playing || newState == .paused {
                let positionInfo = try await avTransport.getCurrentPosition()
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
            print("UPnP: Failed to update status: \(error)")
        }
    }

    private func convertTransportState(_ transportState: AVTransportService.TransportState) -> PlaybackState {
        switch transportState {
        case .playing:
            return .playing
        case .paused:
            return .paused
        case .stopped, .noMediaPresent, .transitioning, .unknown:
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
