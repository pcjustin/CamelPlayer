import AVFoundation
import CoreAudio
import Foundation

public class PlaybackController {
    private let player: AudioPlayer
    private let playlist: Playlist
    private let volumeController: VolumeController

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
    }

    public func addToPlaylist(url: URL) {
        playlist.add(url: url)
    }

    public func addToPlaylist(urls: [URL]) {
        playlist.addAll(urls: urls)
    }

    public func play() throws {
        guard let item = playlist.currentItem else {
            throw AudioPlayerError.fileLoadError("No items in playlist")
        }

        try player.load(url: item.url)
        try player.play()
    }

    public func playItem(at index: Int) throws {
        guard let item = playlist.jumpTo(index: index) else {
            throw AudioPlayerError.fileLoadError("Invalid playlist index")
        }

        try player.load(url: item.url)
        try player.play()
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

        try player.load(url: item.url)
        try player.play()
    }

    public func previous() throws {
        guard let item = playlist.previous() else {
            throw AudioPlayerError.fileLoadError("No previous item")
        }

        try player.load(url: item.url)
        try player.play()
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
