import Foundation

public enum PlaybackMode {
    case sequential
    case loop
    case loopOne
    case shuffle
}

public class Playlist {
    private var items: [PlaylistItem] = []
    private var currentIndex: Int = -1
    public var mode: PlaybackMode = .sequential

    public var count: Int {
        items.count
    }

    public var currentItem: PlaylistItem? {
        guard currentIndex >= 0 && currentIndex < items.count else {
            return nil
        }
        return items[currentIndex]
    }

    public var currentPosition: Int {
        currentIndex
    }

    public init() {}

    public func add(_ item: PlaylistItem) {
        items.append(item)
        if currentIndex == -1 {
            currentIndex = 0
        }
    }

    public func add(url: URL) {
        add(PlaylistItem(url: url))
    }

    public func addAll(urls: [URL]) {
        for url in urls {
            add(url: url)
        }
    }

    public func remove(at index: Int) {
        guard index >= 0 && index < items.count else { return }

        items.remove(at: index)

        if currentIndex >= items.count {
            currentIndex = items.count - 1
        }

        if items.isEmpty {
            currentIndex = -1
        }
    }

    public func clear() {
        items.removeAll()
        currentIndex = -1
    }

    public func next() -> PlaylistItem? {
        guard !items.isEmpty else { return nil }

        switch mode {
        case .sequential:
            if currentIndex + 1 < items.count {
                currentIndex += 1
                return items[currentIndex]
            }
            return nil

        case .loop:
            currentIndex = (currentIndex + 1) % items.count
            return items[currentIndex]

        case .loopOne:
            return items[currentIndex]

        case .shuffle:
            currentIndex = Int.random(in: 0..<items.count)
            return items[currentIndex]
        }
    }

    public func previous() -> PlaylistItem? {
        guard !items.isEmpty else { return nil }

        switch mode {
        case .sequential:
            if currentIndex > 0 {
                currentIndex -= 1
                return items[currentIndex]
            }
            return nil

        case .loop:
            currentIndex = (currentIndex - 1 + items.count) % items.count
            return items[currentIndex]

        case .loopOne:
            return items[currentIndex]

        case .shuffle:
            currentIndex = Int.random(in: 0..<items.count)
            return items[currentIndex]
        }
    }

    public func jumpTo(index: Int) -> PlaylistItem? {
        guard index >= 0 && index < items.count else { return nil }
        currentIndex = index
        return items[currentIndex]
    }

    public func allItems() -> [PlaylistItem] {
        items
    }
}
