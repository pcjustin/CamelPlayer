import XCTest
@testable import CamelPlayerCore

final class ContainerTraversalTests: XCTestCase {
    private func track(_ id: String) -> MediaObject {
        MediaObject(id: id, parentID: "0", title: id, isContainer: false, resURL: "http://nas/\(id)")
    }

    func testServerPageLimitDoesNotTruncateContainer() async throws {
        var indexes: [Int] = []
        var added: [String] = []
        let count = try await PlaybackController.addContainer(objectID: "0", browse: { _, index, requested in
            XCTAssertEqual(requested, 200)
            indexes.append(index)
            let objects = (index..<min(index + 100, 250)).map { self.track(String($0)) }
            return .init(objects: objects, numberReturned: objects.count, totalMatches: 250)
        }, add: { added.append($0.id); return true })
        XCTAssertEqual(indexes, [0, 100, 200])
        XCTAssertEqual(count, 250)
        XCTAssertEqual(added, (0..<250).map(String.init))
    }

    func testNestedContainersAndUnplayableItems() async throws {
        var added: [String] = []
        let count = try await PlaybackController.addContainer(objectID: "0", browse: { id, _, _ in
            let objects = id == "0"
                ? [MediaObject(id: "album", parentID: "0", title: "Album", isContainer: true), self.track("tail")]
                : [self.track("song"), MediaObject(id: "bad", parentID: "album", title: "Bad", isContainer: false)]
            return .init(objects: objects, numberReturned: objects.count, totalMatches: objects.count)
        }, add: { object in
            guard object.resURL != nil else { return false }
            added.append(object.id)
            return true
        })
        XCTAssertEqual(count, 2)
        XCTAssertEqual(added, ["song", "tail"])
    }

    func testEmptyPageTerminatesDespiteStaleTotal() async throws {
        var calls = 0
        let count = try await PlaybackController.addContainer(objectID: "0", browse: { _, _, _ in
            calls += 1
            return .init(objects: [], numberReturned: 0, totalMatches: 500)
        }, add: { _ in XCTFail("Empty page must not add items"); return true })
        XCTAssertEqual(count, 0)
        XCTAssertEqual(calls, 1)
    }
}
