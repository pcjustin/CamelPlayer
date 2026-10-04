import Foundation
#if os(macOS)
import AVFoundation
import CoreAudio
#endif

// MARK: - Output Device Types

public enum OutputDeviceType {
    case local(AudioDeviceID)
    case upnp(UPnPDevice)
}

public struct OutputDevice: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let type: OutputDeviceType

    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    public static func == (lhs: OutputDevice, rhs: OutputDevice) -> Bool {
        return lhs.id == rhs.id
    }
}

// MARK: - Playback Controller

public class PlaybackController {
    private let player: AudioPlayer?
    private let playlist: Playlist
    private var playbackGeneration = 0
    private var queueGeneration = 0
    private var albumContainerIDCache: [String: String] = [:]

    // UPnP support
    private let upnpManager: UPnPDeviceManager
    private let mediaServer: LocalMediaServer
    private var currentEngine: PlaybackEngine
    private var localEngine: PlaybackEngine
    private(set) public var currentOutputDevice: OutputDevice

    /// Called when the set of available UPnP renderers changes (discovery is async).
    public var onUPnPDevicesChanged: (() -> Void)?

    /// Called when the set of available UPnP media servers changes.
    public var onUPnPServersChanged: (() -> Void)?

    public var currentState: PlaybackState {
        currentEngine.state
    }

    public var currentItem: PlaylistItem? {
        playlist.currentItem
    }

    public var shuffle: Bool {
        get { playlist.shuffle }
        set { playlist.shuffle = newValue; refreshPreloadedNext() }
    }

    public var loopMode: LoopMode {
        get { playlist.loopMode }
        set { playlist.loopMode = newValue; refreshPreloadedNext() }
    }

    public var volume: Float {
        get { currentEngine.volume }
        set { currentEngine.volume = newValue.isFinite ? max(0, min(1, newValue)) : 0 }
    }

    public var currentTime: TimeInterval {
        currentEngine.currentTime
    }

    public var duration: TimeInterval? {
        currentEngine.duration
    }

    public init() throws {
        let audioPlayer = try AudioPlayer()
        player = audioPlayer
        playlist = Playlist()

        // Initialize UPnP components
        upnpManager = UPnPDeviceManager()
        mediaServer = LocalMediaServer()

        // Set up playback engines
        localEngine = LocalPlaybackEngine(audioPlayer: audioPlayer)
        currentEngine = localEngine

        // Set default output device (local default device)
        let defaultDeviceID = try audioPlayer.getDefaultOutputDevice()
        currentOutputDevice = OutputDevice(
            id: "local-\(defaultDeviceID)",
            name: "Default Output",
            type: .local(defaultDeviceID)
        )

        // Notify when discovery changes the device lists so the UI can refresh live
        upnpManager.onRenderersChanged = { [weak self] in self?.onUPnPDevicesChanged?() }
        upnpManager.onServersChanged = { [weak self] in self?.onUPnPServersChanged?() }

        // Start HTTP server and UPnP discovery
        try? mediaServer.start()
        upnpManager.startDiscovery()

        configureCallbacks(for: currentEngine)
    }

    init(engine: PlaybackEngine) {
        player = nil
        playlist = Playlist()
        upnpManager = UPnPDeviceManager()
        mediaServer = LocalMediaServer()
        localEngine = engine
        currentEngine = engine
        currentOutputDevice = OutputDevice(id: "test", name: "Test", type: .local(0))
        configureCallbacks(for: engine)
    }

    private func configureCallbacks(for engine: PlaybackEngine) {
        engine.onPlaybackFinished = { [weak self, weak engine] in
            guard let self = self else { return }
            let generation = self.playbackGeneration
            Task { @MainActor [weak self, weak engine] in
                guard let self = self, let engine = engine,
                      self.currentEngine === engine, generation == self.playbackGeneration else { return }
                self.playNextIfAvailable()
            }
        }
        engine.onAdvancedToNext = { [weak self, weak engine] in
            guard let self = self, let engine = engine, self.currentEngine === engine else { return }
            self.handleAdvancedToNext()
        }
    }

