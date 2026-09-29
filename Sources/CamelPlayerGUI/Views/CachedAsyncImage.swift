import SwiftUI
import CryptoKit

/// Two-level cache for remote album art: an in-memory NSCache plus an on-disk
/// cache so covers survive relaunches and aren't re-downloaded.
enum ImageCache {
    static let memory = NSCache<NSURL, NSImage>()

    static let directory: URL = {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CamelPlayer/Covers", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }()

    static func fileURL(for url: URL) -> URL {
        let digest = SHA256.hash(data: Data(url.absoluteString.utf8))
        let name = digest.map { String(format: "%02x", $0) }.joined()
        return directory.appendingPathComponent(name)
    }
}

/// Drop-in replacement for AsyncImage that caches loaded images by URL.
struct CachedAsyncImage<Placeholder: View>: View {
    let url: URL?
    @ViewBuilder let placeholder: () -> Placeholder

    @State private var image: NSImage?

    var body: some View {
        Group {
            if let image = image {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
            } else {
                placeholder()
            }
        }
        .task(id: url) { await load() }
    }

    private func load() async {
        image = nil
        guard let url = url else { return }
        let key = url as NSURL
        if let cached = ImageCache.memory.object(forKey: key) {
            image = cached
            return
        }
        // Disk read + network off the main actor; return Sendable Data.
        let data: Data? = await Task.detached(priority: .utility) {
            let file = ImageCache.fileURL(for: url)
            if let onDisk = try? Data(contentsOf: file), NSImage(data: onDisk) != nil { return onDisk }
            if url.isFileURL { return try? Data(contentsOf: url) }
            guard let (downloaded, response) = try? await URLSession.shared.data(from: url),
                  let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode),
                  NSImage(data: downloaded) != nil else { return nil }
            try? downloaded.write(to: file, options: .atomic)
            return downloaded
        }.value
        guard !Task.isCancelled, let data = data, let loaded = NSImage(data: data) else { return }
        ImageCache.memory.setObject(loaded, forKey: key)
        image = loaded
    }
}
