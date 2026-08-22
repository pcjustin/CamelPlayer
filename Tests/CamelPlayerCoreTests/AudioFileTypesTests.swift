import XCTest
@testable import CamelPlayerCore

final class AudioFileTypesTests: XCTestCase {
    /// The GTK file dialog filter hard-codes the same extensions in C, where
    /// the Swift constant is not reachable. Parse it back out so a format added
    /// on one side cannot silently be missed on the other.
    func testGTKDialogFilterMatchesSharedList() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let shim = repoRoot.appendingPathComponent("Sources/CGtk4/shim.h")
        let source = try String(contentsOf: shim, encoding: .utf8)

        guard let start = source.range(of: "const char *suffixes[] = {"),
              let end = source.range(of: "NULL};", range: start.upperBound..<source.endIndex) else {
            return XCTFail("suffixes[] literal not found in \(shim.path)")
        }

        let literal = source[start.upperBound..<end.lowerBound]
        let suffixes = literal.split(separator: ",").compactMap { part -> String? in
            let trimmed = part.trimmingCharacters(in: .whitespacesAndNewlines)
            guard trimmed.hasPrefix("\""), trimmed.hasSuffix("\""), trimmed.count > 2 else { return nil }
            return String(trimmed.dropFirst().dropLast())
        }

        XCTAssertEqual(Set(suffixes), Set(audioFileExtensions))
    }
}
