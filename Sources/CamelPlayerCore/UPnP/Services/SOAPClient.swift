import Foundation

/// Errors that can occur during SOAP communication
public enum SOAPError: Error {
    case invalidURL
    case networkError(Error)
    case invalidResponse
    case soapFault(String)
    case parsingError(String)
}

/// A client for making SOAP requests to UPnP services
public class SOAPClient {
    private let session: URLSession

    public init(session: URLSession = .shared) {
        self.session = session
    }

    /// Builds a SOAP request XML envelope
    /// - Parameters:
    ///   - action: The SOAP action name (e.g., "Play", "SetAVTransportURI")
    ///   - serviceType: The UPnP service type URN
    ///   - arguments: Dictionary of argument names and values
    /// - Returns: The SOAP XML string
    public func buildSOAPRequest(action: String, serviceType: String, arguments: [String: String] = [:]) -> String {
        var argumentsXML = ""
        for (key, value) in arguments {
            let escapedValue = value
                .replacingOccurrences(of: "&", with: "&amp;")
                .replacingOccurrences(of: "<", with: "&lt;")
                .replacingOccurrences(of: ">", with: "&gt;")
                .replacingOccurrences(of: "\"", with: "&quot;")
            argumentsXML += "<\(key)>\(escapedValue)</\(key)>"
        }

        return """
        <?xml version="1.0" encoding="utf-8"?>
        <s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
            <s:Body>
                <u:\(action) xmlns:u="\(serviceType)">
                    \(argumentsXML)
                </u:\(action)>
            </s:Body>
        </s:Envelope>
        """
    }

    /// Calls a SOAP action on a UPnP service
    /// - Parameters:
    ///   - controlURL: The control URL of the service
    ///   - action: The action name
    ///   - serviceType: The service type URN
    ///   - arguments: Dictionary of argument names and values
    /// - Returns: Dictionary of response values
    public func call(
        controlURL: String,
        action: String,
        serviceType: String,
        arguments: [String: String] = [:]
    ) async throws -> [String: String] {
        guard let url = URL(string: controlURL) else {
            throw SOAPError.invalidURL
        }

        let soapBody = buildSOAPRequest(action: action, serviceType: serviceType, arguments: arguments)

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("text/xml; charset=\"utf-8\"", forHTTPHeaderField: "Content-Type")
        request.setValue("\"\(serviceType)#\(action)\"", forHTTPHeaderField: "SOAPACTION")
        request.httpBody = soapBody.data(using: .utf8)

        let (data, response) = try await session.data(for: request)

        guard let httpResponse = response as? HTTPURLResponse else {
            throw SOAPError.invalidResponse
        }

        guard httpResponse.statusCode == 200 else {
            // Try to parse SOAP fault
            if let faultMessage = try? parseSOAPFault(from: data) {
                throw SOAPError.soapFault(faultMessage)
            }
            throw SOAPError.invalidResponse
        }

        return try parseSOAPResponse(data: data, action: action)
    }

    /// Parses a SOAP response and extracts the result values
    /// - Parameters:
    ///   - data: The response data
    ///   - action: The action name (to find the response element)
    /// - Returns: Dictionary of response values
    private func parseSOAPResponse(data: Data, action: String) throws -> [String: String] {
        let parser = SOAPResponseParser(action: action)

        guard let xmlParser = XMLParser(data: data) as XMLParser? else {
            throw SOAPError.parsingError("Failed to create XML parser")
        }

        xmlParser.delegate = parser

        guard xmlParser.parse() else {
            if let error = xmlParser.parserError {
                throw SOAPError.parsingError(error.localizedDescription)
            }
            throw SOAPError.parsingError("Unknown parsing error")
        }

        return parser.results
    }

    /// Parses a SOAP fault message
    private func parseSOAPFault(from data: Data) throws -> String? {
        let parser = SOAPFaultParser()

        guard let xmlParser = XMLParser(data: data) as XMLParser? else {
            return nil
        }

        xmlParser.delegate = parser
        xmlParser.parse()

        return parser.faultMessage
    }
}

// MARK: - SOAP Response Parser

private class SOAPResponseParser: NSObject, XMLParserDelegate {
    let action: String
    var results: [String: String] = [:]

    private var currentElement = ""
    private var currentValue = ""
    private var insideResponseElement = false

    init(action: String) {
        self.action = action
    }

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes attributeDict: [String : String] = [:]) {
        if elementName == "\(action)Response" || elementName.hasSuffix(":\(action)Response") {
            insideResponseElement = true
        } else if insideResponseElement {
            currentElement = elementName
            // Handle namespaced elements (e.g., "u:Volume")
            if let colonIndex = elementName.lastIndex(of: ":") {
                currentElement = String(elementName[elementName.index(after: colonIndex)...])
            }
            currentValue = ""
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        if insideResponseElement && !currentElement.isEmpty {
            currentValue += string
        }
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        if elementName == "\(action)Response" || elementName.hasSuffix(":\(action)Response") {
            insideResponseElement = false
        } else if insideResponseElement && !currentElement.isEmpty {
            results[currentElement] = currentValue.trimmingCharacters(in: .whitespacesAndNewlines)
            currentElement = ""
            currentValue = ""
        }
    }
}

// MARK: - SOAP Fault Parser

private class SOAPFaultParser: NSObject, XMLParserDelegate {
    var faultMessage: String?

    private var currentElement = ""
    private var currentValue = ""
    private var insideFault = false

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes attributeDict: [String : String] = [:]) {
        if elementName == "Fault" || elementName.hasSuffix(":Fault") {
            insideFault = true
        } else if insideFault {
            currentElement = elementName
            currentValue = ""
        }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        if insideFault {
            currentValue += string
        }
    }

    func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
        if elementName == "Fault" || elementName.hasSuffix(":Fault") {
            insideFault = false
        } else if insideFault && (elementName == "faultstring" || elementName.hasSuffix(":faultstring")) {
            faultMessage = currentValue.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        currentElement = ""
        currentValue = ""
    }
}
