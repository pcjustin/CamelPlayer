import CamelPlayerCore
import Foundation

public class InteractiveMode {
    private let controller: PlaybackController
    private let parser = CommandParser()
    private var isRunning = false
    private var statusThread: Thread?

    public init() throws {
        controller = try PlaybackController()
    }

    public func run() {
        isRunning = true
        printWelcome()

        while isRunning {
            print("\n> ", terminator: "")
            fflush(stdout)

            guard let input = readLine() else {
                break
            }

            let command = parser.parse(input)
            handleCommand(command)
        }

        print("Goodbye!")
    }

    private func printWelcome() {
        print("""

        CamelPlayer - Interactive Mode
        ==============================
        Type 'help' for available commands
        """)
    }

    private func handleCommand(_ command: PlayerCommand) {
        do {
            switch command {
            case .play(let index):
                if let idx = index {
                    try controller.playItem(at: idx)
                    if let item = controller.currentItem {
                        print("Playing: \(item.title)")
                    }
                } else {
                    try controller.play()
                    if let item = controller.currentItem {
                        print("Playing: \(item.title)")
                    }
                }

            case .pause:
                controller.pause()
                print("Paused")

            case .resume:
                try controller.resume()
                print("Resumed")

            case .stop:
                controller.stop()
                print("Stopped")

            case .next:
                try controller.next()
                if let item = controller.currentItem {
                    print("Playing: \(item.title)")
                }

            case .previous:
                try controller.previous()
                if let item = controller.currentItem {
                    print("Playing: \(item.title)")
                }

            case .seek(let time):
                try controller.seek(to: time)
                print("Seeked to \(formatTime(time))")

            case .volume(let level):
                controller.volume = level
                print("Volume set to \(Int(level * 100))%")

            case .add(let paths):
                var addedCount = 0
                for path in paths {
                    let urls = expandPath(path)
                    for url in urls {
                        controller.addToPlaylist(url: url)
                        addedCount += 1
                    }
                }
                print("Added \(addedCount) file(s) to playlist")

            case .list:
                printPlaylist()

            case .remove(let index):
                controller.removeFromPlaylist(at: index)
                print("Removed item at index \(index)")

            case .clear:
                controller.clearPlaylist()
                print("Playlist cleared")

            case .mode(let mode):
                controller.playbackMode = mode
                print("Playback mode set to \(mode)")

            case .device(let deviceID):
                if let id = deviceID {
                    try controller.setOutputDevice(deviceID: id)
                    print("Output device set to ID: \(id)")
                } else {
                    let devices = try controller.listOutputDevices()
                    let currentID = try controller.getCurrentOutputDevice()
                    print("\nAvailable output devices:")
                    for device in devices {
                        let marker = device.id == currentID ? "*" : " "
                        print("  \(marker) [\(device.id)] \(device.name)")
                    }
                }

            case .bitPerfect(let enabled):
                if let enable = enabled {
                    controller.bitPerfectMode = enable
                    print("Bit-perfect mode: \(enable ? "ON" : "OFF")")
                } else {
                    let isEnabled = controller.bitPerfectMode
                    print("Bit-perfect mode: \(isEnabled ? "ON" : "OFF")")
                }

            case .info:
                printAudioInfo()

            case .status:
                printStatus()

            case .help:
                printHelp()

            case .quit:
                isRunning = false

            case .unknown(let input):
                if !input.isEmpty {
                    print("Unknown command: \(input). Type 'help' for available commands.")
                }
            }
        } catch {
            if let playerError = error as? AudioPlayerError {
                switch playerError {
                case .fileNotFound:
                    print("Error: File not found")
                case .unsupportedFormat:
                    print("Error: Unsupported audio format")
                case .audioEngineError(let message):
                    print("Error: Audio engine error - \(message)")
                case .fileLoadError(let message):
                    print("Error: Failed to load file - \(message)")
                }
            } else if let deviceError = error as? OutputDeviceError {
                switch deviceError {
                case .deviceNotFound:
                    print("Error: Audio device not found")
                case .deviceSetupFailed(let message):
                    print("Error: Device setup failed - \(message)")
                case .propertyAccessFailed(let message):
                    print("Error: Property access failed - \(message)")
                }
            } else {
                print("Error: \(error.localizedDescription)")
            }
        }
    }

