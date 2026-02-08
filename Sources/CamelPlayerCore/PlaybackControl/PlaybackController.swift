import AVFoundation
import CoreAudio
import Foundation

public class PlaybackController {
    private let player: AudioPlayer
    private let playlist: Playlist
    private let volumeController: VolumeController
    private var lastPlayStartTime: Date?
    private let minimumPlayDuration: TimeInterval = 0.5 // 最小播放時間閾值

    public var currentState: PlaybackState {
        player.state
    }

    public var currentItem: PlaylistItem? {
        playlist.currentItem
    }

    public var playbackMode: PlaybackMode {
        get { playlist.mode }
        set { playlist.mode = newValue }
    }

    public var volume: Float {
        get { volumeController.volume }
        set { volumeController.volume = newValue }
    }

    public var currentTime: TimeInterval {
        player.currentTime
    }

    public var duration: TimeInterval? {
        player.duration
    }

    public init() throws {
        player = try AudioPlayer()
        playlist = Playlist()
        volumeController = VolumeController(mixerNode: player.mixerNode)

        // Set up auto-play next track when current track finishes
        player.onPlaybackFinished = { [weak self] in
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

        do {
            lastPlayStartTime = Date()
            try player.loadAndPlay(url: nextItem.url)
        } catch {
            // Silently fail - could log error in future
            print("Error auto-playing next track: \(error.localizedDescription)")
        }
    }

    public func addToPlaylist(url: URL) {
        playlist.add(url: url)
    }

    public func addToPlaylist(urls: [URL]) {
        playlist.addAll(urls: urls)
    }

    public func play() throws {
        // 如果已經在播放，直接返回
        if player.state == .playing {
            return
        }

        // 如果是暫停狀態，直接恢復播放
        if player.state == .paused {
            lastPlayStartTime = Date()
            try player.play()
            return
        }

        // 否則是 stopped 狀態，需要加載文件
        guard let item = playlist.currentItem else {
            throw AudioPlayerError.fileLoadError("No items in playlist")
        }

        try player.load(url: item.url)
        lastPlayStartTime = Date()
        try player.play()
    }

    public func playItem(at index: Int) throws {
        guard let item = playlist.jumpTo(index: index) else {
            throw AudioPlayerError.fileLoadError("Invalid playlist index")
        }

        lastPlayStartTime = Date()
        try player.loadAndPlay(url: item.url)
    }

    public func pause() {
        player.pause()
    }

    public func resume() throws {
        try player.play()
    }

    public func stop() {
        player.stop()
    }

    public func next() throws {
        guard let item = playlist.next() else {
            throw AudioPlayerError.fileLoadError("No next item")
        }

        lastPlayStartTime = Date()
        try player.loadAndPlay(url: item.url)
    }

    public func previous() throws {
        guard let item = playlist.previous() else {
            throw AudioPlayerError.fileLoadError("No previous item")
        }

        lastPlayStartTime = Date()
        try player.loadAndPlay(url: item.url)
    }

    public func seek(to time: TimeInterval) throws {
        try player.seek(to: time)
    }

    public func listOutputDevices() throws -> [AudioDevice] {
        try player.listOutputDevices()
    }

    public func setOutputDevice(deviceID: AudioDeviceID) throws {
        try player.setOutputDevice(deviceID: deviceID)
    }

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
        player.getFileFormat()
    }
}