    @MainActor
    private func playNextIfAvailable() {
        guard let nextItem = playlist.next() else {
            // No next item available
            return
        }

        let generation = playbackGeneration
        Task {
            guard generation == playbackGeneration else { return }
            do {
                try await startPlaying(nextItem)
            } catch {
                // Silently fail - could log error in future
                coreLog("Error auto-playing next track: \(error.localizedDescription)")
            }
        }
    }

    /// Loads/plays an item and preloads the next one for gapless playback.
    @MainActor
    private func startPlaying(_ item: PlaylistItem) async throws {
        playbackGeneration += 1
        let generation = playbackGeneration
        let engine = currentEngine
        do {
            try await engine.loadAndPlay(url: item.url, metadata: item.metadata)
            guard generation == playbackGeneration, currentEngine === engine else { return }
            setNextOnEngine()
        } catch {
            guard generation == playbackGeneration else { return }
            throw error
        }
    }

    private func setNextOnEngine() {
        let next = playlist.peekNext()
        currentEngine.setNextTrack(url: next?.url, metadata: next?.metadata)
    }

    /// Re-syncs the preloaded next track after anything that changes what
    /// peekNext returns (shuffle/loop toggles, playlist edits).
    public func refreshPreloadedNext() {
        guard currentEngine.state != .stopped else { return }
        setNextOnEngine()
    }

    /// The renderer gaplessly advanced to the preloaded next track: move our
    /// pointer to match and preload the following one.
    private func handleAdvancedToNext() {
        _ = playlist.next()
        setNextOnEngine()
    }

    public func addToPlaylist(url: URL) {
        playlist.add(url: url)
        refreshPreloadedNext()
    }

    public func addToPlaylist(urls: [URL]) {
        playlist.addAll(urls: urls)
        refreshPreloadedNext()
    }

    public func addTrack(url: URL, title: String, metadata: String?) {
        playlist.add(PlaylistItem(url: url, title: title, metadata: metadata))
        refreshPreloadedNext()
    }

    @MainActor
    public func play() async throws {
        if currentEngine.state == .playing {
            return
        }

        if currentEngine.state == .paused {
            try await currentEngine.play()
            setNextOnEngine()
            return
        }

        // Stopped: load the current item before playing.
        guard let item = playlist.currentItem else {
            throw AudioPlayerError.fileLoadError("No items in playlist")
        }

        try await startPlaying(item)
    }

    @MainActor
    public func playItem(at index: Int) async throws {
        guard let item = playlist.jumpTo(index: index) else {
            throw AudioPlayerError.fileLoadError("Invalid playlist index")
        }

        try await startPlaying(item)
    }

    public func pause() {
        currentEngine.pause()
    }

    @MainActor
    public func resume() async throws {
        try await play()
    }

    public func stop() {
        playbackGeneration += 1
        currentEngine.stop()
    }

    /// Does nothing past the end of the queue: media keys and MPRIS call this
    /// without checking whether a next track exists.
    @MainActor
    public func next() async throws {
        guard let item = playlist.next() else { return }
        try await startPlaying(item)
    }

    @MainActor
    public func previous() async throws {
        guard let item = playlist.previous() else { return }
        try await startPlaying(item)
    }

    @MainActor
    public func seek(to time: TimeInterval) async throws {
        try await currentEngine.seek(to: time)
    }

    // MARK: - Output Device Management

    /// Lists all available output devices (local + UPnP)
    public func listAllOutputDevices() -> [OutputDevice] {
        var devices: [OutputDevice] = []

        // Add local audio devices
        if let localDevices = try? player?.listOutputDevices() {
            for device in localDevices {
                devices.append(OutputDevice(
                    id: "local-\(device.id)",
                    name: device.name,
                    type: .local(device.id)
                ))
            }
        }

        // Add UPnP renderers
        for upnpDevice in upnpManager.availableRenderers {
            devices.append(OutputDevice(
                id: "upnp-\(upnpDevice.id)",
                name: upnpDevice.friendlyName,
                type: .upnp(upnpDevice)
            ))
        }

        return devices
    }

