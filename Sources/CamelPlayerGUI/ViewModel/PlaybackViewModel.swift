import SwiftUI
import Foundation
import CamelPlayerCore
import CoreAudio
import AVFoundation
import MediaPlayer

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
    @Published var shuffle: Bool = false
    @Published var loopMode: LoopMode = .off
    @Published var bitPerfectMode: Bool = true
    @Published var outputDevices: [OutputDevice] = []
    @Published var currentOutputDevice: OutputDevice?
    @Published var errorMessage: String?
    @Published var showError: Bool = false
    @Published var formatInfo: String?
    @Published var lastError: String?
    @Published var albumArt: NSImage?
    @Published var currentAlbum: String?
    @Published var currentCoverURL: URL?
    @Published var mediaServers: [UPnPDevice] = []
    @Published var libraryServerID: String? = UserDefaults.standard.string(forKey: Keys.libraryServerID)
    @Published var favoriteAlbums: [AlbumRef] = []
    @Published var favoriteTracks: [TrackRef] = []
    @Published var recentAlbums: [AlbumRef] = []
    @Published var recentTracks: [TrackRef] = []

    // Private properties
    private let controller: PlaybackController
    private var updateTimer: Timer?
    private let updateInterval: TimeInterval = 0.1 // 100ms
    private var lastLoadedCoverPath: String?
    private var hasRestoredOutputDevice = false
    private var currentArtist: String?
    private var lastNowPlayingKey: String?

    // MARK: - Persisted settings

    private enum Keys {
        static let volume = "settings.volume"
        static let shuffle = "settings.shuffle"
        static let loopMode = "settings.loopMode"
        static let outputDeviceID = "settings.outputDeviceID"
        static let queuePosition = "queue.position"
        static let libraryServerID = "settings.libraryServerID"
        static let favoriteAlbums = "library.favoriteAlbums"
        static let favoriteTracks = "library.favoriteTracks"
        static let recentAlbums = "library.recentAlbums"
        static let recentTracks = "library.recentTracks"
    }

    private let recentLimit = 50
    private var lastRecordedURL: String?

    private static func loopMode(from string: String?) -> LoopMode {
        switch string {
        case "all": return .all
        case "one": return .one
        default: return .off
        }
    }

    private static func string(for loop: LoopMode) -> String {
        switch loop {
        case .off: return "off"
        case .all: return "all"
        case .one: return "one"
        }
    }

    // Initialization
    init() {
        do {
            controller = try PlaybackController()
            let defaults = UserDefaults.standard
            controller.volume = (defaults.object(forKey: Keys.volume) as? Double).map(Float.init) ?? 1.0
            controller.bitPerfectMode = true
            controller.shuffle = defaults.bool(forKey: Keys.shuffle)
            controller.loopMode = Self.loopMode(from: defaults.string(forKey: Keys.loopMode))
            controller.onUPnPDevicesChanged = { [weak self] in
                Task { @MainActor in self?.refreshDevices() }
            }
            controller.onUPnPServersChanged = { [weak self] in
                Task { @MainActor in self?.refreshMediaServers() }
            }
            loadInitialState()
            setupRemoteCommands()
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
        let position = controller.getCurrentPosition()
        if position != currentPosition {
            currentPosition = position
            UserDefaults.standard.set(position, forKey: Keys.queuePosition)
        }
        // Reassign only on change: publishing a fresh array every 100ms makes
        // SwiftUI re-diff the whole list constantly. Persist the queue on the
        // same signal so it survives quits and crashes.
        let items = controller.getPlaylistItems()
        if items.map(\.id) != playlistItems.map(\.id) {
            playlistItems = items
            saveQueue()
        }
        formatInfo = controller.getFileFormat()
        bitPerfectMode = controller.bitPerfectMode
        shuffle = controller.shuffle
        loopMode = controller.loopMode
        volume = controller.volume

        // Record into recently played when the current track changes.
        if let item = currentItem, item.url.absoluteString != lastRecordedURL {
            lastRecordedURL = item.url.absoluteString
            recordTrackPlayed(item)
        }

        // Load album art if current track changed
        loadAlbumArt()

        // Refresh the system Now Playing panel only when state or track
        // changes; the system extrapolates elapsed time from the rate.
        let nowPlayingKey = "\(playbackState)-\(currentItem?.id.uuidString ?? "none")"
        if nowPlayingKey != lastNowPlayingKey {
            lastNowPlayingKey = nowPlayingKey
            updateNowPlaying()
        }
    }

    // MARK: - System Now Playing / media keys

    private func setupRemoteCommands() {
        let center = MPRemoteCommandCenter.shared()
        center.playCommand.addTarget { [weak self] _ in
            Task { @MainActor in
                guard let self = self, !self.isPlaying else { return }
                self.togglePlayPause()
            }
            return .success
        }
        center.pauseCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.pause() }
            return .success
        }
        center.togglePlayPauseCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.togglePlayPause() }
            return .success
        }
        center.nextTrackCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.next() }
            return .success
        }
        center.previousTrackCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.previous() }
            return .success
        }
        center.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let event = event as? MPChangePlaybackPositionCommandEvent else {
                return .commandFailed
            }
            Task { @MainActor in self?.seek(to: event.positionTime) }
            return .success
        }
    }

    private func updateNowPlaying() {
        let center = MPNowPlayingInfoCenter.default()
        guard let item = currentItem else {
            center.nowPlayingInfo = nil
            center.playbackState = .stopped
            return
        }

        var info: [String: Any] = [
            MPMediaItemPropertyTitle: item.title,
            MPNowPlayingInfoPropertyElapsedPlaybackTime: currentTime,
            MPNowPlayingInfoPropertyPlaybackRate: isPlaying ? 1.0 : 0.0
        ]
        if let album = currentAlbum, !album.isEmpty {
            info[MPMediaItemPropertyAlbumTitle] = album
        }
        if let artist = currentArtist, !artist.isEmpty {
            info[MPMediaItemPropertyArtist] = artist
        }
        if let duration = duration {
            info[MPMediaItemPropertyPlaybackDuration] = duration
        }
        // Local/embedded art, or a remote cover the UI has already cached.
        if let image = albumArt
            ?? currentCoverURL.flatMap({ ImageCache.memory.object(forKey: $0 as NSURL) }) {
            info[MPMediaItemPropertyArtwork] = MPMediaItemArtwork(boundsSize: image.size) { _ in image }
        }

        center.nowPlayingInfo = info
        center.playbackState = isPlaying ? .playing : (isPaused ? .paused : .stopped)
    }

    private func loadAlbumArt() {
        guard let currentItem = currentItem else {
            albumArt = nil
            currentAlbum = nil
            currentArtist = nil
            currentCoverURL = nil
            lastLoadedCoverPath = nil
            return
        }

        // Skip if we already loaded cover from this file
        if lastLoadedCoverPath == currentItem.url.path {
            return
        }

        // Album name + remote cover (for network tracks) come from the DIDL.
        let parsed = currentItem.metadata.flatMap { DIDLParser().parse($0).first }
        currentAlbum = parsed?.album
        currentArtist = parsed?.artist
        currentCoverURL = parsed?.albumArtURI.flatMap { URL(string: $0) }

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
        // AVAsset metadata loading is synchronous; for a remote URL it would
        // block the main thread on network I/O. Remote covers come from DIDL.
        guard url.isFileURL else { return nil }
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
        restoreQueue()
        updateState()
        refreshDevices()
        refreshMediaServers()
        loadCoverCache()
        loadFavorites()
        loadRecent()
    }

    // MARK: - Queue persistence

    private var queueFile: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CamelPlayer", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("queue.json")
    }

    private func saveQueue() {
        try? controller.exportPlaylist(to: queueFile)
    }

    private func restoreQueue() {
        let restored = (try? controller.importPlaylist(from: queueFile)) ?? 0
        guard restored > 0 else { return }
        let position = UserDefaults.standard.integer(forKey: Keys.queuePosition)
        controller.setPlaylistPosition(min(max(0, position), restored - 1))
        // Remember the restored track so it isn't re-recorded as newly played.
        lastRecordedURL = controller.currentItem?.url.absoluteString
    }

    // MARK: - Recently played

    private func loadRecent() {
        let defaults = UserDefaults.standard
        if let data = defaults.data(forKey: Keys.recentAlbums),
           let decoded = try? JSONDecoder().decode([AlbumRef].self, from: data) {
            recentAlbums = decoded
        }
        if let data = defaults.data(forKey: Keys.recentTracks),
           let decoded = try? JSONDecoder().decode([TrackRef].self, from: data) {
            recentTracks = decoded
        }
    }

    private func persistRecent() {
        let defaults = UserDefaults.standard
        if let data = try? JSONEncoder().encode(recentAlbums) {
            defaults.set(data, forKey: Keys.recentAlbums)
        }
        if let data = try? JSONEncoder().encode(recentTracks) {
            defaults.set(data, forKey: Keys.recentTracks)
        }
    }

    private func recordTrackPlayed(_ item: PlaylistItem) {
        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        let ref = TrackRef(
            url: item.url.absoluteString, title: item.title,
            album: parsed?.album, albumArtURI: parsed?.albumArtURI, metadata: item.metadata
        )
        recentTracks.removeAll { $0.url == ref.url }
        recentTracks.insert(ref, at: 0)
        if recentTracks.count > recentLimit { recentTracks = Array(recentTracks.prefix(recentLimit)) }
        persistRecent()
    }

    private func recordAlbumPlayed(_ album: MediaObject) {
        let ref = AlbumRef(album: album)
        recentAlbums.removeAll { $0.id == ref.id }
        recentAlbums.insert(ref, at: 0)
        if recentAlbums.count > recentLimit { recentAlbums = Array(recentAlbums.prefix(recentLimit)) }
        persistRecent()
    }

    func clearRecentlyPlayed() {
        recentTracks = []
        recentAlbums = []
        persistRecent()
    }

    // MARK: - Favorites

    private func loadFavorites() {
        let defaults = UserDefaults.standard
        if let data = defaults.data(forKey: Keys.favoriteAlbums),
           let decoded = try? JSONDecoder().decode([AlbumRef].self, from: data) {
            favoriteAlbums = decoded
        }
        if let data = defaults.data(forKey: Keys.favoriteTracks),
           let decoded = try? JSONDecoder().decode([TrackRef].self, from: data) {
            favoriteTracks = decoded
        }
    }

    private func persistFavorites() {
        let defaults = UserDefaults.standard
        if let data = try? JSONEncoder().encode(favoriteAlbums) {
            defaults.set(data, forKey: Keys.favoriteAlbums)
        }
        if let data = try? JSONEncoder().encode(favoriteTracks) {
            defaults.set(data, forKey: Keys.favoriteTracks)
        }
    }

    func isFavoriteAlbum(_ id: String) -> Bool {
        favoriteAlbums.contains { $0.id == id }
    }

    func toggleFavoriteAlbum(_ album: MediaObject) {
        if let index = favoriteAlbums.firstIndex(where: { $0.id == album.id }) {
            favoriteAlbums.remove(at: index)
        } else {
            favoriteAlbums.insert(AlbumRef(album: album), at: 0)
        }
        persistFavorites()
    }

    func isFavoriteTrack(_ url: String) -> Bool {
        favoriteTracks.contains { $0.url == url }
    }

    private func toggleFavoriteTrack(_ ref: TrackRef) {
        if let index = favoriteTracks.firstIndex(where: { $0.url == ref.url }) {
            favoriteTracks.remove(at: index)
        } else {
            favoriteTracks.insert(ref, at: 0)
        }
        persistFavorites()
    }

    func toggleFavoriteTrack(_ object: MediaObject) {
        guard let res = object.resURL else { return }
        toggleFavoriteTrack(TrackRef(
            url: res, title: object.title,
            album: object.album, albumArtURI: object.albumArtURI,
            metadata: DIDLBuilder.metadata(for: object)
        ))
    }

    func toggleFavoriteTrack(_ item: PlaylistItem) {
        // Recover album/cover from the track's DIDL metadata if available.
        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        toggleFavoriteTrack(TrackRef(
            url: item.url.absoluteString, title: item.title,
            album: parsed?.album, albumArtURI: parsed?.albumArtURI,
            metadata: item.metadata
        ))
    }

    func addTrack(_ ref: TrackRef) {
        guard let url = URL(string: ref.url) else { return }
        controller.addTrack(url: url, title: ref.title, metadata: ref.metadata)
        updateState()
    }

    func playTrack(_ object: MediaObject) {
        guard let res = object.resURL else { return }
        playTrack(TrackRef(
            url: res, title: object.title,
            album: object.album, albumArtURI: object.albumArtURI,
            metadata: DIDLBuilder.metadata(for: object)
        ))
    }

    /// Plays a track now: jumps to it if already queued, otherwise appends and plays.
    func playTrack(_ ref: TrackRef) {
        guard let url = URL(string: ref.url) else { return }
        if let index = controller.getPlaylistItems().firstIndex(where: { $0.url.absoluteString == ref.url }) {
            playItem(at: index)
        } else {
            controller.addTrack(url: url, title: ref.title, metadata: ref.metadata)
            playItem(at: controller.getPlaylistCount() - 1)
        }
    }

    func openFavoriteAlbum(_ ref: AlbumRef) -> MediaObject {
        MediaObject(id: ref.id, parentID: "", title: ref.title, isContainer: true, artist: ref.artist)
    }

    func unfavoriteTrack(_ ref: TrackRef) {
        favoriteTracks.removeAll { $0.url == ref.url }
        persistFavorites()
    }

    func unfavoriteAlbum(_ ref: AlbumRef) {
        favoriteAlbums.removeAll { $0.id == ref.id }
        persistFavorites()
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
                // Re-anchor the system panel's extrapolated elapsed time.
                updateNowPlaying()
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
    private var coverSaveTask: Task<Void, Never>?

    private var coverCacheFile: URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CamelPlayer", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("coverURLs.json")
    }

    private func loadCoverCache() {
        if let data = try? Data(contentsOf: coverCacheFile),
           let decoded = try? JSONDecoder().decode([String: String].self, from: data) {
            albumArtCache = decoded
        }
    }

    private func scheduleCoverCacheSave() {
        coverSaveTask?.cancel()
        let snapshot = albumArtCache
        let file = coverCacheFile
        coverSaveTask = Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            guard !Task.isCancelled else { return }
            if let data = try? JSONEncoder().encode(snapshot) {
                try? data.write(to: file)
            }
        }
    }

    /// The media server used for album browsing: the user-selected one if it is
    /// currently discovered, otherwise the first discovered server.
    var libraryServer: UPnPDevice? {
        mediaServers.first { $0.id == libraryServerID } ?? mediaServers.first
    }

    func setLibraryServer(_ id: String) {
        libraryServerID = id
        UserDefaults.standard.set(id, forKey: Keys.libraryServerID)
    }

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
        scheduleCoverCacheSave()
        return URL(string: uri)
    }

    /// Searches the library server (album wall). Caller splits albums vs tracks.
    func searchLibrary(query: String, requestedCount: Int = 100) async -> [MediaObject] {
        guard let server = libraryServer else { return [] }
        return (await search(server: server, query: query, requestedCount: requestedCount))?.objects ?? []
    }

    func albumTracks(albumID: String) async -> [MediaObject] {
        guard let server = libraryServer,
              let page = await browse(server: server, objectID: albumID) else { return [] }
        return page.objects
    }

    func playAlbum(_ album: MediaObject) {
        recordAlbumPlayed(album)
        guard let server = libraryServer else { return }
        Task {
            do {
                try await controller.playAlbum(server: server, objectID: album.id)
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

    func setShuffle(_ on: Bool) {
        controller.shuffle = on
        shuffle = on
        UserDefaults.standard.set(on, forKey: Keys.shuffle)
    }

    /// Cycles the loop control: off → all → one → off.
    func cycleLoop() {
        let next: LoopMode
        switch loopMode {
        case .off: next = .all
        case .all: next = .one
        case .one: next = .off
        }
        controller.loopMode = next
        loopMode = next
        UserDefaults.standard.set(Self.string(for: next), forKey: Keys.loopMode)
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
        return currentPosition < count - 1 || loopMode != .off || shuffle
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
