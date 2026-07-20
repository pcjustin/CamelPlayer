import CamelPlayerCore
import Foundation

/// Port of the macOS PlaybackViewModel for the GTK front end: same persisted
/// state and behavior, with plain vars polled by the GLib tick instead of
/// @Published properties.
final class PlayerModel {
    let controller: PlaybackController

    // State refreshed by updateState()
    private(set) var playbackState: PlaybackState = .stopped
    private(set) var currentItem: PlaylistItem?
    private(set) var playlistItems: [PlaylistItem] = []
    private(set) var currentPosition: Int = -1
    private(set) var currentTime: TimeInterval = 0
    private(set) var duration: TimeInterval?
    private(set) var formatInfo: String?
    /// nil when unknown (stopped or UPnP output).
    private(set) var isBitPerfect: Bool?
    private(set) var currentAlbum: String?
    private(set) var currentCoverURL: URL?
    private(set) var outputDevices: [OutputDevice] = []
    private(set) var currentOutputDevice: OutputDevice?
    private(set) var mediaServers: [UPnPDevice] = []

    var volume: Float { controller.volume }
    var shuffle: Bool { controller.shuffle }
    var loopMode: LoopMode { controller.loopMode }
    var isPlaying: Bool { playbackState == .playing }
    var isPaused: Bool { playbackState == .paused }
    var isStopped: Bool { playbackState == .stopped }

    private(set) var favoriteAlbums: [AlbumRef] = []
    private(set) var favoriteTracks: [TrackRef] = []
    private(set) var recentAlbums: [AlbumRef] = []
    private(set) var recentTracks: [TrackRef] = []

    /// Reported to the UI (alert dialog).
    var onError: ((String) -> Void)?

    private var libraryServerID: String?
    private var lastRecordedURL: String?
    private var lastCoverKey: String?
    private var hasRestoredOutputDevice = false
    private let recentLimit = 50

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

    init() throws {
        controller = try PlaybackController()
        let defaults = UserDefaults.standard
        controller.volume = (defaults.object(forKey: Keys.volume) as? Double).map(Float.init) ?? 1.0
        controller.shuffle = defaults.bool(forKey: Keys.shuffle)
        controller.loopMode = Self.loopMode(from: defaults.string(forKey: Keys.loopMode))
        libraryServerID = defaults.string(forKey: Keys.libraryServerID)
        restoreQueue()
        loadRefs()
        loadCoverCache()
        refreshMediaServers()
        refreshDevices()
        updateState()
    }

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

    // MARK: - Polling

    private var lastQueueIDs: [String] = []

    func updateState() {
        playbackState = controller.currentState
        currentItem = controller.currentItem
        currentTime = controller.currentTime
        duration = controller.duration
        formatInfo = controller.getFileFormat()
        let position = controller.getCurrentPosition()
        if position != currentPosition {
            currentPosition = position
            UserDefaults.standard.set(position, forKey: Keys.queuePosition)
        }
        let items = controller.getPlaylistItems()
        let ids = items.map { $0.url.absoluteString }
        if ids != lastQueueIDs {
            lastQueueIDs = ids
            playlistItems = items
            saveQueue()
            controller.refreshPreloadedNext()
        } else {
            playlistItems = items
        }
        updateBitPerfectStatus()
        updateCurrentCover()
        if let item = currentItem, item.url.absoluteString != lastRecordedURL {
            lastRecordedURL = item.url.absoluteString
            recordTrackPlayed(item)
        }
    }

    private func updateBitPerfectStatus() {
        var status: Bool?
        if case .local = controller.currentOutputDevice.type,
           playbackState != .stopped,
           let fileRate = controller.getFileSampleRate(),
           let deviceRate = try? controller.getCurrentDeviceSampleRate() {
            status = abs(fileRate - deviceRate) < 0.1
        }
        isBitPerfect = status
    }

