import Foundation
import Swifter

/// HTTP server for sharing local media files with UPnP devices
public class LocalMediaServer {
    private let server: HttpServer
    private var sharedFiles: [String: URL] = [:]
    private var nextID = 1
    private let preferredPort: UInt16
    private let portRange: UInt16 = 10
    private var activePort: UInt16

    public var isRunning: Bool {
        // Swifter doesn't provide a public API to check if running, so we track it ourselves
        return _isRunning
    }
    private var _isRunning = false

    /// Initializes the media server
    /// - Parameter port: Preferred port to listen on (default 8080)
    public init(port: UInt16 = 8080) {
        self.preferredPort = port
        self.activePort = port
        self.server = HttpServer()
        setupRoutes()
    }

    /// Chunk size used when streaming files to clients.
    private static let chunkSize = 64 * 1024

    /// Sets up HTTP routes
    private func setupRoutes() {
        // Route for serving media files
        server["/media/:id"] = { [weak self] request in
            guard let self = self else {
                return .notFound
            }

            guard let id = request.params[":id"],
                  let fileURL = self.sharedFiles[id] else {
                return .notFound
            }

            guard let fileSize = self.fileSize(of: fileURL) else {
                return .internalServerError
            }

            let mimeType = self.getMimeType(for: fileURL)

            // Support range requests for seeking
            if let rangeHeader = request.headers["range"] {
                return self.handleRangeRequest(fileURL: fileURL, fileSize: fileSize, range: rangeHeader, mimeType: mimeType)
            }

            return self.streamResponse(fileURL: fileURL, start: 0, end: fileSize - 1, fileSize: fileSize, mimeType: mimeType, partial: false)
        }

        // Health check endpoint
        server["/health"] = { _ in
            return .ok(.text("OK"))
        }
    }

    /// Handles HTTP range requests for seeking support
    private func handleRangeRequest(fileURL: URL, fileSize: Int, range: String, mimeType: String) -> HttpResponse {
        // Parse range header (format: "bytes=start-end")
        let rangePattern = "bytes=(\\d+)-(\\d*)"
        guard let regex = try? NSRegularExpression(pattern: rangePattern),
              let match = regex.firstMatch(in: range, range: NSRange(range.startIndex..., in: range)),
              let startString = Range(match.range(at: 1), in: range).map({ String(range[$0]) }),
              let start = Int(startString) else {
            return streamResponse(fileURL: fileURL, start: 0, end: fileSize - 1, fileSize: fileSize, mimeType: mimeType, partial: false)
        }

        let end: Int
        if let endString = Range(match.range(at: 2), in: range).map({ String(range[$0]) }),
           !endString.isEmpty,
           let parsedEnd = Int(endString) {
            end = min(parsedEnd, fileSize - 1)
        } else {
            end = fileSize - 1
        }

        guard start <= end && start < fileSize else {
            return streamResponse(fileURL: fileURL, start: 0, end: fileSize - 1, fileSize: fileSize, mimeType: mimeType, partial: false)
        }

        return streamResponse(fileURL: fileURL, start: start, end: end, fileSize: fileSize, mimeType: mimeType, partial: true)
    }

    /// Streams the requested byte range of a file to the client in chunks,
    /// avoiding loading the whole file into memory.
    private func streamResponse(fileURL: URL, start: Int, end: Int, fileSize: Int, mimeType: String, partial: Bool) -> HttpResponse {
        let length = end - start + 1
        var headers = [
            "Content-Type": mimeType,
            "Content-Length": String(length),
            "Accept-Ranges": "bytes"
        ]
        if partial {
            headers["Content-Range"] = "bytes \(start)-\(end)/\(fileSize)"
        }

        let statusCode = partial ? 206 : 200
        let statusText = partial ? "Partial Content" : "OK"

        return HttpResponse.raw(statusCode, statusText, headers) { writer in
            guard let handle = try? FileHandle(forReadingFrom: fileURL) else { return }
            defer { try? handle.close() }
            try handle.seek(toOffset: UInt64(start))

            var remaining = length
            while remaining > 0 {
                let toRead = min(Self.chunkSize, remaining)
                let chunk = handle.readData(ofLength: toRead)
                if chunk.isEmpty { break }
                try writer.write(chunk)
                remaining -= chunk.count
            }
        }
    }

    /// Returns the size of a file in bytes, or nil if it cannot be determined.
    private func fileSize(of url: URL) -> Int? {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = (attributes[.size] as? NSNumber)?.intValue else {
            return nil
        }
        return size
    }

