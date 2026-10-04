import Foundation
import Swifter
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

/// HTTP server for sharing local media files with UPnP devices
public class LocalMediaServer {
    private let server: HttpServer
    /// Guarded by filesLock: written by callers, read on Swifter's request threads.
    /// Keys are random UUIDs so LAN peers cannot enumerate shared files.
    private var sharedFiles: [String: URL] = [:]
    private let filesLock = NSLock()
    private let preferredPort: UInt16
    private let portRange: UInt16 = 10
    private var activePort: UInt16
    private var isRunning = false

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

            self.filesLock.lock()
            let sharedURL = request.params[":id"].flatMap { self.sharedFiles[$0] }
            self.filesLock.unlock()
            guard let fileURL = sharedURL else {
                return .notFound
            }

            return self.response(for: fileURL, method: request.method, range: request.headers["range"])
        }

        // Health check endpoint
        server["/health"] = { _ in
            return .ok(.text("OK"))
        }
    }

    func response(for fileURL: URL, method: String = "GET", range: String? = nil) -> HttpResponse {
        guard method == "GET" || method == "HEAD" else {
            return .raw(405, "Method Not Allowed", ["Allow": "GET, HEAD", "Content-Length": "0"], nil)
        }
        guard let fileSize = fileSize(of: fileURL) else { return .notFound }
        let mimeType = getMimeType(for: fileURL)
        if method == "HEAD" {
            return .raw(200, "OK", ["Content-Type": mimeType, "Content-Length": String(fileSize),
                                    "Accept-Ranges": "bytes"], nil)
        }
        if let range = range?.trimmingCharacters(in: .whitespaces), range.hasPrefix("bytes=") {
            let parts = range.dropFirst(6).split(separator: "-", omittingEmptySubsequences: false)
            // Unsupported multipart and malformed ranges may be ignored.
            if parts.count == 2, !(parts[0].isEmpty && parts[1].isEmpty),
               parts.allSatisfy({ $0.utf8.allSatisfy { (48...57).contains($0) } }) {
                let start: Int
                let end: Int
                if parts[0].isEmpty {
                    let suffix = Int(parts[1]) ?? Int.max
                    start = max(0, fileSize - suffix)
                    end = fileSize - 1
                } else {
                    start = Int(parts[0]) ?? Int.max
                    end = parts[1].isEmpty ? fileSize - 1 : min(Int(parts[1]) ?? Int.max, fileSize - 1)
                }
                guard start < fileSize, start <= end else {
                    return .raw(416, "Range Not Satisfiable",
                                ["Content-Range": "bytes */\(fileSize)", "Content-Length": "0"], nil)
                }
                return streamResponse(fileURL: fileURL, start: start, end: end,
                                      fileSize: fileSize, mimeType: mimeType, partial: true)
            }
        }
        return streamResponse(fileURL: fileURL, start: 0, end: fileSize - 1,
                              fileSize: fileSize, mimeType: mimeType, partial: false)
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
            let handle = try FileHandle(forReadingFrom: fileURL)
            defer { try? handle.close() }
            try handle.seek(toOffset: UInt64(start))

            var remaining = length
            while remaining > 0 {
                let toRead = min(Self.chunkSize, remaining)
                let chunk = try handle.read(upToCount: toRead) ?? Data()
                if chunk.isEmpty { break }
                try writer.write(chunk)
                remaining -= chunk.count
            }
        }
    }

    /// Returns the size of a file in bytes, or nil if it cannot be determined.
    private func fileSize(of url: URL) -> Int? {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              attributes[.type] as? FileAttributeType == .typeRegular,
              let size = (attributes[.size] as? NSNumber)?.intValue, size >= 0 else {
            return nil
        }
        return size
    }

    /// Starts the HTTP server
    public func start() throws {
        guard !isRunning else {
            coreLog("HTTP Server: Already running on port \(activePort)")
            return
        }

        var lastError: Error?
        let lastPort = UInt16(min(Int(UInt16.max), Int(preferredPort) + Int(portRange)))
        for candidate in preferredPort...lastPort {
            coreLog("HTTP Server: Starting on port \(candidate)...")
            do {
                try server.start(candidate, forceIPv4: true)
                activePort = UInt16(try server.port())
                isRunning = true
                coreLog("HTTP Server: Successfully started on port \(candidate)")

                if let ip = getLocalIPAddress() {
                    coreLog("HTTP Server: Server accessible at http://\(ip):\(candidate)")
                } else {
                    coreLog("HTTP Server: WARNING - Could not determine local IP address!")
                }
                return
            } catch {
                lastError = error
                coreLog("HTTP Server: Port \(candidate) unavailable: \(error)")
            }
        }

        coreLog("HTTP Server: Failed to start on any port in range \(preferredPort)-\(lastPort)")
        throw ServerError.failedToStart(lastError ?? ServerError.cannotDetermineIP)
    }

    /// Stops the HTTP server
    public func stop() {
        guard isRunning else { return }
        server.stop()
        isRunning = false
        filesLock.lock()
        sharedFiles.removeAll()
        filesLock.unlock()
    }

    /// Shares a local file and returns its HTTP URL
    /// - Parameter fileURL: Local file URL
    /// - Returns: HTTP URL that UPnP devices can access
    public func shareFile(_ fileURL: URL) throws -> URL {
        coreLog("HTTP Server: Sharing file: \(fileURL.lastPathComponent)")

        guard fileURL.isFileURL, fileSize(of: fileURL) != nil,
              FileManager.default.isReadableFile(atPath: fileURL.path) else {
            throw ServerError.invalidFile
        }
        try start()
        guard let ip = getLocalIPAddress() else { throw ServerError.cannotDetermineIP }

        filesLock.lock()
        let id: String
        if let existing = sharedFiles.first(where: { $0.value == fileURL })?.key {
            // Already shared (e.g. re-played or preloaded): reuse the entry.
            id = existing
        } else {
            id = UUID().uuidString
            sharedFiles[id] = fileURL
        }
        filesLock.unlock()

        let urlString = "http://\(ip):\(activePort)/media/\(id)"
        guard let url = URL(string: urlString) else {
            coreLog("HTTP Server: ERROR - Invalid URL: \(urlString)")
            throw ServerError.invalidURL
        }

        coreLog("HTTP Server: File shared at: \(url)")
        return url
    }

    /// Gets the local IP address
    /// - Returns: IP address string or nil
    public func getLocalIPAddress() -> String? {
        var preferredAddress: String?
        var fallbackAddress: String?

        // Get list of all interfaces on the local machine
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0 else {
            coreLog("HTTP Server: Failed to get network interfaces")
            return nil
        }
        guard let firstAddr = ifaddr else {
            coreLog("HTTP Server: No network interfaces found")
            return nil
        }

        defer { freeifaddrs(ifaddr) }

        coreLog("HTTP Server: Scanning network interfaces for IP address...")

        // Iterate through linked list of interfaces
        for ifptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let interface = ifptr.pointee
            guard interface.ifa_flags & UInt32(IFF_UP) != 0,
                  interface.ifa_flags & UInt32(IFF_LOOPBACK) == 0 else { continue }

            // Some interfaces (utun, awdl) report no address at all.
            guard let ifaAddr = interface.ifa_addr else { continue }

            // Check for IPv4 interface
            let addrFamily = ifaAddr.pointee.sa_family
            if addrFamily == sa_family_t(AF_INET) {
                // Get interface name
                let name = String(cString: interface.ifa_name)

                var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                // sockaddr.sa_len is BSD-only; AF_INET length is sockaddr_in on all platforms.
                guard getnameinfo(ifaAddr, socklen_t(MemoryLayout<sockaddr_in>.size),
                           &hostname, socklen_t(hostname.count),
                           nil, socklen_t(0), NI_NUMERICHOST) == 0 else { continue }
                let ipAddress = String(cString: hostname)

                coreLog("HTTP Server: Found interface \(name) with IP \(ipAddress)")

                // Skip localhost
                if ipAddress == "127.0.0.1" {
                    continue
                }

                // Prefer en0 (Wi-Fi) or en1 (Ethernet)
                if name == "en0" {
                    preferredAddress = ipAddress
                    coreLog("HTTP Server: Using preferred interface en0: \(ipAddress)")
                    break
                } else if name == "en1" && preferredAddress == nil {
                    preferredAddress = ipAddress
                    coreLog("HTTP Server: Using preferred interface en1: \(ipAddress)")
                } else if fallbackAddress == nil {
                    // Use any other non-localhost IPv4 as fallback
                    fallbackAddress = ipAddress
                }
            }
        }

        let finalAddress = preferredAddress ?? fallbackAddress

        if let addr = finalAddress {
            coreLog("HTTP Server: Selected IP address: \(addr)")
        } else {
            coreLog("HTTP Server: ERROR - No valid IP address found!")
            coreLog("HTTP Server: Make sure you're connected to a network (Wi-Fi or Ethernet)")
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

    deinit { stop() }
}

// MARK: - Server Errors

public enum ServerError: LocalizedError {
    case failedToStart(Error)
    case cannotDetermineIP
    case invalidURL
    case invalidFile

    public var errorDescription: String? {
        switch self {
        case .failedToStart(let error): return "Could not start the local media server: \(error.localizedDescription)"
        case .cannotDetermineIP: return "No network address to share local files with the renderer"
        case .invalidURL: return "Could not build a media URL for the renderer"
        case .invalidFile: return "File not found or not readable"
        }
    }
}
