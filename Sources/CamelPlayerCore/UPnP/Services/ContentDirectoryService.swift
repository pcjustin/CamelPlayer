import Foundation

public enum MediaBrowseError: LocalizedError {
    case serverHasNoContentDirectory

    public var errorDescription: String? { "This server cannot be browsed" }
}

/// UPnP ContentDirectory service for browsing a MediaServer (e.g. MinimServer).
public class ContentDirectoryService {
    private let controlURL: String
    private let serviceType: String
    private let soapClient: SOAPClient

    public enum BrowseFlag: String {
        case directChildren = "BrowseDirectChildren"
        case metadata = "BrowseMetadata"
    }

    public struct BrowseResult {
        public let objects: [MediaObject]
        public let numberReturned: Int
        public let totalMatches: Int
    }

    public init(controlURL: String, serviceType: String = "urn:schemas-upnp-org:service:ContentDirectory:1") {
        self.controlURL = controlURL
        self.serviceType = serviceType
        self.soapClient = SOAPClient()
    }

    /// Browses a container. The root container ID is "0".
    /// - Parameters:
    ///   - objectID: Container ID to list ("0" for root).
    ///   - flag: DirectChildren (list contents) or Metadata (the object itself).
    ///   - startingIndex/requestedCount: paging (0 count = server default / all).
    ///   - sortCriteria: e.g. "+dc:title" (empty = server default order).
    public func browse(
        objectID: String,
        flag: BrowseFlag = .directChildren,
        filter: String = "*",
        startingIndex: Int = 0,
        requestedCount: Int = 0,
        sortCriteria: String = ""
    ) async throws -> BrowseResult {
        let response = try await soapClient.call(
            controlURL: controlURL,
            action: "Browse",
            serviceType: serviceType,
            argumentOrder: ["ObjectID", "BrowseFlag", "Filter", "StartingIndex", "RequestedCount", "SortCriteria"],
            arguments: [
                "ObjectID": objectID,
                "BrowseFlag": flag.rawValue,
                "Filter": filter,
                "StartingIndex": String(startingIndex),
                "RequestedCount": String(requestedCount),
                "SortCriteria": sortCriteria
            ]
        )

        return try Self.parseResult(response)
    }

    /// Searches a container subtree (root "0" = whole library) with a UPnP
    /// SearchCriteria expression. Returns matching objects like browse().
    public func search(
        containerID: String = "0",
        searchCriteria: String,
        filter: String = "*",
        startingIndex: Int = 0,
        requestedCount: Int = 200,
        sortCriteria: String = ""
    ) async throws -> BrowseResult {
        let response = try await soapClient.call(
            controlURL: controlURL,
            action: "Search",
            serviceType: serviceType,
            argumentOrder: ["ContainerID", "SearchCriteria", "Filter", "StartingIndex", "RequestedCount", "SortCriteria"],
            arguments: [
                "ContainerID": containerID,
                "SearchCriteria": searchCriteria,
                "Filter": filter,
                "StartingIndex": String(startingIndex),
                "RequestedCount": String(requestedCount),
                "SortCriteria": sortCriteria
            ]
        )
        return try Self.parseResult(response)
    }

    static func parseResult(_ response: [String: String]) throws -> BrowseResult {
        // SOAPClient has already entity-decoded the Result element.
        guard let didl = response["Result"] else { throw SOAPError.invalidResponse }
        let objects = didl.isEmpty ? [] : try DIDLParser().parseValidated(didl)
        func count(_ key: String) throws -> Int {
            guard let value = response[key] else { return objects.count }
            guard let count = Int(value), count >= 0 else { throw SOAPError.invalidResponse }
            return count
        }
        let numberReturned = try count("NumberReturned")
        let totalMatches = try count("TotalMatches")
        guard numberReturned == objects.count else { throw SOAPError.invalidResponse }
        return BrowseResult(objects: objects, numberReturned: numberReturned, totalMatches: totalMatches)
    }

    static func textSearchCriteria(_ query: String) -> String {
        let escaped = query.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        // No class filter: some servers reject class OR-expressions.
        return ["dc:title", "upnp:artist", "upnp:album"]
            .map { "\($0) contains \"\(escaped)\"" }.joined(separator: " or ")
    }

    /// Returns the sort fields the server supports (e.g. "dc:title", "upnp:artist").
    /// An empty array means the server reports no sortable fields.
    public func getSortCapabilities() async throws -> [String] {
        let response = try await soapClient.call(
            controlURL: controlURL,
            action: "GetSortCapabilities",
            serviceType: serviceType
        )
        let caps = response["SortCaps"] ?? ""
        return caps
            .split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
    }
}