    /// Starts the HTTP server
    public func start() throws {
        guard !_isRunning else {
            print("HTTP Server: Already running on port \(activePort)")
            return
        }

        var lastError: Error?
        for candidate in preferredPort...(preferredPort + portRange) {
            print("HTTP Server: Starting on port \(candidate)...")
            do {
                try server.start(candidate, forceIPv4: true)
                activePort = candidate
                _isRunning = true
                print("HTTP Server: Successfully started on port \(candidate)")

                if let ip = getLocalIPAddress() {
                    print("HTTP Server: Server accessible at http://\(ip):\(candidate)")
                } else {
                    print("HTTP Server: WARNING - Could not determine local IP address!")
                }
                return
            } catch {
                lastError = error
                print("HTTP Server: Port \(candidate) unavailable: \(error)")
            }
        }

        print("HTTP Server: Failed to start on any port in range \(preferredPort)-\(preferredPort + portRange)")
        throw ServerError.failedToStart(lastError ?? ServerError.cannotDetermineIP)
    }

    /// Stops the HTTP server
    public func stop() {
        guard _isRunning else { return }
        server.stop()
        _isRunning = false
        sharedFiles.removeAll()
    }

    /// Shares a local file and returns its HTTP URL
    /// - Parameter fileURL: Local file URL
    /// - Returns: HTTP URL that UPnP devices can access
    public func shareFile(_ fileURL: URL) throws -> URL {
        print("HTTP Server: Sharing file: \(fileURL.lastPathComponent)")

        let id = String(nextID)
        nextID += 1

        sharedFiles[id] = fileURL

        guard let ip = getLocalIPAddress() else {
            print("HTTP Server: ERROR - Cannot determine local IP address")
            throw ServerError.cannotDetermineIP
        }

        let urlString = "http://\(ip):\(activePort)/media/\(id)"
        guard let url = URL(string: urlString) else {
            print("HTTP Server: ERROR - Invalid URL: \(urlString)")
            throw ServerError.invalidURL
        }

        print("HTTP Server: File shared at: \(url)")
        return url
    }

    /// Removes a shared file
    /// - Parameter id: File ID to remove
    public func unshareFile(id: String) {
        sharedFiles.removeValue(forKey: id)
    }

    /// Removes all shared files
    public func unshareAll() {
        sharedFiles.removeAll()
        nextID = 1
    }

    /// Gets the local IP address
    /// - Returns: IP address string or nil
    public func getLocalIPAddress() -> String? {
        var preferredAddress: String?
        var fallbackAddress: String?

        // Get list of all interfaces on the local machine
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0 else {
            print("HTTP Server: Failed to get network interfaces")
            return nil
        }
        guard let firstAddr = ifaddr else {
            print("HTTP Server: No network interfaces found")
            return nil
        }

        defer { freeifaddrs(ifaddr) }

        print("HTTP Server: Scanning network interfaces for IP address...")

        // Iterate through linked list of interfaces
        for ifptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let interface = ifptr.pointee

            // Check for IPv4 interface
            let addrFamily = interface.ifa_addr.pointee.sa_family
            if addrFamily == UInt8(AF_INET) {
                // Get interface name
                let name = String(cString: interface.ifa_name)

                var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                getnameinfo(interface.ifa_addr, socklen_t(interface.ifa_addr.pointee.sa_len),
                           &hostname, socklen_t(hostname.count),
                           nil, socklen_t(0), NI_NUMERICHOST)
                let ipAddress = String(cString: hostname)

                print("HTTP Server: Found interface \(name) with IP \(ipAddress)")

                // Skip localhost
                if ipAddress == "127.0.0.1" {
                    continue
                }

                // Prefer en0 (Wi-Fi) or en1 (Ethernet)
                if name == "en0" {
                    preferredAddress = ipAddress
                    print("HTTP Server: Using preferred interface en0: \(ipAddress)")
                    break
                } else if name == "en1" && preferredAddress == nil {
                    preferredAddress = ipAddress
                    print("HTTP Server: Using preferred interface en1: \(ipAddress)")
                } else if fallbackAddress == nil {
                    // Use any other non-localhost IPv4 as fallback
                    fallbackAddress = ipAddress
                }
            }
        }

        let finalAddress = preferredAddress ?? fallbackAddress

        if let addr = finalAddress {
            print("HTTP Server: Selected IP address: \(addr)")
        } else {
            print("HTTP Server: ERROR - No valid IP address found!")
            print("HTTP Server: Make sure you're connected to a network (Wi-Fi or Ethernet)")
        }

        return finalAddress
    }

    /// Gets the MIME type for a file
    private func getMimeType(for url: URL) -> String {
        let ext = url.pathExtension.lowercased()
        switch ext {
        case "mp3":
            return "audio/mpeg"
        case "m4a", "m4b", "m4p":
            return "audio/mp4"
        case "flac":
            return "audio/flac"
        case "wav":
            return "audio/wav"
        case "aac":
            return "audio/aac"
        case "ogg":
            return "audio/ogg"
        case "opus":
            return "audio/opus"
        case "wma":
            return "audio/x-ms-wma"
        case "aiff", "aif":
            return "audio/aiff"
        default:
            return "application/octet-stream"
        }
    }
}

// MARK: - Server Errors

public enum ServerError: Error {
    case failedToStart(Error)
    case cannotDetermineIP
    case invalidURL
}
