import XCTest
@testable import CamelPlayerCore

final class SOAPClientTests: XCTestCase {
    func testArgumentOrderAndEscaping() {
        let xml = SOAPClient().buildSOAPRequest(action: "SetAVTransportURI", serviceType: "urn:test",
            argumentOrder: ["InstanceID", "CurrentURI", "CurrentURIMetaData"],
            arguments: ["CurrentURI": "http://nas/track?a=1&b=2", "CurrentURIMetaData": "<title>\"Song\"</title>", "InstanceID": "0"])
        let instance = xml.range(of: "<InstanceID>")!
        let uri = xml.range(of: "<CurrentURI>")!
        let metadata = xml.range(of: "<CurrentURIMetaData>")!
        XCTAssertLessThan(instance.lowerBound, uri.lowerBound)
        XCTAssertLessThan(uri.lowerBound, metadata.lowerBound)
        XCTAssertTrue(xml.contains("a=1&amp;b=2"))
        XCTAssertTrue(xml.contains("&lt;title&gt;&quot;Song&quot;&lt;/title&gt;"))
    }

    func testResponseRequiresMatchingAction() throws {
        let client = SOAPClient()
        XCTAssertThrowsError(try client.parseSOAPResponse(data: Data("<html>OK</html>".utf8), action: "Play"))
        XCTAssertThrowsError(try client.parseSOAPResponse(data: Data("<StopResponse/>".utf8), action: "Play"))
        XCTAssertTrue(try client.parseSOAPResponse(data: Data("<PlayResponse/>".utf8), action: "Play").isEmpty)
    }

    func testCDATAResultAndSOAPFault() throws {
        let client = SOAPClient()
        let data = Data("<BrowseResponse><Result><![CDATA[<DIDL-Lite/>]]></Result><NumberReturned>0</NumberReturned></BrowseResponse>".utf8)
        let result = try client.parseSOAPResponse(data: data, action: "Browse")
        XCTAssertEqual(result["Result"], "<DIDL-Lite/>")
        let fault = Data("<Fault><faultstring>Invalid action</faultstring></Fault>".utf8)
        XCTAssertThrowsError(try client.parseSOAPResponse(data: fault, action: "Play")) { error in
            guard case SOAPError.soapFault("Invalid action") = error else {
                return XCTFail("Expected SOAP fault, got \(error)")
            }
        }
    }
}
