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

/// SSDP (Simple Service Discovery Protocol) implementation for UPnP device discovery
public class SSDPDiscovery: @unchecked Sendable {
    private static let multicastGroup = "239.255.255.250"
    private static let multicastPort: UInt16 = 1900
    private static let searchTargets = [
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:device:MediaServer:1"
    ]

    public weak var delegate: SSDPDiscoveryDelegate?

    private var socketFD: Int32 = -1
    private var isDiscovering = false
    /// Guarded by devicesLock: read on the listener thread, written from
    /// concurrent description-fetch tasks.
    private var discoveredDevices: [String: UPnPDevice] = [:]
    private let devicesLock = NSLock()
    private var listenerThread: Thread?
    private var searchTimer: Timer?

    public init() {}

    /// Starts SSDP discovery
    public func startDiscovery() {
        guard !isDiscovering else { return }

        isDiscovering = true
        coreLog("SSDP: Starting discovery...")

        // Create UDP socket
        if !createSocket() {
            coreLog("SSDP: Failed to create socket")
            isDiscovering = false
            return
        }

        // Start listener thread
        startListenerThread()

        // Send an initial burst of M-SEARCH packets right away. SSDP runs over
        // UDP and may drop packets, so a few quick repeats speed up and harden
        // initial discovery instead of waiting for the 30s periodic timer.
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            for i in 0..<3 {
                guard self?.isDiscovering == true else { return }
                self?.sendMSearch()
                if i < 2 { Thread.sleep(forTimeInterval: 0.1) }
            }
        }

        // Send periodic M-SEARCH messages
        DispatchQueue.main.async { [weak self] in
            self?.searchTimer = Timer.scheduledTimer(withTimeInterval: 30.0, repeats: true) { [weak self] _ in
                self?.sendMSearch()
            }
        }
    }

    /// Stops SSDP discovery
    public func stopDiscovery() {
        guard isDiscovering else { return }

        coreLog("SSDP: Stopping discovery...")
        isDiscovering = false

        searchTimer?.invalidate()
        searchTimer = nil

        closeSocket()
        devicesLock.lock()
        discoveredDevices.removeAll()
        devicesLock.unlock()
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

        // Set socket timeout
        var timeout = timeval()
        timeout.tv_sec = 1
        timeout.tv_usec = 0
        if setsockopt(socketFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) < 0 {
            coreLog("SSDP: Failed to set socket timeout: \(String(cString: strerror(errno)))")
            // Continue anyway - timeout is optional
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

    /// Starts the listener thread
    private func startListenerThread() {
        listenerThread = Thread { [weak self] in
            self?.listenForResponses()
        }
        listenerThread?.start()
    }

    /// Listens for SSDP responses
    private func listenForResponses() {
        coreLog("SSDP: Listener thread started")

        let bufferSize = 8192
        var buffer = [UInt8](repeating: 0, count: bufferSize)
        var addr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        while isDiscovering {
            let bytesRead = withUnsafeMutablePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    recvfrom(socketFD, &buffer, bufferSize, 0, $0, &addrLen)
                }
            }

            if bytesRead > 0 {
                if let message = String(bytes: buffer[0..<bytesRead], encoding: .utf8) {
                    // Get source IP for debugging
                    let sourceIP = String(cString: inet_ntoa(addr.sin_addr))
                    coreLog("SSDP: Received \(bytesRead) bytes from \(sourceIP)")
                    parseResponse(message)
                }
            } else if bytesRead < 0 {
                let error = errno
                if error != EAGAIN && error != EWOULDBLOCK {
                    coreLog("SSDP: Receive error: \(String(cString: strerror(error)))")
                }
            }
            // No sleep needed: recvfrom blocks up to the 1s SO_RCVTIMEO, so the
            // loop never busy-spins, and responses are handled with no added delay.
        }

        coreLog("SSDP: Listener thread stopped")
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

        // Avoid duplicates
        devicesLock.lock()
        let alreadyDiscovered = discoveredDevices[uuid] != nil
        devicesLock.unlock()
        guard !alreadyDiscovered else {
            coreLog("SSDP: Device already discovered: \(uuid)")
            return
        }

        // Fetch and parse device description
        Task {
            await fetchDeviceDescription(uuid: uuid, location: locationURL)
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
        devicesLock.lock()
        let removed = discoveredDevices.removeValue(forKey: uuid)
        devicesLock.unlock()
        guard let device = removed else { return }

        coreLog("SSDP: Device left the network: \(device.friendlyName)")
        DispatchQueue.main.async {
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

    /// Fetches and parses device description XML
    private func fetchDeviceDescription(uuid: String, location: URL) async {
        coreLog("SSDP: Fetching device description from \(location)")
        do {
            let (data, _) = try await URLSession.shared.data(from: location)

            // Fresh parser per fetch: concurrent fetches sharing one stateful
            // parser corrupt each other's fields.
            let parser = DeviceDescriptionParser()

            if let device = await parser.parse(data: data, location: location, uuid: uuid) {
                coreLog("SSDP: Parsed device: \(device.friendlyName)")
                coreLog("  Manufacturer: \(device.manufacturer)")
                coreLog("  Model: \(device.modelName)")
                coreLog("  AVTransport URL: \(device.avTransportURL ?? "nil")")
                coreLog("  RenderingControl URL: \(device.renderingControlURL ?? "nil")")

                // Keep renderers (AVTransport) and servers (ContentDirectory).
                if device.avTransportURL != nil || device.contentDirectoryURL != nil {
                    coreLog("SSDP: Adding \(device.deviceType) device to list")
                    storeDevice(device, uuid: uuid)
                    DispatchQueue.main.async {
                        self.delegate?.ssdpDiscovery(self, didDiscoverDevice: device)
                    }
                } else {
                    coreLog("SSDP: Device has no usable service, ignoring")
                }
            } else {
                coreLog("SSDP: Failed to parse device description")
            }
        } catch {
            coreLog("SSDP: Failed to fetch device description from \(location): \(error)")
        }
    }

    /// Records a parsed device (sync so the lock is usable from async callers).
    private func storeDevice(_ device: UPnPDevice, uuid: String) {
        devicesLock.lock()
        discoveredDevices[uuid] = device
        devicesLock.unlock()
    }

    /// Gets all discovered devices
    public func getDiscoveredDevices() -> [UPnPDevice] {
        devicesLock.lock()
        defer { devicesLock.unlock() }
        return Array(discoveredDevices.values)
    }

    deinit {
        stopDiscovery()
    }
}
