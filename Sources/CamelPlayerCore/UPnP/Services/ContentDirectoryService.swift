import Foundation

public enum MediaBrowseError: Error {
    case serverHasNoContentDirectory
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
            arguments: [
                "ObjectID": objectID,
                "BrowseFlag": flag.rawValue,
                "Filter": filter,
                "StartingIndex": String(startingIndex),
                "RequestedCount": String(requestedCount),
                "SortCriteria": sortCriteria
            ]
        )

        // SOAPClient already entity-decodes the Result element, so it is plain
        // DIDL-Lite XML at this point.
        let didl = response["Result"] ?? ""
        let objects = DIDLParser().parse(didl)
        let numberReturned = Int(response["NumberReturned"] ?? "") ?? objects.count
        let totalMatches = Int(response["TotalMatches"] ?? "") ?? objects.count

        return BrowseResult(objects: objects, numberReturned: numberReturned, totalMatches: totalMatches)
    }
}
