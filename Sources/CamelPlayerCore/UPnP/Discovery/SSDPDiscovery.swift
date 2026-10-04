import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Protocol for SSDP discovery delegate
public protocol SSDPDiscoveryDelegate: AnyObject {
    func ssdpDiscovery(_ discovery: SSDPDiscovery, didDiscoverDevice device: UPnPDevice)
    func ssdpDiscovery(_ discovery: SSDPDiscovery, didRemoveDevice device: UPnPDevice)
}

/// SSDP lifecycle, sockets and discovery results are confined to one queue.
public class SSDPDiscovery: @unchecked Sendable {
    private static let multicastGroup = "239.255.255.250"
    private static let multicastPort: UInt16 = 1900
    private static let searchTargets = [
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:device:MediaServer:1"
    ]

    public weak var delegate: SSDPDiscoveryDelegate?
    private let queue = DispatchQueue(label: "CamelPlayer.SSDP")
    private let queueKey = DispatchSpecificKey<Bool>()
    private var socketFD: Int32 = -1
    private var generation = 0
    private var source: DispatchSourceRead?
    private var searchTimer: DispatchSourceTimer?
    private var discoveredDevices: [String: UPnPDevice] = [:]
    private var pendingRequests: [String: UUID] = [:]
    private var fetchTasks: [String: Task<Void, Never>] = [:]
    private let fetchDescription: (URL) async throws -> Data

