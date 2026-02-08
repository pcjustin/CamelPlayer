import Foundation

public struct PlaylistItem {
    public let url: URL
    public let title: String

    public init(url: URL) {
        self.url = url
        self.title = url.deletingPathExtension().lastPathComponent
    }

    public init(url: URL, title: String) {
        self.url = url
        self.title = title
    }
}
