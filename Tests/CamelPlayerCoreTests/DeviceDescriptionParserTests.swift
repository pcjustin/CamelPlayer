import XCTest
@testable import CamelPlayerCore

final class DeviceDescriptionParserTests: XCTestCase {
    private let location = URL(string: "http://192.168.1.50:8080/desc.xml")!

    private func xml(
        friendlyName: String = "Living Room",
        manufacturer: String = "Acme",
        modelName: String = "SpeakerX",
        avControlURL: String = "/AVTransport/control",
        renderingControlURL: String = "RenderingControl/control"
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
                <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
                <controlURL>\(renderingControlURL)</controlURL>
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

    func testResolvesAbsolutePathControlURL() async {
        // "/AVTransport/control" -> scheme://host:port + path
        let device = await DeviceDescriptionParser().parse(data: xml(), location: location, uuid: "u")
        XCTAssertEqual(device?.avTransportURL, "http://192.168.1.50:8080/AVTransport/control")
    }

    func testResolvesRelativeControlURL() async {
        // "RenderingControl/control" -> appended to base directory
        let device = await DeviceDescriptionParser().parse(data: xml(), location: location, uuid: "u")
        XCTAssertEqual(device?.renderingControlURL, "http://192.168.1.50:8080/RenderingControl/control")
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
}
