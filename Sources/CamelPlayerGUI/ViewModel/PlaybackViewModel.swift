import SwiftUI
import Foundation
import CamelPlayerCore
import CoreAudio

@MainActor
class PlaybackViewModel: ObservableObject {
    // Published properties (reactive)
    @Published var playbackState: PlaybackState = .stopped
    @Published var currentItem: PlaylistItem?
    @Published var playlistItems: [PlaylistItem] = []
    @Published var currentPosition: Int = -1
    @Published var currentTime: TimeInterval = 0
    @Published var duration: TimeInterval?
    @Published var volume: Float = 0.7
    @Published var playbackMode: PlaybackMode = .sequential
    @Published var bitPerfectMode: Bool = true
    @Published var audioDevices: [AudioDevice] = []
    @Published var currentDeviceID: AudioDeviceID?
    @Published var errorMessage: String?
    @Published var showError: Bool = false
    @Published var formatInfo: String?
    @Published var lastError: String?
    @Published var albumArt: NSImage?

    // Private properties
    private let controller: PlaybackController
    private var updateTimer: Timer?
    private let updateInterval: TimeInterval = 0.1 // 100ms
    private var lastLoadedCoverPath: String?

    // Initialization
    init() {
        do {
            controller = try PlaybackController()
            controller.volume = 0.7
            controller.bitPerfectMode = true
            loadInitialState()
            startPolling()
        } catch {
            fatalError("Failed to initialize PlaybackController: \(error)")
        }
    }

    deinit {
        updateTimer?.invalidate()
    }

    // MARK: - Timer Management

    private func startPolling() {
        updateTimer = Timer.scheduledTimer(
            withTimeInterval: updateInterval,
            repeats: true
        ) { [weak self] _ in
            Task { @MainActor in
                self?.updateState()
            }
        }
        RunLoop.current.add(updateTimer!, forMode: .common)
    }

    private func stopPolling() {
        updateTimer?.invalidate()
        updateTimer = nil
    }

    private func updateState() {
        playbackState = controller.currentState
        currentItem = controller.currentItem
        currentTime = controller.currentTime
        duration = controller.duration
        currentPosition = controller.getCurrentPosition()
        playlistItems = controller.getPlaylistItems()
        formatInfo = controller.getFileFormat()
        bitPerfectMode = controller.bitPerfectMode
        playbackMode = controller.playbackMode
        volume = controller.volume

        // Load album art if current track changed
        loadAlbumArt()
    }

    private func loadAlbumArt() {
        guard let currentItem = currentItem else {
            albumArt = nil
            lastLoadedCoverPath = nil
            return
        }

        let folderURL = currentItem.url.deletingLastPathComponent()
        let folderPath = folderURL.path

        // Skip if we already loaded cover from this folder
        if lastLoadedCoverPath == folderPath {
            return
        }

        // Look for cover.jpg or cover.jpeg in the same folder
        let coverNames = ["cover.jpg", "cover.jpeg", "Cover.jpg", "Cover.jpeg"]

        for coverName in coverNames {
            let coverURL = folderURL.appendingPathComponent(coverName)
            if FileManager.default.fileExists(atPath: coverURL.path) {
                if let image = NSImage(contentsOf: coverURL) {
                    albumArt = image
                    lastLoadedCoverPath = folderPath
                    return
                }
            }
        }

        // No cover found
        albumArt = nil
        lastLoadedCoverPath = folderPath
    }

    private func loadInitialState() {
        updateState()
        refreshDevices()
    }

    // MARK: - Playback Control

    func play() {
        do {
            try controller.play()
            // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
            updateState()
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    func pause() {
        controller.pause()
        // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
        updateState()
    }

    func resume() {
        do {
            try controller.resume()
            // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
            updateState()
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    func stop() {
        controller.stop()
        // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
        updateState()
    }

    func next() {
        do {
            try controller.next()
            // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
            updateState()
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    func previous() {
        do {
            try controller.previous()
            // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
            updateState()
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    func seek(to time: TimeInterval) {
        do {
            try controller.seek(to: time)
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    // MARK: - Playlist Management

    func addFiles(_ urls: [URL]) {
        controller.addToPlaylist(urls: urls)
        updateState()
    }

    func playItem(at index: Int) {
        do {
            try controller.playItem(at: index)
            // 立即更新狀態，避免 timer 延遲導致的 UI 不同步
            updateState()
        } catch let error as AudioPlayerError {
            handleAudioPlayerError(error)
        } catch {
            handleError(error.localizedDescription)
        }
    }

    func removeFromPlaylist(at index: Int) {
        controller.removeFromPlaylist(at: index)
        updateState()
    }

    func clearPlaylist() {
        controller.clearPlaylist()
        updateState()
    }

    // MARK: - Device Management

    func refreshDevices() {
        do {
            audioDevices = try controller.listOutputDevices()
            currentDeviceID = try controller.getCurrentOutputDevice()
        } catch {
            handleError("Failed to list audio devices: \(error.localizedDescription)")
        }
    }

    func setOutputDevice(_ deviceID: AudioDeviceID) {
        do {
            try controller.setOutputDevice(deviceID: deviceID)
            currentDeviceID = deviceID
        } catch {
            handleError("Failed to set output device: \(error.localizedDescription)")
        }
    }

    // MARK: - Settings

    func setVolume(_ newVolume: Float) {
        controller.volume = newVolume
        volume = newVolume
    }

    func setPlaybackMode(_ mode: PlaybackMode) {
        controller.playbackMode = mode
        playbackMode = mode
    }

    func setBitPerfectMode(_ enabled: Bool) {
        controller.bitPerfectMode = enabled
        bitPerfectMode = enabled
    }

    // MARK: - Error Handling

    private func handleAudioPlayerError(_ error: AudioPlayerError) {
        switch error {
        case .fileNotFound:
            handleError("File not found")
        case .unsupportedFormat:
            handleError("Unsupported audio format")
        case .audioEngineError(let msg):
            handleError("Audio engine error: \(msg)")
        case .fileLoadError(let msg):
            handleError("Failed to load file: \(msg)")
        }
    }

    private func handleError(_ message: String) {
        errorMessage = message
        showError = true
    }

    // MARK: - Computed Properties

    var canGoNext: Bool {
        let count = controller.getPlaylistCount()
        return currentPosition < count - 1 || playbackMode == .loop || playbackMode == .shuffle
    }

    var canGoPrevious: Bool {
        return currentPosition > 0
    }

    var isPlaying: Bool {
        return playbackState == .playing
    }

    var isPaused: Bool {
        return playbackState == .paused
    }

    var isStopped: Bool {
        return playbackState == .stopped
    }
}