    /// Sets the output device (local or UPnP)
    public func setOutputDevice(_ device: OutputDevice) throws {
        guard device.id != currentOutputDevice.id else { return }
        // Stop current playback and carry the current volume over to the new engine.
        let wasPlaying = currentEngine.state == .playing
        let currentVolume = currentEngine.volume
        stop()

        switch device.type {
        case .local(let deviceID):
            // Switch to local playback
            guard let player = player else { throw OutputDeviceError.deviceNotFound }
            try player.setOutputDevice(deviceID: deviceID)
            currentEngine = localEngine
            currentOutputDevice = device

        case .upnp(let upnpDevice):
            // Switch to UPnP playback
            let upnpEngine = UPnPPlaybackEngine(device: upnpDevice, mediaServer: mediaServer)
            configureCallbacks(for: upnpEngine)
            currentEngine = upnpEngine
            currentOutputDevice = device
        }

        currentEngine.volume = currentVolume

        // Resume playback only if something was actually playing
        if wasPlaying, let currentItem = playlist.currentItem {
            let generation = playbackGeneration
            Task { @MainActor in
                guard generation == playbackGeneration else { return }
                do {
                    try await startPlaying(currentItem)
                } catch {
                    coreLog("Error resuming playback on new device: \(error)")
                }
            }
        }
    }

    /// Refreshes the UPnP device list
    public func refreshUPnPDevices() {
        upnpManager.refresh()
    }

    // MARK: - Media Server Browsing

    /// UPnP media servers (sources) discovered on the network.
    public var availableMediaServers: [UPnPDevice] {
        upnpManager.availableServers
    }

    /// One page of a Browse result.
    public struct BrowsePage {
        public let objects: [MediaObject]
        public let totalMatches: Int
    }

    private static let browsePageSize = 200

    /// Browses one page of a container. Root container ID is "0".
    @MainActor
    public func browse(
        server: UPnPDevice,
        objectID: String = "0",
        startingIndex: Int = 0,
        requestedCount: Int = 200,
        sortCriteria: String = ""
    ) async throws -> BrowsePage {
        guard let controlURL = server.contentDirectoryURL else {
            throw MediaBrowseError.serverHasNoContentDirectory
        }
        let service = ContentDirectoryService(controlURL: controlURL)
        let result = try await service.browse(
            objectID: objectID,
            startingIndex: startingIndex,
            requestedCount: requestedCount,
            sortCriteria: sortCriteria
        )
        return BrowsePage(objects: result.objects.map { $0.sourced(from: server.id) }, totalMatches: result.totalMatches)
    }

    /// Searches a server's whole library for audio tracks matching a free-text
    /// query (title, artist or album contains the text).
    @MainActor
    public func search(
        server: UPnPDevice,
        query: String,
        startingIndex: Int = 0,
        requestedCount: Int = 200
    ) async throws -> BrowsePage {
        guard let controlURL = server.contentDirectoryURL else {
            throw MediaBrowseError.serverHasNoContentDirectory
        }
        let service = ContentDirectoryService(controlURL: controlURL)
        let result = try await service.search(
            searchCriteria: ContentDirectoryService.textSearchCriteria(query),
            startingIndex: startingIndex,
            requestedCount: requestedCount
        )
        return BrowsePage(objects: result.objects.map { $0.sourced(from: server.id) }, totalMatches: result.totalMatches)
    }

    /// Adds a single track object to the playlist. Returns false if it is not
    /// a playable item.
    @discardableResult
    public func addTrackToPlaylist(_ object: MediaObject) -> Bool {
        guard !object.isContainer, let res = object.resURL, let url = URL(string: res),
              ["http", "https"].contains(url.scheme?.lowercased() ?? ""), url.host != nil else {
            return false
        }
        playlist.add(PlaylistItem(url: url, title: object.title, metadata: DIDLBuilder.metadata(for: object)))
        refreshPreloadedNext()
        return true
    }

