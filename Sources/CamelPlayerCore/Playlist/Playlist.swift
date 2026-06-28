import Foundation

public enum PlaybackMode {
    case sequential
    case loop
    case loopOne
    case shuffle
}

/// Loop is orthogonal to shuffle: off / all (wrap) / one (repeat current).
public enum LoopMode {
    case off
    case all
    case one
}

public class Playlist {
    private var items: [PlaylistItem] = []
    private var currentIndex: Int = -1
    private let lock = NSLock()
    private var _shuffle: Bool = false
    private var _loop: LoopMode = .off
    private var _shufflePlayed: Set<UUID> = []

    public var shuffle: Bool {
        get { withLock { _shuffle } }
        set { withLock { _shuffle = newValue; _shufflePlayed.removeAll() } }
    }

    public var loopMode: LoopMode {
        get { withLock { _loop } }
        set { withLock { _loop = newValue } }
    }

    /// Compatibility view over the orthogonal shuffle + loop state, used by the
    /// CLI and tests which still address the four exclusive modes. Legacy
    /// `.shuffle` maps to shuffle + loop-all (infinite random).
    public var mode: PlaybackMode {
        get {
            withLock {
                if _shuffle { return .shuffle }
                switch _loop {
                case .off: return .sequential
                case .all: return .loop
                case .one: return .loopOne
                }
            }
        }
        set {
            withLock {
                _shufflePlayed.removeAll()
                switch newValue {
                case .sequential: _shuffle = false; _loop = .off
                case .loop: _shuffle = false; _loop = .all
                case .loopOne: _shuffle = false; _loop = .one
                case .shuffle: _shuffle = true; _loop = .all
                }
            }
        }
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
            _shufflePlayed.removeAll()
        }
    }

    public func next() -> PlaylistItem? {
        withLock {
            guard !items.isEmpty else { return nil }

            if _loop == .one { return items[currentIndex] }
            if _shuffle { return shuffleNextLocked() }

            if currentIndex + 1 < items.count {
                currentIndex += 1
                return items[currentIndex]
            }
            if _loop == .all {
                currentIndex = 0
                return items[currentIndex]
            }
            return nil
        }
    }

    /// Random next within the current shuffle cycle. A track is not repeated
    /// until the cycle is exhausted; then loop-all refills it and loop-off stops.
    private func shuffleNextLocked() -> PlaylistItem? {
        if let current = currentItemLocked { _shufflePlayed.insert(current.id) }
        var candidates = items.indices.filter {
            $0 != currentIndex && !_shufflePlayed.contains(items[$0].id)
        }
        if candidates.isEmpty {
            guard _loop == .all else { return nil }
            _shufflePlayed.removeAll()
            candidates = items.indices.filter { $0 != currentIndex }
            if candidates.isEmpty { candidates = Array(items.indices) }
        }
        currentIndex = candidates.randomElement()!
        return items[currentIndex]
    }

    /// The next item without advancing, for gapless preloading. Only the
    /// deterministic in-order cases are eligible; shuffle and loop-one return nil.
    public func peekNext() -> PlaylistItem? {
        withLock {
            guard !items.isEmpty else { return nil }
            if _shuffle || _loop == .one { return nil }
            if currentIndex + 1 < items.count { return items[currentIndex + 1] }
            return _loop == .all ? items[0] : nil
        }
    }

    public func previous() -> PlaylistItem? {
        withLock {
            guard !items.isEmpty else { return nil }

            if _loop == .one { return items[currentIndex] }
            if _shuffle {
                // ponytail: previous in shuffle = fresh random jump, no back-history
                currentIndex = randomIndexLocked()
                return items[currentIndex]
            }

            if currentIndex > 0 {
                currentIndex -= 1
                return items[currentIndex]
            }
            if _loop == .all {
                currentIndex = items.count - 1
                return items[currentIndex]
            }
            return nil
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

    /// Reorders items, keeping the currently playing track current.
    /// Offsets follow SwiftUI's onMove semantics (toOffset is in the pre-move
    /// index space).
    public func move(fromOffsets source: IndexSet, toOffset destination: Int) {
        withLock {
            let currentID = currentItemLocked?.id
            let moving = source.sorted().map { items[$0] }
            for index in source.sorted(by: >) {
                items.remove(at: index)
            }
            let adjusted = destination - source.filter { $0 < destination }.count
            items.insert(contentsOf: moving, at: adjusted)
            if let currentID = currentID,
               let newIndex = items.firstIndex(where: { $0.id == currentID }) {
                currentIndex = newIndex
            }
        }
    }

    public func jumpTo(index: Int) -> PlaylistItem? {
        withLock {
            guard index >= 0 && index < items.count else { return nil }
            currentIndex = index
            _shufflePlayed.removeAll()
            return items[currentIndex]
        }
    }

    public func allItems() -> [PlaylistItem] {
        withLock { items }
    }
}
