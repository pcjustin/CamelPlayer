import AVFoundation
import CoreAudio
import Foundation

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
    private let player: AudioPlayer
    private let playlist: Playlist
    private var lastPlayStartTime: Date?
    private var albumContainerIDCache: [String: String] = [:]
    private let minimumPlayDuration: TimeInterval = 0.5 // minimum play time threshold

    // UPnP support
    private let upnpManager: UPnPDeviceManager
    private let mediaServer: LocalMediaServer
    private var currentEngine: PlaybackEngine
    private var localEngine: LocalPlaybackEngine
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

    public var playbackMode: PlaybackMode {
        get { playlist.mode }
        set { playlist.mode = newValue }
    }

    public var volume: Float {
        get { currentEngine.volume }
        set { currentEngine.volume = newValue }
    }

    public var currentTime: TimeInterval {
        currentEngine.currentTime
    }

    public var duration: TimeInterval? {
        currentEngine.duration
    }

    public init() throws {
        player = try AudioPlayer()
        playlist = Playlist()

        // Initialize UPnP components
        upnpManager = UPnPDeviceManager()
        mediaServer = LocalMediaServer()

        // Set up playback engines
        localEngine = LocalPlaybackEngine(audioPlayer: player)
        currentEngine = localEngine

        // Set default output device (local default device)
        let defaultDeviceID = try player.getDefaultOutputDevice()
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

        // Set up auto-play next track when current track finishes
        currentEngine.onPlaybackFinished = { [weak self] in
            self?.playNextIfAvailable()
        }
    }

    private func playNextIfAvailable() {
        // If the previous track played too briefly it likely failed to load;
        // stop auto-advancing to avoid rapidly skipping through the playlist.
        if let startTime = lastPlayStartTime {
            let playDuration = Date().timeIntervalSince(startTime)
            if playDuration < minimumPlayDuration {
                coreLog("Warning: Track played for only \(playDuration)s, stopping auto-play to prevent rapid skipping")
                return
            }
        }

        guard let nextItem = playlist.next() else {
            // No next item available
            return
        }

        Task {
            do {
                lastPlayStartTime = Date()
                try await currentEngine.loadAndPlay(url: nextItem.url, metadata: nextItem.metadata)
            } catch {
                // Silently fail - could log error in future
                coreLog("Error auto-playing next track: \(error.localizedDescription)")
            }
        }
    }

    public func addToPlaylist(url: URL) {
        playlist.add(url: url)
    }

    public func addToPlaylist(urls: [URL]) {
        playlist.addAll(urls: urls)
    }

    public func addTrack(url: URL, title: String, metadata: String?) {
        playlist.add(PlaylistItem(url: url, title: title, metadata: metadata))
    }

    public func play() async throws {
        if currentEngine.state == .playing {
            return
        }

        if currentEngine.state == .paused {
            lastPlayStartTime = Date()
            try await currentEngine.play()
            return
        }

        // Stopped: load the current item before playing.
        guard let item = playlist.currentItem else {
            throw AudioPlayerError.fileLoadError("No items in playlist")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url, metadata: item.metadata)
    }

    public func playItem(at index: Int) async throws {
        guard let item = playlist.jumpTo(index: index) else {
            throw AudioPlayerError.fileLoadError("Invalid playlist index")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url, metadata: item.metadata)
    }

    public func pause() {
        currentEngine.pause()
    }

    public func resume() async throws {
        try await currentEngine.play()
    }

    public func stop() {
        currentEngine.stop()
    }

    public func next() async throws {
        guard let item = playlist.next() else {
            throw AudioPlayerError.fileLoadError("No next item")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url, metadata: item.metadata)
    }

    public func previous() async throws {
        guard let item = playlist.previous() else {
            throw AudioPlayerError.fileLoadError("No previous item")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url, metadata: item.metadata)
    }

    public func seek(to time: TimeInterval) async throws {
        try await currentEngine.seek(to: time)
    }

    // MARK: - Output Device Management

    /// Lists all available output devices (local + UPnP)
    public func listAllOutputDevices() -> [OutputDevice] {
        var devices: [OutputDevice] = []

        // Add local audio devices
        if let localDevices = try? player.listOutputDevices() {
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
        // Stop current playback and carry the current volume over to the new engine.
        let currentVolume = currentEngine.volume
        currentEngine.stop()

        switch device.type {
        case .local(let deviceID):
            // Switch to local playback
            try player.setOutputDevice(deviceID: deviceID)
            currentEngine = localEngine
            currentOutputDevice = device

        case .upnp(let upnpDevice):
            // Switch to UPnP playback
            let upnpEngine = UPnPPlaybackEngine(device: upnpDevice, mediaServer: mediaServer)
            upnpEngine.onPlaybackFinished = { [weak self] in
                self?.playNextIfAvailable()
            }
            currentEngine = upnpEngine
            currentOutputDevice = device
        }

        currentEngine.volume = currentVolume

        // Resume playback if there was something playing
        if let currentItem = playlist.currentItem {
            Task {
                do {
                    try await currentEngine.loadAndPlay(url: currentItem.url, metadata: currentItem.metadata)
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
        return BrowsePage(objects: result.objects, totalMatches: result.totalMatches)
    }

    /// Searches a server's whole library for audio tracks matching a free-text
    /// query (title, artist or album contains the text).
    public func search(
        server: UPnPDevice,
        query: String,
        startingIndex: Int = 0,
        requestedCount: Int = 200
    ) async throws -> BrowsePage {
        guard let controlURL = server.contentDirectoryURL else {
            throw MediaBrowseError.serverHasNoContentDirectory
        }
        // Strip quotes so they can't break the criteria expression; SOAPClient
        // XML-escapes the rest. No class filter, so both tracks and albums
        // match (a class OR-expression is rejected by some servers).
        let safe = query.replacingOccurrences(of: "\"", with: "")
        let criteria = "dc:title contains \"\(safe)\""
            + " or upnp:artist contains \"\(safe)\""
            + " or upnp:album contains \"\(safe)\""
        let service = ContentDirectoryService(controlURL: controlURL)
        let result = try await service.search(
            searchCriteria: criteria,
            startingIndex: startingIndex,
            requestedCount: requestedCount
        )
        return BrowsePage(objects: result.objects, totalMatches: result.totalMatches)
    }

    /// Adds a single track object to the playlist. Returns false if it is not
    /// a playable item.
    @discardableResult
    public func addTrackToPlaylist(_ object: MediaObject) -> Bool {
        guard !object.isContainer, let res = object.resURL, let url = URL(string: res) else {
            return false
        }
        playlist.add(PlaylistItem(url: url, title: object.title, metadata: DIDLBuilder.metadata(for: object)))
        return true
    }

    // MARK: - Album-centric browsing

    /// Finds the server's all-albums container id (MinimServer: "0$albums").
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
    public func albums(server: UPnPDevice, startingIndex: Int = 0, requestedCount: Int = 100) async throws -> BrowsePage {
        guard let containerID = await albumContainerID(server: server) else {
            return BrowsePage(objects: [], totalMatches: 0)
        }
        return try await browse(server: server, objectID: containerID, startingIndex: startingIndex, requestedCount: requestedCount)
    }

    /// Fetches an album's cover URL via BrowseMetadata (album lists omit it).
    public func albumArtURI(server: UPnPDevice, objectID: String) async -> String? {
        guard let controlURL = server.contentDirectoryURL else { return nil }
        let service = ContentDirectoryService(controlURL: controlURL)
        let result = try? await service.browse(objectID: objectID, flag: .metadata, requestedCount: 1)
        return result?.objects.first?.albumArtURI
    }

    /// Replaces the playlist with an album's tracks and starts playback.
    public func playAlbum(server: UPnPDevice, objectID: String) async throws {
        playlist.clear()
        _ = try await addContainerToPlaylist(server: server, objectID: objectID)
        guard let item = playlist.jumpTo(index: 0) else { return }
        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url, metadata: item.metadata)
    }

    /// Sort fields the server supports, or an empty array if none/unavailable.
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
    public func addContainerToPlaylist(
        server: UPnPDevice,
        objectID: String,
        sortCriteria: String = ""
    ) async throws -> Int {
        try await addContainer(server: server, objectID: objectID, sortCriteria: sortCriteria, depth: 0)
    }

    private func addContainer(
        server: UPnPDevice,
        objectID: String,
        sortCriteria: String,
        depth: Int
    ) async throws -> Int {
        // ponytail: depth cap guards against pathological trees; raise it or add
        // a track-count limit/cancel if recursing huge tag containers bites.
        guard depth <= Self.maxAddDepth else { return 0 }
        var added = 0
        var index = 0
        while true {
            let page = try await browse(
                server: server,
                objectID: objectID,
                startingIndex: index,
                requestedCount: Self.browsePageSize,
                sortCriteria: sortCriteria
            )
            guard !page.objects.isEmpty else { break }
            for object in page.objects {
                if object.isContainer {
                    added += try await addContainer(server: server, objectID: object.id, sortCriteria: sortCriteria, depth: depth + 1)
                } else if addTrackToPlaylist(object) {
                    added += 1
                }
            }
            index += page.objects.count
            if index >= page.totalMatches || page.objects.count < Self.browsePageSize { break }
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

    public func clearPlaylist() {
        playlist.clear()
    }

    public func removeFromPlaylist(at index: Int) {
        playlist.remove(at: index)
    }

    public func movePlaylistItem(fromOffsets: IndexSet, toOffset: Int) {
        playlist.move(fromOffsets: fromOffsets, toOffset: toOffset)
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
        try data.write(to: fileURL)
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
        return added
    }

    public var bitPerfectMode: Bool {
        get { player.bitPerfectMode }
        set { player.bitPerfectMode = newValue }
    }

    public func getCurrentDeviceSampleRate() throws -> Float64 {
        try player.getCurrentDeviceSampleRate()
    }

    public func getFileSampleRate() -> Float64? {
        player.getFileSampleRate()
    }

    public func getFileFormat() -> String? {
        currentEngine.getFileFormat()
    }

    deinit {
        upnpManager.stopDiscovery()
        mediaServer.stop()
    }
}
