import XCTest
@testable import CamelPlayerCore

final class DeviceDescriptionParserTests: XCTestCase {
    private let location = URL(string: "http://192.168.1.50:8080/desc.xml")!

    private func xml(
        friendlyName: String = "Living Room",
        manufacturer: String = "Acme",
        modelName: String = "SpeakerX",
        avControlURL: String = "/AVTransport/control",
        contentDirectoryURL: String = "ContentDirectory/control"
    ) -> Data {
        """
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <device>
            <friendlyName>\(friendlyName)</friendlyName>
            <manufacturer>\(manufacturer)</manufacturer>
            <modelName>\(modelName)</modelName>
            <serviceList>
              <service>
                <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
                <controlURL>\(avControlURL)</controlURL>
              </service>
              <service>
                <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>
                <controlURL>\(contentDirectoryURL)</controlURL>
              </service>
            </serviceList>
          </device>
        </root>
        """.data(using: .utf8)!
    }

    func testParsesBasicFields() async {
        let device = await DeviceDescriptionParser().parse(data: xml(), location: location, uuid: "uuid-1")
        XCTAssertEqual(device?.id, "uuid-1")
        XCTAssertEqual(device?.friendlyName, "Living Room")
        XCTAssertEqual(device?.manufacturer, "Acme")
        XCTAssertEqual(device?.modelName, "SpeakerX")
        XCTAssertEqual(device?.location, location)
    }

    func testCDATADeviceFieldsAreDecoded() async {
        let data = xml(friendlyName: "<![CDATA[Living & Dining]]>",
                       avControlURL: "<![CDATA[/control?a=1&b=2]]>")
        let device = await DeviceDescriptionParser().parse(data: data, location: location, uuid: "u")
        XCTAssertEqual(device?.friendlyName, "Living & Dining")
        XCTAssertEqual(device?.avTransportURL, "http://192.168.1.50:8080/control?a=1&b=2")
    }

    func testResolvesAbsolutePathControlURL() async {
        // "/AVTransport/control" -> scheme://host:port + path
        let device = await DeviceDescriptionParser().parse(data: xml(), location: location, uuid: "u")
        XCTAssertEqual(device?.avTransportURL, "http://192.168.1.50:8080/AVTransport/control")
    }

    func testResolvesRelativeControlURL() async {
        // "ContentDirectory/control" -> appended to base directory
        let device = await DeviceDescriptionParser().parse(data: xml(), location: location, uuid: "u")
        XCTAssertEqual(device?.contentDirectoryURL, "http://192.168.1.50:8080/ContentDirectory/control")
    }

    func testKeepsAbsoluteHTTPControlURL() async {
        let data = xml(avControlURL: "http://10.0.0.9/avt")
        let device = await DeviceDescriptionParser().parse(data: data, location: location, uuid: "u")
        XCTAssertEqual(device?.avTransportURL, "http://10.0.0.9/avt")
    }

    func testMissingFriendlyNameReturnsNil() async {
        let data = xml(friendlyName: "")
        let device = await DeviceDescriptionParser().parse(data: data, location: location, uuid: "u")
        XCTAssertNil(device)
    }

    func testEmptyManufacturerAndModelDefaultToUnknown() async {
        let data = xml(manufacturer: "", modelName: "")
        let device = await DeviceDescriptionParser().parse(data: data, location: location, uuid: "u")
        XCTAssertEqual(device?.manufacturer, "Unknown")
        XCTAssertEqual(device?.modelName, "Unknown")
    }

    func testMalformedXMLReturnsNil() async {
        let data = "<root><device><friendlyName>oops".data(using: .utf8)!
        let device = await DeviceDescriptionParser().parse(data: data, location: location, uuid: "u")
        XCTAssertNil(device)
    }

    func testURLBaseAndRelativeQueryAreResolvedAsURLs() async {
        let data = String(decoding: xml(avControlURL: "../control?service=AVTransport"), as: UTF8.self)
            .replacingOccurrences(of: "<device>", with: "<URLBase>http://10.0.0.2:9000/base/</URLBase><device>")
        let device = await DeviceDescriptionParser().parse(data: Data(data.utf8), location: location, uuid: "u")
        XCTAssertEqual(device?.avTransportURL, "http://10.0.0.2:9000/control?service=AVTransport")
        XCTAssertEqual(device?.contentDirectoryURL, "http://10.0.0.2:9000/base/ContentDirectory/control")
    }

    func testIPv6LocationAndQueryInControlURL() async {
        let location = URL(string: "http://[::1]:8080/devices/desc.xml")!
        let device = await DeviceDescriptionParser().parse(
            data: xml(avControlURL: "/control?action=1"), location: location, uuid: "u")
        XCTAssertEqual(device?.avTransportURL, "http://[::1]:8080/control?action=1")
    }
}
