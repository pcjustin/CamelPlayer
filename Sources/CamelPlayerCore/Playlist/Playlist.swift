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
    private let lock = NSLock()
    private var _mode: PlaybackMode = .sequential

    public var mode: PlaybackMode {
        get { withLock { _mode } }
        set { withLock { _mode = newValue } }
    }

    public var count: Int {
        withLock { items.count }
    }

    public var currentItem: PlaylistItem? {
        withLock { currentItemLocked }
    }

    public var currentPosition: Int {
        withLock { currentIndex }
    }

    public init() {}

    private func withLock<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }

    private var currentItemLocked: PlaylistItem? {
        guard currentIndex >= 0 && currentIndex < items.count else {
            return nil
        }
        return items[currentIndex]
    }

    private func appendLocked(_ item: PlaylistItem) {
        items.append(item)
        if currentIndex == -1 {
            currentIndex = 0
        }
    }

    public func add(_ item: PlaylistItem) {
        withLock { appendLocked(item) }
    }

    public func add(url: URL) {
        withLock { appendLocked(PlaylistItem(url: url)) }
    }

    public func addAll(urls: [URL]) {
        withLock { urls.forEach { appendLocked(PlaylistItem(url: $0)) } }
    }

    public func remove(at index: Int) {
        withLock {
            guard index >= 0 && index < items.count else { return }

            items.remove(at: index)

            if items.isEmpty {
                currentIndex = -1
            } else if index < currentIndex {
                currentIndex -= 1
            } else if currentIndex >= items.count {
                currentIndex = items.count - 1
            }
        }
    }

    public func clear() {
        withLock {
            items.removeAll()
            currentIndex = -1
        }
    }

    public func next() -> PlaylistItem? {
        withLock {
            guard !items.isEmpty else { return nil }

            switch _mode {
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
                currentIndex = randomIndexLocked()
                return items[currentIndex]
            }
        }
    }

    public func previous() -> PlaylistItem? {
        withLock {
            guard !items.isEmpty else { return nil }

            switch _mode {
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
                currentIndex = randomIndexLocked()
                return items[currentIndex]
            }
        }
    }

    /// Picks a random index, avoiding an immediate repeat of the current track
    /// when more than one item is available.
    private func randomIndexLocked() -> Int {
        guard items.count > 1 else { return 0 }
        var index = Int.random(in: 0..<items.count)
        while index == currentIndex {
            index = Int.random(in: 0..<items.count)
        }
        return index
    }

    public func jumpTo(index: Int) -> PlaylistItem? {
        withLock {
            guard index >= 0 && index < items.count else { return nil }
            currentIndex = index
            return items[currentIndex]
        }
    }

    public func allItems() -> [PlaylistItem] {
        withLock { items }
    }
}