    /// Album name and cover come from DIDL metadata for network tracks, or a
    /// cover.jpg next to the file for local ones. Embedded art is not read on
    /// Linux.
    private func updateCurrentCover() {
        guard let item = currentItem else {
            currentAlbum = nil
            currentCoverURL = nil
            lastCoverKey = nil
            return
        }
        let key = item.url.absoluteString
        guard key != lastCoverKey else { return }
        lastCoverKey = key

        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        currentAlbum = parsed?.album
        currentCoverURL = parsed?.albumArtURI.flatMap { URL(string: $0) }

        if currentCoverURL == nil, item.url.isFileURL {
            let folder = item.url.deletingLastPathComponent()
            for name in ["cover.jpg", "cover.jpeg", "Cover.jpg", "Cover.jpeg"] {
                let cover = folder.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: cover.path) {
                    currentCoverURL = cover
                    break
                }
            }
        }
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
        lastRecordedURL = controller.currentItem?.url.absoluteString
    }

    // MARK: - Favorites / recently played

    private func loadRefs() {
        let defaults = UserDefaults.standard
        func decode<T: Codable>(_ key: String) -> [T] {
            guard let data = defaults.data(forKey: key),
                  let decoded = try? JSONDecoder().decode([T].self, from: data) else { return [] }
            return decoded
        }
        favoriteAlbums = decode(Keys.favoriteAlbums)
        favoriteTracks = decode(Keys.favoriteTracks)
        recentAlbums = decode(Keys.recentAlbums)
        recentTracks = decode(Keys.recentTracks)
    }

    private func persist<T: Codable>(_ value: [T], key: String) {
        if let data = try? JSONEncoder().encode(value) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    private func recordTrackPlayed(_ item: PlaylistItem) {
        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        let ref = TrackRef(url: item.url.absoluteString, title: item.title,
                           album: parsed?.album, albumArtURI: parsed?.albumArtURI,
                           metadata: item.metadata)
        recentTracks.removeAll { $0.url == ref.url }
        recentTracks.insert(ref, at: 0)
        if recentTracks.count > recentLimit { recentTracks = Array(recentTracks.prefix(recentLimit)) }
        persist(recentTracks, key: Keys.recentTracks)
    }

    func recordAlbumPlayed(_ album: MediaObject) {
        let ref = AlbumRef(album: album)
        recentAlbums.removeAll { $0.id == ref.id }
        recentAlbums.insert(ref, at: 0)
        if recentAlbums.count > recentLimit { recentAlbums = Array(recentAlbums.prefix(recentLimit)) }
        persist(recentAlbums, key: Keys.recentAlbums)
    }

    func clearRecentlyPlayed() {
        recentTracks = []
        recentAlbums = []
        persist(recentTracks, key: Keys.recentTracks)
        persist(recentAlbums, key: Keys.recentAlbums)
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
        persist(favoriteAlbums, key: Keys.favoriteAlbums)
    }

    func unfavoriteAlbum(_ ref: AlbumRef) {
        favoriteAlbums.removeAll { $0.id == ref.id }
        persist(favoriteAlbums, key: Keys.favoriteAlbums)
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
        persist(favoriteTracks, key: Keys.favoriteTracks)
    }

    func toggleFavoriteTrack(_ object: MediaObject) {
        guard let res = object.resURL else { return }
        toggleFavoriteTrack(TrackRef(url: res, title: object.title,
                                     album: object.album, albumArtURI: object.albumArtURI,
                                     metadata: DIDLBuilder.metadata(for: object)))
    }

    func toggleFavoriteTrack(_ item: PlaylistItem) {
        let parsed = item.metadata.flatMap { DIDLParser().parse($0).first }
        toggleFavoriteTrack(TrackRef(url: item.url.absoluteString, title: item.title,
                                     album: parsed?.album, albumArtURI: parsed?.albumArtURI,
                                     metadata: item.metadata))
    }

    func unfavoriteTrack(_ ref: TrackRef) {
        favoriteTracks.removeAll { $0.url == ref.url }
        persist(favoriteTracks, key: Keys.favoriteTracks)
    }

    func openAlbumRef(_ ref: AlbumRef) -> MediaObject {
        MediaObject(id: ref.id, parentID: "", title: ref.title, isContainer: true, artist: ref.artist)
    }

    // MARK: - Playback control

    private func run(_ label: String, _ body: @escaping () async throws -> Void) {
        Task {
            do {
                try await body()
                DispatchQueue.main.async { self.updateState() }
            } catch let error as AudioPlayerError {
                self.report(Self.describe(error))
            } catch {
                self.report("\(label): \(error.localizedDescription)")
            }
        }
    }

    func play() { run("Play") { try await self.controller.play() } }
    func resume() { run("Resume") { try await self.controller.resume() } }
    func next() { run("Next") { try await self.controller.next() } }
    func previous() { run("Previous") { try await self.controller.previous() } }
    func playItem(at index: Int) { run("Play") { try await self.controller.playItem(at: index) } }
    func seek(to time: TimeInterval) { run("Seek") { try await self.controller.seek(to: time) } }

    func pause() {
        controller.pause()
        updateState()
    }

    func stop() {
        controller.stop()
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

    var canGoNext: Bool {
        currentPosition < controller.getPlaylistCount() - 1 || loopMode != .off || shuffle
    }

    var canGoPrevious: Bool {
        currentPosition > 0 || shuffle
    }

    /// True when the current track lives on a network server but the selected
    /// output is local.
    var currentTrackNeedsRenderer: Bool {
        guard let item = currentItem, !item.url.isFileURL else { return false }
        if case .local = currentOutputDevice?.type { return true }
        return false
    }

    // MARK: - Playlist

    func addFiles(_ urls: [URL]) {
        controller.addToPlaylist(urls: urls)
        updateState()
    }

    func addTrack(_ ref: TrackRef) {
        guard let url = URL(string: ref.url) else { return }
        controller.addTrack(url: url, title: ref.title, metadata: ref.metadata)
        updateState()
    }

    func addTrack(_ object: MediaObject) {
        if controller.addTrackToPlaylist(object) {
            updateState()
        } else {
            report("This item has no playable source")
        }
    }

    func playTrack(_ object: MediaObject) {
        guard let res = object.resURL else { return }
        playTrack(TrackRef(url: res, title: object.title,
                           album: object.album, albumArtURI: object.albumArtURI,
                           metadata: DIDLBuilder.metadata(for: object)))
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

    func removeFromPlaylist(at index: Int) {
        controller.removeFromPlaylist(at: index)
        updateState()
    }

    func movePlaylistItem(from index: Int, to target: Int) {
        controller.movePlaylistItem(fromOffsets: IndexSet(integer: index), toOffset: target)
        updateState()
    }

    func clearPlaylist() {
        controller.clearPlaylist()
        updateState()
    }

    func exportPlaylist(to url: URL) {
        do {
            try controller.exportPlaylist(to: url)
        } catch {
            report("Failed to save playlist: \(error.localizedDescription)")
        }
    }

    func importPlaylist(from url: URL) {
        do {
            let count = try controller.importPlaylist(from: url)
            updateState()
            if count == 0 { report("No tracks in playlist file") }
        } catch {
            report("Failed to load playlist: \(error.localizedDescription)")
        }
    }

    // MARK: - Devices

    func refreshDevices() {
        outputDevices = controller.listAllOutputDevices()
        currentOutputDevice = controller.currentOutputDevice
        restoreOutputDeviceIfNeeded()
    }

    func setOutputDevice(_ device: OutputDevice) {
        do {
            try controller.setOutputDevice(device)
            currentOutputDevice = device
            UserDefaults.standard.set(device.id, forKey: Keys.outputDeviceID)
        } catch {
            report("Failed to set output device: \(error.localizedDescription)")
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

    func setVolume(_ volume: Float) {
        controller.volume = volume
        UserDefaults.standard.set(Double(volume), forKey: Keys.volume)
    }

    func setShuffle(_ on: Bool) {
        controller.shuffle = on
        controller.refreshPreloadedNext()
        UserDefaults.standard.set(on, forKey: Keys.shuffle)
    }

    /// Cycles the loop control: off -> all -> one -> off.
    func cycleLoop() {
        let next: LoopMode
        switch loopMode {
        case .off: next = .all
        case .all: next = .one
        case .one: next = .off
        }
        controller.loopMode = next
        controller.refreshPreloadedNext()
        UserDefaults.standard.set(Self.string(for: next), forKey: Keys.loopMode)
    }

    // MARK: - Library (album wall)

    private var albumArtCache: [String: String] = [:]
    private var coverSavePending = false

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
        guard !coverSavePending else { return }
        coverSavePending = true
        let file = coverCacheFile
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self = self else { return }
            self.coverSavePending = false
            if let data = try? JSONEncoder().encode(self.albumArtCache) {
                try? data.write(to: file)
            }
        }
    }

    func refreshMediaServers() {
        mediaServers = controller.availableMediaServers
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
            report("Failed to load albums: \(error.localizedDescription)")
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

    func searchLibrary(query: String, requestedCount: Int = 100) async -> [MediaObject] {
        guard let server = libraryServer else { return [] }
        do {
            return try await controller.search(server: server, query: query,
                                               requestedCount: requestedCount).objects
        } catch {
            report("Search failed: \(error.localizedDescription)")
            return []
        }
    }

    func albumTracks(albumID: String) async -> [MediaObject] {
        guard let server = libraryServer else { return [] }
        do {
            return try await controller.browse(server: server, objectID: albumID).objects
        } catch {
            report("Browse failed: \(error.localizedDescription)")
            return []
        }
    }

    func playAlbum(_ album: MediaObject) {
        recordAlbumPlayed(album)
        guard let server = libraryServer else { return }
        run("Play album") { try await self.controller.playAlbum(server: server, objectID: album.id) }
    }

    func addContainerToPlaylist(server: UPnPDevice, objectID: String, sortCriteria: String = "") async -> Int {
        do {
            let count = try await controller.addContainerToPlaylist(
                server: server, objectID: objectID, sortCriteria: sortCriteria)
            DispatchQueue.main.async { self.updateState() }
            if count == 0 { report("No playable tracks in this folder") }
            return count
        } catch {
            report("Failed to add folder: \(error.localizedDescription)")
            return 0
        }
    }

    // MARK: - Errors

    private static func describe(_ error: AudioPlayerError) -> String {
        switch error {
        case .fileNotFound:
            return "File not found"
        case .unsupportedFormat:
            return "Unsupported audio format"
        case .audioEngineError(let message):
            return "Audio engine error: \(message)"
        case .fileLoadError(let message):
            return "Failed to load file: \(message)"
        case .remoteURLRequiresRenderer:
            return "This track is on a network server. Choose a network renderer as the output device to play it."
        }
    }

    private func report(_ message: String) {
        DispatchQueue.main.async { self.onError?(message) }
    }
}