    // MARK: - Album-centric browsing

    /// Finds the server's all-albums container id (MinimServer: "0$albums").
    @MainActor
    private func albumContainerID(server: UPnPDevice) async -> String? {
        if let cached = albumContainerIDCache[server.id] { return cached }
        guard let page = try? await browse(server: server, objectID: "0", requestedCount: 100) else {
            return nil
        }
        // ponytail: heuristic for MinimServer/DLNA; a richer match can come with
        // the route-B local index.
        let match = page.objects.first {
            $0.isContainer && ($0.id.hasSuffix("$albums") || $0.title.lowercased().hasSuffix("albums"))
        }
        if let id = match?.id { albumContainerIDCache[server.id] = id }
        return match?.id
    }

    /// Lists albums (paged) from the server's album index.
    @MainActor
    public func albums(server: UPnPDevice, startingIndex: Int = 0, requestedCount: Int = 100) async throws -> BrowsePage {
        guard let containerID = await albumContainerID(server: server) else {
            return BrowsePage(objects: [], totalMatches: 0)
        }
        return try await browse(server: server, objectID: containerID, startingIndex: startingIndex, requestedCount: requestedCount)
    }

    /// Fetches an album's cover URL via BrowseMetadata (album lists omit it).
    @MainActor
    public func albumArtURI(server: UPnPDevice, objectID: String) async -> String? {
        guard let controlURL = server.contentDirectoryURL else { return nil }
        let service = ContentDirectoryService(controlURL: controlURL)
        let result = try? await service.browse(objectID: objectID, flag: .metadata, requestedCount: 1)
        return result?.objects.first?.albumArtURI
    }

    /// Replaces the playlist with an album's tracks and starts playback.
    @MainActor
    public func playAlbum(server: UPnPDevice, objectID: String) async throws {
        playbackGeneration += 1
        let generation = playbackGeneration
        let tracks = try await containerTracks(server: server, objectID: objectID)
        guard generation == playbackGeneration else { return }
        guard !tracks.isEmpty else { throw AudioPlayerError.fileLoadError("No playable tracks in this album") }
        clearPlaylist()
        for track in tracks { addTrackToPlaylist(track) }
        guard let item = playlist.jumpTo(index: 0) else { return }
        try await startPlaying(item)
    }

    /// Sort fields the server supports, or an empty array if none/unavailable.
    @MainActor
    public func sortCapabilities(server: UPnPDevice) async -> [String] {
        guard let controlURL = server.contentDirectoryURL else { return [] }
        let service = ContentDirectoryService(controlURL: controlURL)
        return (try? await service.getSortCapabilities()) ?? []
    }

    private static let maxAddDepth = 8

    /// Adds every playable track in a container subtree to the playlist, paging
    /// through each container and recursing into sub-containers. Returns the
    /// number of tracks added.
    @discardableResult
    @MainActor
    public func addContainerToPlaylist(
        server: UPnPDevice,
        objectID: String,
        sortCriteria: String = ""
    ) async throws -> Int {
        let generation = queueGeneration
        let tracks = try await containerTracks(server: server, objectID: objectID, sortCriteria: sortCriteria)
        guard generation == queueGeneration else { return 0 }
        for track in tracks { addTrackToPlaylist(track) }
        return tracks.count
    }

    @MainActor
    private func containerTracks(server: UPnPDevice, objectID: String, sortCriteria: String = "") async throws -> [MediaObject] {
        guard let controlURL = server.contentDirectoryURL else {
            throw MediaBrowseError.serverHasNoContentDirectory
        }
        let service = ContentDirectoryService(controlURL: controlURL)
        var tracks: [MediaObject] = []
        _ = try await Self.addContainer(
            objectID: objectID,
            browse: { id, index, count in
                try await service.browse(objectID: id, startingIndex: index,
                                         requestedCount: count, sortCriteria: sortCriteria)
            },
            add: { object in
                guard let resource = object.resURL, let url = URL(string: resource),
                      ["http", "https"].contains(url.scheme?.lowercased() ?? ""), url.host != nil else { return false }
                tracks.append(object)
                return true
            }
        )
        return tracks.map { $0.sourced(from: server.id) }
    }

