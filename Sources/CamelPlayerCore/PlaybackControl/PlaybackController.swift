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
    private let volumeController: VolumeController
    private var lastPlayStartTime: Date?
    private let minimumPlayDuration: TimeInterval = 0.5 // 最小播放時間閾值

    // UPnP support
    private let upnpManager: UPnPDeviceManager
    private let mediaServer: LocalMediaServer
    private var currentEngine: PlaybackEngine
    private var localEngine: LocalPlaybackEngine
    private(set) public var currentOutputDevice: OutputDevice

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
        volumeController = VolumeController(mixerNode: player.mixerNode)

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

        // Start HTTP server and UPnP discovery
        try? mediaServer.start()
        upnpManager.startDiscovery()

        // Set up auto-play next track when current track finishes
        currentEngine.onPlaybackFinished = { [weak self] in
            self?.playNextIfAvailable()
        }
    }

    private func playNextIfAvailable() {
        // 檢查上一首歌曲是否播放了足夠長的時間
        // 如果播放時間太短，可能是文件加載失敗，停止自動播放以避免連續跳轉
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
        // 如果已經在播放，直接返回
        if currentEngine.state == .playing {
            return
        }

        // 如果是暫停狀態，直接恢復播放
        if currentEngine.state == .paused {
            lastPlayStartTime = Date()
            try await currentEngine.play()
            return
        }

        // 否則是 stopped 狀態，需要加載文件
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

        // Add UPnP devices
        for upnpDevice in upnpManager.availableDevices {
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
        // Stop current playback
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

    @available(*, deprecated, message: "Use listAllOutputDevices() instead")
    public func listOutputDevices() throws -> [AudioDevice] {
        try player.listOutputDevices()
    }

    @available(*, deprecated, message: "Use setOutputDevice(_ device: OutputDevice) instead")
    public func setOutputDevice(deviceID: AudioDeviceID) throws {
        try player.setOutputDevice(deviceID: deviceID)
    }

    @available(*, deprecated, message: "Use currentOutputDevice instead")
    public func getCurrentOutputDevice() throws -> AudioDeviceID {
        try player.getCurrentOutputDevice()
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