    private func printPlaylist() {
        let items = controller.getPlaylistItems()
        let currentPos = controller.getCurrentPosition()

        if items.isEmpty {
            print("Playlist is empty")
            return
        }

        print("\nPlaylist (\(items.count) items):")
        for (index, item) in items.enumerated() {
            let marker = index == currentPos ? ">" : " "
            print("  \(marker) [\(index)] \(item.title)")
        }
    }

    private func printStatus() {
        let state = controller.currentState
        let volume = Int(controller.volume * 100)
        let mode = controller.playbackMode

        print("\nStatus:")
        print("  State: \(state)")
        print("  Volume: \(volume)%")
        print("  Mode: \(mode)")
        print("  Bit-perfect: \(controller.bitPerfectMode ? "ON" : "OFF")")

        if let item = controller.currentItem {
            print("  Current: \(item.title)")

            let current = controller.currentTime
            if let total = controller.duration {
                print("  Progress: \(formatTime(current)) / \(formatTime(total))")
            }
        } else {
            print("  Current: None")
        }

        print("  Playlist: \(controller.getPlaylistCount()) items")
    }

    private func printAudioInfo() {
        do {
            let deviceSampleRate = try controller.getCurrentDeviceSampleRate()
            print("\nAudio Information:")
            print("  Device sample rate: \(Int(deviceSampleRate)) Hz")

            if let fileSampleRate = controller.getFileSampleRate() {
                print("  File sample rate: \(Int(fileSampleRate)) Hz")

                if abs(deviceSampleRate - fileSampleRate) < 0.1 {
                    print("  ✓ Sample rates match (Bit-perfect)")
                } else {
                    print("  ⚠ Sample rates differ (Resampling active)")
                }
            }

            if let format = controller.getFileFormat() {
                print("  File format: \(format)")
            }

            if let item = controller.currentItem {
                print("  Current file: \(item.title)")
            }

            print("  Bit-perfect mode: \(controller.bitPerfectMode ? "ON" : "OFF")")
        } catch {
            print("Error getting audio info: \(error.localizedDescription)")
        }
    }

    private func printHelp() {
        print("""

        Available Commands:
        ===================

        Playback Control:
          play, p [index]      - Play current or specified track
          pause                - Pause playback
          resume, r            - Resume playback
          stop, s              - Stop playback
          next, n              - Play next track
          previous, prev       - Play previous track
          seek <time>          - Seek to time (seconds or MM:SS)

        Playlist Management:
          add, a <path>        - Add file(s) or folder to playlist (auto-scans folders)
          list, l              - Show playlist
          remove, rm <index>   - Remove item from playlist
          clear                - Clear playlist

        Settings:
          volume, vol, v <0-100> - Set volume
          mode, m <mode>       - Set playback mode (sequential, loop, loopone, shuffle)
          device, dev, d [id]  - List or set output device
          bitperfect, bp [on|off] - Enable/disable bit-perfect mode (auto-matches sample rate)

        Information:
          info, i              - Show audio format and bit-perfect status
          status, st           - Show playback status
          help, h, ?           - Show this help

        Other:
          quit, q, exit        - Exit interactive mode
        """)
    }

    private func formatTime(_ time: TimeInterval) -> String {
        let totalSeconds = Int(time)
        let minutes = totalSeconds / 60
        let seconds = totalSeconds % 60
        return String(format: "%d:%02d", minutes, seconds)
    }

    private func expandPath(_ path: String) -> [URL] {
        let fileManager = FileManager.default
        let url = URL(fileURLWithPath: path)

        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory) else {
            print("Warning: Path does not exist: \(path)")
            return []
        }

        if isDirectory.boolValue {
            return scanDirectory(url)
        } else {
            return isAudioFile(url) ? [url] : []
        }
    }

    private func scanDirectory(_ directoryURL: URL) -> [URL] {
        let fileManager = FileManager.default
        var audioFiles: [URL] = []

        guard let enumerator = fileManager.enumerator(
            at: directoryURL,
            includingPropertiesForKeys: [.isRegularFileKey, .nameKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }

        for case let fileURL as URL in enumerator {
            if isAudioFile(fileURL) {
                audioFiles.append(fileURL)
            }
        }

        return audioFiles.sorted { $0.path < $1.path }
    }

    private func isAudioFile(_ url: URL) -> Bool {
        let audioExtensions = ["mp3", "wav", "m4a", "flac", "alac", "aac", "aiff", "caf"]
        let ext = url.pathExtension.lowercased()
        return audioExtensions.contains(ext)
    }
}
