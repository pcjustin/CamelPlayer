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
                print("Warning: Track played for only \(playDuration)s, stopping auto-play to prevent rapid skipping")
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
                try await currentEngine.loadAndPlay(url: nextItem.url)
            } catch {
                // Silently fail - could log error in future
                print("Error auto-playing next track: \(error.localizedDescription)")
            }
        }
    }

    public func addToPlaylist(url: URL) {
        playlist.add(url: url)
    }

    public func addToPlaylist(urls: [URL]) {
        playlist.addAll(urls: urls)
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
        try await currentEngine.loadAndPlay(url: item.url)
    }

    public func playItem(at index: Int) async throws {
        guard let item = playlist.jumpTo(index: index) else {
            throw AudioPlayerError.fileLoadError("Invalid playlist index")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url)
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
        try await currentEngine.loadAndPlay(url: item.url)
    }

    public func previous() async throws {
        guard let item = playlist.previous() else {
            throw AudioPlayerError.fileLoadError("No previous item")
        }

        lastPlayStartTime = Date()
        try await currentEngine.loadAndPlay(url: item.url)
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
                    try await currentEngine.loadAndPlay(url: currentItem.url)
                } catch {
                    print("Error resuming playback on new device: \(error)")
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

    /// Sort fields the server supports, or an empty array if none/unavailable.
    public func sortCapabilities(server: UPnPDevice) async -> [String] {
        guard let controlURL = server.contentDirectoryURL else { return [] }
        let service = ContentDirectoryService(controlURL: controlURL)
        return (try? await service.getSortCapabilities()) ?? []
    }

    /// Adds every playable track directly inside a container to the playlist,
    /// paging through the whole container. Returns the number of tracks added.
    /// (Immediate children only — nested folders are not recursed.)
    @discardableResult
    public func addContainerToPlaylist(
        server: UPnPDevice,
        objectID: String,
        sortCriteria: String = ""
    ) async throws -> Int {
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
            for object in page.objects where !object.isContainer {
                guard let res = object.resURL, let url = URL(string: res) else { continue }
                playlist.add(PlaylistItem(url: url, title: object.title))
                added += 1
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
