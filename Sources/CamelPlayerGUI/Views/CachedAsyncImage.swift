import SwiftUI

/// Small in-memory cache for remote album art so scrolling the browser does
/// not re-download the same thumbnails.
private enum ImageCache {
    static let shared = NSCache<NSURL, NSImage>()
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
        if let cached = ImageCache.shared.object(forKey: url as NSURL) {
            image = cached
            return
        }
        guard let (data, _) = try? await URLSession.shared.data(from: url),
              let loaded = NSImage(data: data) else { return }
        ImageCache.shared.setObject(loaded, forKey: url as NSURL)
        image = loaded
    }
}