    public convenience init() {
        self.init(fetchDescription: { url in
            var request = URLRequest(url: url)
            request.timeoutInterval = 15
            let (data, response) = try await URLSession.shared.data(for: request)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw SOAPError.invalidResponse }
            return data
        })
    }

    init(fetchDescription: @escaping (URL) async throws -> Data) {
        self.fetchDescription = fetchDescription
        queue.setSpecific(key: queueKey, value: true)
    }

    private func withQueue<T>(_ body: () -> T) -> T {
        if DispatchQueue.getSpecific(key: queueKey) == true { return body() }
        return queue.sync(execute: body)
    }

    public func startDiscovery() {
        withQueue {
            guard socketFD < 0, createSocket() else { return }
            generation += 1
            let sessionGeneration = generation
            let descriptor = socketFD
            let reader = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: queue)
            reader.setEventHandler { [weak self] in
                guard let self = self, self.generation == sessionGeneration else { return }
                self.readResponses(from: descriptor)
            }
            reader.setCancelHandler { close(descriptor) }
            source = reader
            reader.resume()

            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: 30)
            timer.setEventHandler { [weak self] in self?.sendMSearch() }
            searchTimer = timer
            timer.resume()
            for delay in [0.1, 0.2] {
                queue.asyncAfter(deadline: .now() + delay) { [weak self] in
                    guard let self = self, self.generation == sessionGeneration else { return }
                    self.sendMSearch()
                }
            }
        }
    }

    public func stopDiscovery() {
        withQueue {
            generation += 1
            searchTimer?.cancel()
            searchTimer = nil
            source?.cancel()
            source = nil
            socketFD = -1
            for task in fetchTasks.values { task.cancel() }
            fetchTasks.removeAll()
            pendingRequests.removeAll()
            discoveredDevices.removeAll()
        }
    }

    private func readResponses(from descriptor: Int32) {
        var buffer = [UInt8](repeating: 0, count: 8192)
        // Bound each dispatch event so an SSDP burst cannot starve stopDiscovery.
        for _ in 0..<32 {
            let count = recv(descriptor, &buffer, buffer.count, 0)
            guard count > 0 else { return }
            if let message = String(bytes: buffer.prefix(count), encoding: .utf8) { parseResponse(message) }
        }
    }

    func receive(_ response: String) {
        withQueue { parseResponse(response) }
    }

    /// Creates a UDP socket for SSDP
    private func createSocket() -> Bool {
        // Create UDP socket
        #if canImport(Darwin)
        socketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        #else
        socketFD = socket(AF_INET, Int32(SOCK_DGRAM.rawValue), Int32(IPPROTO_UDP))
        #endif
        guard socketFD >= 0 else {
            coreLog("SSDP: Failed to create socket: \(String(cString: strerror(errno)))")
            return false
        }

        // Allow socket reuse
        var reuseAddr: Int32 = 1
        guard setsockopt(socketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, socklen_t(MemoryLayout<Int32>.size)) >= 0 else {
            coreLog("SSDP: Failed to set SO_REUSEADDR: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        var reusePort: Int32 = 1
        guard setsockopt(socketFD, SOL_SOCKET, SO_REUSEPORT, &reusePort, socklen_t(MemoryLayout<Int32>.size)) >= 0 else {
            coreLog("SSDP: Failed to set SO_REUSEPORT: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        // Bind to SSDP port
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.multicastPort.bigEndian
        addr.sin_addr.s_addr = INADDR_ANY.bigEndian

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                #if canImport(Darwin)
                Darwin.bind(socketFD, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                #else
                Glibc.bind(socketFD, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                #endif
            }
        }

        guard bindResult >= 0 else {
            coreLog("SSDP: Failed to bind socket: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        // Join multicast group
        var mreq = ip_mreq()
        mreq.imr_multiaddr.s_addr = inet_addr(Self.multicastGroup)
        mreq.imr_interface.s_addr = INADDR_ANY.bigEndian

        guard setsockopt(socketFD, Int32(IPPROTO_IP), IP_ADD_MEMBERSHIP, &mreq, socklen_t(MemoryLayout<ip_mreq>.size)) >= 0 else {
            coreLog("SSDP: Failed to join multicast group: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        guard fcntl(socketFD, F_SETFL, O_NONBLOCK) >= 0 else {
            closeSocket()
            return false
        }

        coreLog("SSDP: Socket created and bound to port \(Self.multicastPort)")
        return true
    }

    /// Closes the socket
    private func closeSocket() {
        if socketFD >= 0 {
            close(socketFD)
            socketFD = -1
        }
    }

    /// Sends M-SEARCH multicast request
    private func sendMSearch() {
        for target in Self.searchTargets {
            sendMSearch(target: target)
        }
    }

    private func sendMSearch(target: String) {
        let message = """
        M-SEARCH * HTTP/1.1\r
        HOST: \(Self.multicastGroup):\(Self.multicastPort)\r
        MAN: "ssdp:discover"\r
        MX: 1\r
        ST: \(target)\r
        \r

        """

        coreLog("SSDP: Sending M-SEARCH for \(target)...")

        guard let messageData = message.data(using: .utf8) else { return }

        // Send to multicast address
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.multicastPort.bigEndian
        addr.sin_addr.s_addr = inet_addr(Self.multicastGroup)

        let bytesSent = messageData.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) -> Int in
            withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(socketFD, bytes.baseAddress, messageData.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }

        if bytesSent > 0 {
            coreLog("SSDP: M-SEARCH sent successfully (\(bytesSent) bytes)")
        } else {
            coreLog("SSDP: Failed to send M-SEARCH: \(String(cString: strerror(errno)))")
        }
    }

    /// Parses SSDP response
    private func parseResponse(_ response: String) {
        let lines = response.components(separatedBy: "\r\n")

        // Check if it's a response (not a NOTIFY)
        guard let firstLine = lines.first else { return }

        if firstLine.hasPrefix("NOTIFY") {
            handleNotify(lines)
            return
        }

        guard firstLine.hasPrefix("HTTP/1.1 200 OK") else {
            coreLog("SSDP: Received unknown message type: \(firstLine)")
            return
        }

        coreLog("SSDP: Received HTTP 200 OK response")

        var location: String?
        var usn: String?
        var st: String?

        for line in lines {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2 else { continue }

            let key = parts[0].trimmingCharacters(in: .whitespaces).uppercased()
            let value = parts[1].trimmingCharacters(in: .whitespaces)

            switch key {
            case "LOCATION":
                location = value
            case "USN":
                usn = value
            case "ST":
                st = value
            default:
                break
            }
        }

        // Verify this is a MediaRenderer
        guard let st = st,
              let location = location,
              let locationURL = URL(string: location),
              let usn = usn else {
            coreLog("SSDP: Response missing required fields")
            coreLog("  ST: \(st ?? "nil")")
            coreLog("  Location: \(location ?? "nil")")
            coreLog("  USN: \(usn ?? "nil")")
            return
        }

        coreLog("SSDP: Found device - ST: \(st), Location: \(location)")

        // We care about MediaRenderer and MediaServer devices; parse others
        // anyway and let the description decide whether they are usable.
        if !st.contains("MediaRenderer") && !st.contains("MediaServer") {
            coreLog("SSDP: Device ST not a renderer/server (\(st)), parsing anyway")
        }

        // Extract UUID from USN
        let uuid = extractUUID(from: usn)

        guard discoveredDevices[uuid] == nil, pendingRequests[uuid] == nil, locationURL.isHTTP else { return }
        let token = UUID()
        let sessionGeneration = generation
        pendingRequests[uuid] = token
        let fetch = fetchDescription
        fetchTasks[uuid] = Task { [weak self] in
            let device: UPnPDevice?
            do {
                let data = try await fetch(locationURL)
                try Task.checkCancellation()
                device = await DeviceDescriptionParser().parse(data: data, location: locationURL, uuid: uuid)
            } catch {
                device = nil
            }
            self?.queue.async { [weak self] in
                guard let self = self, self.generation == sessionGeneration,
                      self.pendingRequests[uuid] == token else { return }
                self.pendingRequests.removeValue(forKey: uuid)
                self.fetchTasks.removeValue(forKey: uuid)
                guard let device = device,
                      device.avTransportURL != nil || device.contentDirectoryURL != nil else { return }
                self.discoveredDevices[uuid] = device
                DispatchQueue.main.async { [weak self] in
                    guard let self = self,
                          self.withQueue({ self.generation == sessionGeneration && self.discoveredDevices[uuid] != nil }) else { return }
                    self.delegate?.ssdpDiscovery(self, didDiscoverDevice: device)
                }
            }
        }
    }

    /// Handles NOTIFY messages: ssdp:byebye drops the device and informs the
    /// delegate. ssdp:alive is ignored — M-SEARCH already finds devices.
    private func handleNotify(_ lines: [String]) {
        var nts: String?
        var usn: String?
        for line in lines {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let key = parts[0].trimmingCharacters(in: .whitespaces).uppercased()
            let value = parts[1].trimmingCharacters(in: .whitespaces)
            if key == "NTS" { nts = value } else if key == "USN" { usn = value }
        }

        guard nts?.contains("byebye") == true, let usn = usn else { return }

        let uuid = extractUUID(from: usn)
        pendingRequests.removeValue(forKey: uuid)
        fetchTasks.removeValue(forKey: uuid)?.cancel()
        let removed = discoveredDevices.removeValue(forKey: uuid)
        guard let device = removed else { return }

        coreLog("SSDP: Device left the network: \(device.friendlyName)")
        let sessionGeneration = generation
        DispatchQueue.main.async { [weak self] in
            guard let self = self,
                  self.withQueue({ self.generation == sessionGeneration && self.discoveredDevices[uuid] == nil }) else { return }
            self.delegate?.ssdpDiscovery(self, didRemoveDevice: device)
        }
    }

    /// Extracts UUID from USN
    private func extractUUID(from usn: String) -> String {
        if usn.hasPrefix("uuid:") {
            let uuidPart = usn.dropFirst(5) // Remove "uuid:"
            if let colonIndex = uuidPart.firstIndex(of: ":") {
                return String(uuidPart[..<colonIndex])
            }
            return String(uuidPart)
        }
        return usn
    }

    public func getDiscoveredDevices() -> [UPnPDevice] {
        withQueue { Array(discoveredDevices.values) }
    }

    deinit { stopDiscovery() }
}
