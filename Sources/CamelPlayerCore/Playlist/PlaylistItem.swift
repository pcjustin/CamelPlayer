import Foundation

public struct PlaylistItem: Identifiable {
    public let id = UUID()
    public let url: URL
    public let title: String
    /// Pre-built DIDL-Lite metadata for UPnP renderers (nil for local files).
    public let metadata: String?

    public init(url: URL) {
        self.url = url
        self.title = url.deletingPathExtension().lastPathComponent
        self.metadata = nil
    }

    public init(url: URL, title: String, metadata: String? = nil) {
        self.url = url
        self.title = title
        self.metadata = metadata
    }
}