    @MainActor
    public func albumTracks(server: UPnPDevice, objectID: String) async throws -> [MediaObject] {
        try await containerTracks(server: server, objectID: objectID)
    }

    // Keep traversal independent of audio hardware and network discovery.
    static func addContainer(
        objectID: String,
        depth: Int = 0,
        ancestors: Set<String> = [],
        browse: (String, Int, Int) async throws -> ContentDirectoryService.BrowseResult,
        add: (MediaObject) -> Bool
    ) async throws -> Int {
        try Task.checkCancellation()
        guard depth <= maxAddDepth, !ancestors.contains(objectID) else { return 0 }
        let ancestors = ancestors.union([objectID])
        var added = 0
        var index = 0
        while true {
            try Task.checkCancellation()
            let page = try await browse(objectID, index, browsePageSize)
            try Task.checkCancellation()
            guard page.numberReturned > 0, !page.objects.isEmpty else { break }
            for object in page.objects {
                if object.isContainer {
                    added += try await addContainer(objectID: object.id, depth: depth + 1,
                                                    ancestors: ancestors, browse: browse, add: add)
                } else if add(object) {
                    added += 1
                }
            }
            let (nextIndex, overflow) = index.addingReportingOverflow(page.numberReturned)
            guard !overflow else { break }
            index = nextIndex
            if index >= page.totalMatches { break }
        }
        return added
    }

    public func getPlaylistItems() -> [PlaylistItem] {
        playlist.allItems()
    }

    public func getPlaylistCount() -> Int {
        playlist.count
    }

    public func getCurrentPosition() -> Int {
        playlist.currentPosition
    }

    /// Moves the current-track pointer without starting playback (used when
    /// restoring a saved queue).
    public func setPlaylistPosition(_ index: Int) {
        _ = playlist.jumpTo(index: index)
    }

    public func clearPlaylist() {
        stop()
        queueGeneration += 1
        playlist.clear()
    }

    public func removeFromPlaylist(at index: Int) {
        if index == playlist.currentPosition { stop() }
        playlist.remove(at: index)
        refreshPreloadedNext()
    }

    public func movePlaylistItem(fromOffsets: IndexSet, toOffset: Int) {
        playlist.move(fromOffsets: fromOffsets, toOffset: toOffset)
        refreshPreloadedNext()
    }

    private struct PlaylistEntry: Codable {
        let url: String
        let title: String
        let metadata: String?
    }

    /// Writes the current playlist to a JSON file.
    public func exportPlaylist(to fileURL: URL) throws {
        let entries = playlist.allItems().map {
            PlaylistEntry(url: $0.url.absoluteString, title: $0.title, metadata: $0.metadata)
        }
        let data = try JSONEncoder().encode(entries)
        try data.write(to: fileURL, options: .atomic)
    }

    /// Appends tracks from a JSON playlist file. Returns the number added.
    @discardableResult
    public func importPlaylist(from fileURL: URL) throws -> Int {
        let data = try Data(contentsOf: fileURL)
        let entries = try JSONDecoder().decode([PlaylistEntry].self, from: data)
        var added = 0
        for entry in entries {
            guard let url = URL(string: entry.url) else { continue }
            playlist.add(PlaylistItem(url: url, title: entry.title, metadata: entry.metadata))
            added += 1
        }
        refreshPreloadedNext()
        return added
    }

    public func getCurrentDeviceSampleRate() throws -> Float64 {
        guard let player = player else { throw OutputDeviceError.deviceNotFound }
        return try player.getCurrentDeviceSampleRate()
    }

    public func getFileSampleRate() -> Float64? {
        player?.getFileSampleRate()
    }

    public func getFileFormat() -> String? {
        currentEngine.getFileFormat()
    }

    deinit {
        currentEngine.stop()
        upnpManager.stopDiscovery()
        mediaServer.stop()
    }
}
