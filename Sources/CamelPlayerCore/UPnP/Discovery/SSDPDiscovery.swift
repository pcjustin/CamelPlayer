import Foundation
import Darwin

/// Protocol for SSDP discovery delegate
public protocol SSDPDiscoveryDelegate: AnyObject {
    func ssdpDiscovery(_ discovery: SSDPDiscovery, didDiscoverDevice device: UPnPDevice)
    func ssdpDiscovery(_ discovery: SSDPDiscovery, didRemoveDevice device: UPnPDevice)
}

/// SSDP (Simple Service Discovery Protocol) implementation for UPnP device discovery
public class SSDPDiscovery {
    private static let multicastGroup = "239.255.255.250"
    private static let multicastPort: UInt16 = 1900
    private static let searchTarget = "urn:schemas-upnp-org:device:MediaRenderer:1"

    public weak var delegate: SSDPDiscoveryDelegate?

    private var socketFD: Int32 = -1
    private var isDiscovering = false
    private var discoveredDevices: [String: UPnPDevice] = [:]
    private var deviceParser: DeviceDescriptionParser?
    private var listenerThread: Thread?
    private var searchTimer: Timer?

    public init() {
        self.deviceParser = DeviceDescriptionParser()
    }

    /// Starts SSDP discovery
    public func startDiscovery() {
        guard !isDiscovering else { return }

        isDiscovering = true
        print("SSDP: Starting discovery...")

        // Create UDP socket
        if !createSocket() {
            print("SSDP: Failed to create socket")
            isDiscovering = false
            return
        }

        // Start listener thread
        startListenerThread()

        // Send initial M-SEARCH
        DispatchQueue.global(qos: .background).asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.sendMSearch()
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

        print("SSDP: Stopping discovery...")
        isDiscovering = false

        searchTimer?.invalidate()
        searchTimer = nil

        closeSocket()
        discoveredDevices.removeAll()
    }

