import SwiftUI
import Foundation
import CamelPlayerCore
import CoreAudio
import AVFoundation

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
    @Published var outputDevices: [OutputDevice] = []
    @Published var currentOutputDevice: OutputDevice?
    @Published var errorMessage: String?
    @Published var showError: Bool = false
    @Published var formatInfo: String?
    @Published var lastError: String?
    @Published var albumArt: NSImage?
    @Published var mediaServers: [UPnPDevice] = []

    // Private properties
    private let controller: PlaybackController
    private var updateTimer: Timer?
    private let updateInterval: TimeInterval = 0.1 // 100ms
    private var lastLoadedCoverPath: String?
    private var hasRestoredOutputDevice = false

    // MARK: - Persisted settings

    private enum Keys {
        static let volume = "settings.volume"
        static let bitPerfect = "settings.bitPerfect"
        static let playbackMode = "settings.playbackMode"
        static let outputDeviceID = "settings.outputDeviceID"
    }

    private static func mode(from string: String?) -> PlaybackMode? {
        switch string {
        case "sequential": return .sequential
        case "loop": return .loop
        case "loopOne": return .loopOne
        case "shuffle": return .shuffle
        default: return nil
        }
    }

    private static func string(for mode: PlaybackMode) -> String {
        switch mode {
        case .sequential: return "sequential"
        case .loop: return "loop"
        case .loopOne: return "loopOne"
        case .shuffle: return "shuffle"
        }
    }

    // Initialization
    init() {
        do {
            controller = try PlaybackController()
            let defaults = UserDefaults.standard
            controller.volume = (defaults.object(forKey: Keys.volume) as? Double).map(Float.init) ?? 0.7
            controller.bitPerfectMode = defaults.object(forKey: Keys.bitPerfect) as? Bool ?? true
            controller.playbackMode = Self.mode(from: defaults.string(forKey: Keys.playbackMode)) ?? .sequential
            controller.onUPnPDevicesChanged = { [weak self] in
                Task { @MainActor in self?.refreshDevices() }
            }
            controller.onUPnPServersChanged = { [weak self] in
                Task { @MainActor in self?.refreshMediaServers() }
            }
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

        // Skip if we already loaded cover from this file
        if lastLoadedCoverPath == currentItem.url.path {
            return
        }

        let folderURL = currentItem.url.deletingLastPathComponent()

        // 1. First, look for cover.jpg or cover.jpeg in the same folder (faster)
        let coverNames = ["cover.jpg", "cover.jpeg", "Cover.jpg", "Cover.jpeg"]
        for coverName in coverNames {
            let coverURL = folderURL.appendingPathComponent(coverName)
            if FileManager.default.fileExists(atPath: coverURL.path) {
                if let image = NSImage(contentsOf: coverURL) {
                    albumArt = image
                    lastLoadedCoverPath = currentItem.url.path
                    return
                }
            }
        }

        // 2. If no external cover found, try to load embedded artwork from file metadata
        if let embeddedArt = loadEmbeddedArtwork(from: currentItem.url) {
            albumArt = embeddedArt
            lastLoadedCoverPath = currentItem.url.path
            return
        }

        // 3. No cover found
        albumArt = nil
        lastLoadedCoverPath = currentItem.url.path
    }

    private func loadEmbeddedArtwork(from url: URL) -> NSImage? {
        let asset = AVAsset(url: url)

        // Get all metadata formats
        for format in asset.availableMetadataFormats {
            let metadata = asset.metadata(forFormat: format)

            // Search for artwork
            for item in metadata {
                // Try commonKeyArtwork (standard key)
                if item.commonKey == .commonKeyArtwork {
                    if let data = item.dataValue {
                        return NSImage(data: data)
                    }
                }

                // Also try other possible keys
                if let key = item.key as? String,
                   (key.lowercased().contains("artwork") ||
                    key.lowercased().contains("picture") ||
                    key == "covr") {
                    if let data = item.dataValue {
                        return NSImage(data: data)
                    }
                }
            }
        }

        return nil
    }

    private func loadInitialState() {
        updateState()
        refreshDevices()
        refreshMediaServers()
    }

    // MARK: - Playback Control

    func play() {
        Task {
            do {
                try await controller.play()
                // Update immediately so the UI doesn't wait for the poll timer.
                updateState()
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    func pause() {
        controller.pause()
        // Update immediately so the UI doesn't wait for the poll timer.
        updateState()
    }

    func togglePlayPause() {
        if isPlaying {
            pause()
        } else if isPaused {
            resume()
        } else {
            play()
        }
    }

    func resume() {
        Task {
            do {
                try await controller.resume()
                // Update immediately so the UI doesn't wait for the poll timer.
                updateState()
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    func stop() {
        controller.stop()
        // Update immediately so the UI doesn't wait for the poll timer.
        updateState()
    }

    func next() {
        Task {
            do {
                try await controller.next()
                // Update immediately so the UI doesn't wait for the poll timer.
                updateState()
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    func previous() {
        Task {
            do {
                try await controller.previous()
                // Update immediately so the UI doesn't wait for the poll timer.
                updateState()
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    func seek(to time: TimeInterval) {
        Task {
            do {
                try await controller.seek(to: time)
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    // MARK: - Playlist Management

    func addFiles(_ urls: [URL]) {
        controller.addToPlaylist(urls: urls)
        updateState()
    }

    func playItem(at index: Int) {
        Task {
            do {
                try await controller.playItem(at: index)
                // Update immediately so the UI doesn't wait for the poll timer.
                updateState()
            } catch let error as AudioPlayerError {
                handleAudioPlayerError(error)
            } catch {
                handleError(error.localizedDescription)
            }
        }
    }

    func removeFromPlaylist(at index: Int) {
        controller.removeFromPlaylist(at: index)
        updateState()
    }

    func movePlaylistItem(fromOffsets: IndexSet, toOffset: Int) {
        controller.movePlaylistItem(fromOffsets: fromOffsets, toOffset: toOffset)
        updateState()
    }

    func savePlaylist() {
        guard let url = FilePickerHelper.savePlaylistPanel() else { return }
        do {
            try controller.exportPlaylist(to: url)
        } catch {
            handleError("Failed to save playlist: \(error.localizedDescription)")
        }
    }

    func loadPlaylist() {
        guard let url = FilePickerHelper.openPlaylistPanel() else { return }
        do {
            let count = try controller.importPlaylist(from: url)
            updateState()
            if count == 0 { handleError("No tracks in playlist file") }
        } catch {
            handleError("Failed to load playlist: \(error.localizedDescription)")
        }
    }

    func clearPlaylist() {
        controller.clearPlaylist()
        updateState()
    }

    // MARK: - Device Management

    func refreshDevices() {
        outputDevices = controller.listAllOutputDevices()
        currentOutputDevice = controller.currentOutputDevice
        restoreOutputDeviceIfNeeded()
    }

    func refreshUPnPDevices() {
        controller.refreshUPnPDevices()
        // Wait a bit for devices to be discovered, then update the list
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            self?.refreshDevices()
        }
    }

    // MARK: - Media Server Browsing

    func refreshMediaServers() {
        mediaServers = controller.availableMediaServers
    }

    func browse(
        server: UPnPDevice,
        objectID: String,
        startingIndex: Int = 0,
        requestedCount: Int = 200,
        sortCriteria: String = ""
    ) async -> PlaybackController.BrowsePage? {
        do {
            return try await controller.browse(
                server: server,
                objectID: objectID,
                startingIndex: startingIndex,
                requestedCount: requestedCount,
                sortCriteria: sortCriteria
            )
        } catch {
            handleError("Browse failed: \(error.localizedDescription)")
            return nil
        }
    }

    func search(
        server: UPnPDevice,
        query: String,
        startingIndex: Int = 0,
        requestedCount: Int = 200
    ) async -> PlaybackController.BrowsePage? {
        do {
            return try await controller.search(
                server: server, query: query,
                startingIndex: startingIndex, requestedCount: requestedCount
            )
        } catch {
            handleError("Search failed: \(error.localizedDescription)")
            return nil
        }
    }

    func addTrack(_ object: MediaObject) {
        if controller.addTrackToPlaylist(object) {
            updateState()
        } else {
            handleError("This item has no playable source")
        }
    }

    func sortCapabilities(server: UPnPDevice) async -> [String] {
        await controller.sortCapabilities(server: server)
    }

    // MARK: - Album-centric browsing

    private var albumArtCache: [String: String] = [:]

    /// The media server used for album browsing (first discovered for now).
    var libraryServer: UPnPDevice? { mediaServers.first }

    func albums(startingIndex: Int = 0, requestedCount: Int = 100) async -> PlaybackController.BrowsePage? {
        guard let server = libraryServer else { return nil }
        do {
            return try await controller.albums(server: server, startingIndex: startingIndex, requestedCount: requestedCount)
        } catch {
            handleError("Failed to load albums: \(error.localizedDescription)")
            return nil
        }
    }

    func albumArtURL(forAlbum id: String) async -> URL? {
        if let cached = albumArtCache[id] { return URL(string: cached) }
        guard let server = libraryServer,
              let uri = await controller.albumArtURI(server: server, objectID: id) else { return nil }
        albumArtCache[id] = uri
        return URL(string: uri)
    }

    func albumTracks(albumID: String) async -> [MediaObject] {
        guard let server = libraryServer,
              let page = await browse(server: server, objectID: albumID) else { return [] }
        return page.objects
    }

    func playAlbum(albumID: String) {
        guard let server = libraryServer else { return }
        Task {
            do {
                try await controller.playAlbum(server: server, objectID: albumID)
                updateState()
            } catch {
                handleError("Failed to play album: \(error.localizedDescription)")
            }
        }
    }

    func addContainerToPlaylist(server: UPnPDevice, objectID: String, sortCriteria: String = "") async {
        do {
            let count = try await controller.addContainerToPlaylist(
                server: server, objectID: objectID, sortCriteria: sortCriteria
            )
            updateState()
            if count == 0 {
                handleError("No playable tracks in this folder")
            }
        } catch {
            handleError("Failed to add folder: \(error.localizedDescription)")
        }
    }

    func setOutputDevice(_ device: OutputDevice) {
        do {
            try controller.setOutputDevice(device)
            currentOutputDevice = device
            UserDefaults.standard.set(device.id, forKey: Keys.outputDeviceID)
        } catch {
            handleError("Failed to set output device: \(error.localizedDescription)")
        }
    }

    private func restoreOutputDeviceIfNeeded() {
        guard !hasRestoredOutputDevice,
              let savedID = UserDefaults.standard.string(forKey: Keys.outputDeviceID),
              savedID != currentOutputDevice?.id,
              let device = outputDevices.first(where: { $0.id == savedID }) else { return }
        hasRestoredOutputDevice = true
        setOutputDevice(device)
    }

    // MARK: - Settings

    func setVolume(_ newVolume: Float) {
        controller.volume = newVolume
        volume = newVolume
        UserDefaults.standard.set(Double(newVolume), forKey: Keys.volume)
    }

    func setPlaybackMode(_ mode: PlaybackMode) {
        controller.playbackMode = mode
        playbackMode = mode
        UserDefaults.standard.set(Self.string(for: mode), forKey: Keys.playbackMode)
    }

    func setBitPerfectMode(_ enabled: Bool) {
        controller.bitPerfectMode = enabled
        bitPerfectMode = enabled
        UserDefaults.standard.set(enabled, forKey: Keys.bitPerfect)
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
        case .remoteURLRequiresRenderer:
            handleError("This track is on a network server. Choose a network renderer as the output device to play it.")
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

    /// True when the current track lives on a network server but the selected
    /// output is local — it can only play through a UPnP renderer.
    var currentTrackNeedsRenderer: Bool {
        guard let item = currentItem, !item.url.isFileURL else { return false }
        if case .local = currentOutputDevice?.type { return true }
        return false
    }
}
