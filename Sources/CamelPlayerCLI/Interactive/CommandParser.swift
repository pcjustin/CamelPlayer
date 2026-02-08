import CamelPlayerCore
import Foundation

public enum PlayerCommand {
    case play(index: Int?)
    case pause
    case resume
    case stop
    case next
    case previous
    case seek(time: TimeInterval)
    case volume(level: Float)
    case add(paths: [String])
    case list
    case remove(index: Int)
    case clear
    case mode(PlaybackMode)
    case device(id: UInt32?)
    case status
    case help
    case quit
    case unknown(String)
}

public struct CommandParser {
    public init() {}

    public func parse(_ input: String) -> PlayerCommand {
        let trimmed = input.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else {
            return .unknown("")
        }

        let parts = trimmed.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: true)
        guard let command = parts.first?.lowercased() else {
            return .unknown(trimmed)
        }

        let args = parts.count > 1 ? String(parts[1]) : ""

        switch command {
        case "play", "p":
            if args.isEmpty {
                return .play(index: nil)
            }
            if let index = Int(args) {
                return .play(index: index)
            }
            return .add(paths: [args])

        case "pause":
            return .pause

        case "resume", "r":
            return .resume

        case "stop", "s":
            return .stop

        case "next", "n":
            return .next

        case "previous", "prev":
            return .previous

        case "seek":
            if let time = parseTime(args) {
                return .seek(time: time)
            }
            return .unknown("Invalid time format. Use seconds or MM:SS")

        case "volume", "vol", "v":
            if let level = Float(args) {
                return .volume(level: level / 100.0)
            }
            return .unknown("Invalid volume level. Use 0-100")

        case "add", "a":
            let paths = parsePaths(args)
            return .add(paths: paths)

        case "list", "l":
            return .list

        case "remove", "rm":
            if let index = Int(args) {
                return .remove(index: index)
            }
            return .unknown("Invalid index")

        case "clear":
            return .clear

        case "mode", "m":
            switch args.lowercased() {
            case "sequential", "seq":
                return .mode(.sequential)
            case "loop", "l":
                return .mode(.loop)
            case "loopone", "one":
                return .mode(.loopOne)
            case "shuffle", "sh":
                return .mode(.shuffle)
            default:
                return .unknown("Unknown mode. Use: sequential, loop, loopone, shuffle")
            }

        case "device", "dev", "d":
            if args.isEmpty {
                return .device(id: nil)
            }
            if let deviceID = UInt32(args) {
                return .device(id: deviceID)
            }
            return .unknown("Invalid device ID")

        case "status", "st":
            return .status

        case "help", "h", "?":
            return .help

        case "quit", "q", "exit":
            return .quit

        default:
            return .unknown(trimmed)
        }
    }

    private func parseTime(_ input: String) -> TimeInterval? {
        let parts = input.split(separator: ":")

        if parts.count == 1 {
            return Double(input)
        } else if parts.count == 2 {
            guard let minutes = Double(parts[0]),
                  let seconds = Double(parts[1]) else {
                return nil
            }
            return minutes * 60 + seconds
        }

        return nil
    }

    private func parsePaths(_ input: String) -> [String] {
        var paths: [String] = []
        var currentPath = ""
        var inQuotes = false
        var escapeNext = false

        for char in input {
            if escapeNext {
                currentPath.append(char)
                escapeNext = false
                continue
            }

            if char == "\\" {
                escapeNext = true
                continue
            }

            if char == "\"" {
                inQuotes.toggle()
                continue
            }

            if char == " " && !inQuotes {
                if !currentPath.isEmpty {
                    paths.append(currentPath.trimmingCharacters(in: .whitespaces))
                    currentPath = ""
                }
            } else {
                currentPath.append(char)
            }
        }

        if !currentPath.isEmpty {
            paths.append(currentPath.trimmingCharacters(in: .whitespaces))
        }

        return paths.isEmpty ? [input] : paths
    }
}