    /// Creates a UDP socket for SSDP
    private func createSocket() -> Bool {
        // Create UDP socket
        socketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socketFD >= 0 else {
            print("SSDP: Failed to create socket: \(String(cString: strerror(errno)))")
            return false
        }

        // Allow socket reuse
        var reuseAddr: Int32 = 1
        guard setsockopt(socketFD, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, socklen_t(MemoryLayout<Int32>.size)) >= 0 else {
            print("SSDP: Failed to set SO_REUSEADDR: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        var reusePort: Int32 = 1
        guard setsockopt(socketFD, SOL_SOCKET, SO_REUSEPORT, &reusePort, socklen_t(MemoryLayout<Int32>.size)) >= 0 else {
            print("SSDP: Failed to set SO_REUSEPORT: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        // Bind to SSDP port
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = Self.multicastPort.bigEndian
        addr.sin_addr.s_addr = INADDR_ANY.bigEndian

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(socketFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindResult >= 0 else {
            print("SSDP: Failed to bind socket: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        // Join multicast group
        var mreq = ip_mreq()
        mreq.imr_multiaddr.s_addr = inet_addr(Self.multicastGroup)
        mreq.imr_interface.s_addr = INADDR_ANY.bigEndian

        guard setsockopt(socketFD, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, socklen_t(MemoryLayout<ip_mreq>.size)) >= 0 else {
            print("SSDP: Failed to join multicast group: \(String(cString: strerror(errno)))")
            closeSocket()
            return false
        }

        // Set socket timeout
        var timeout = timeval()
        timeout.tv_sec = 1
        timeout.tv_usec = 0
        if setsockopt(socketFD, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) < 0 {
            print("SSDP: Failed to set socket timeout: \(String(cString: strerror(errno)))")
            // Continue anyway - timeout is optional
        }

        print("SSDP: Socket created and bound to port \(Self.multicastPort)")
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
        print("SSDP: Listener thread started")

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
                    print("SSDP: Received \(bytesRead) bytes from \(sourceIP)")
                    parseResponse(message)
                }
            } else if bytesRead < 0 {
                let error = errno
                if error != EAGAIN && error != EWOULDBLOCK {
                    print("SSDP: Receive error: \(String(cString: strerror(error)))")
                }
            }

            // Small delay to avoid busy loop
            Thread.sleep(forTimeInterval: 0.1)
        }

        print("SSDP: Listener thread stopped")
    }

    /// Sends M-SEARCH multicast request
    private func sendMSearch() {
        let message = """
        M-SEARCH * HTTP/1.1\r
        HOST: \(Self.multicastGroup):\(Self.multicastPort)\r
        MAN: "ssdp:discover"\r
        MX: 3\r
        ST: \(Self.searchTarget)\r
        \r

        """

        print("SSDP: Sending M-SEARCH for MediaRenderer devices...")

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
            print("SSDP: M-SEARCH sent successfully (\(bytesSent) bytes)")
        } else {
            print("SSDP: Failed to send M-SEARCH: \(String(cString: strerror(errno)))")
        }
    }

    /// Parses SSDP response
    private func parseResponse(_ response: String) {
        let lines = response.components(separatedBy: "\r\n")

        // Check if it's a response (not a NOTIFY)
        guard let firstLine = lines.first else { return }

        if firstLine.hasPrefix("NOTIFY") {
            print("SSDP: Received NOTIFY (ignored)")
            return
        }

        guard firstLine.hasPrefix("HTTP/1.1 200 OK") else {
            print("SSDP: Received unknown message type: \(firstLine)")
            return
        }

        print("SSDP: Received HTTP 200 OK response")

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
            print("SSDP: Response missing required fields")
            print("  ST: \(st ?? "nil")")
            print("  Location: \(location ?? "nil")")
            print("  USN: \(usn ?? "nil")")
            return
        }

        print("SSDP: Found device - ST: \(st), Location: \(location)")

        // Check if it's a MediaRenderer (or accept all for debugging)
        let isMediaRenderer = st.contains("MediaRenderer")
        if !isMediaRenderer {
            print("SSDP: Device is not a MediaRenderer (ST: \(st)), but will try to parse anyway")
            // Continue anyway for debugging
        }

        // Extract UUID from USN
        let uuid = extractUUID(from: usn)

        // Avoid duplicates
        guard discoveredDevices[uuid] == nil else {
            print("SSDP: Device already discovered: \(uuid)")
            return
        }

        // Fetch and parse device description
        Task {
            await fetchDeviceDescription(uuid: uuid, location: locationURL)
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
        print("SSDP: Fetching device description from \(location)")
        do {
            let (data, _) = try await URLSession.shared.data(from: location)

            guard let parser = deviceParser else { return }

            if let device = await parser.parse(data: data, location: location, uuid: uuid) {
                print("SSDP: Parsed device: \(device.friendlyName)")
                print("  Manufacturer: \(device.manufacturer)")
                print("  Model: \(device.modelName)")
                print("  AVTransport URL: \(device.avTransportURL ?? "nil")")
                print("  RenderingControl URL: \(device.renderingControlURL ?? "nil")")

                // Only add devices that have AVTransport service
                if device.avTransportURL != nil {
                    print("SSDP: Device has AVTransport service, adding to list")
                    discoveredDevices[uuid] = device
                    DispatchQueue.main.async {
                        self.delegate?.ssdpDiscovery(self, didDiscoverDevice: device)
                    }
                } else {
                    print("SSDP: Device lacks AVTransport service, ignoring")
                }
            } else {
                print("SSDP: Failed to parse device description")
            }
        } catch {
            print("SSDP: Failed to fetch device description from \(location): \(error)")
        }
    }

    /// Gets all discovered devices
    public func getDiscoveredDevices() -> [UPnPDevice] {
        return Array(discoveredDevices.values)
    }

    deinit {
        stopDiscovery()
    }
}
