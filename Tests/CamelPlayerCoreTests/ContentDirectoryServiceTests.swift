import XCTest
@testable import CamelPlayerCore

final class ContentDirectoryServiceTests: XCTestCase {
    func testSearchPreservesQuotesAndBackslashesAsLiteralText() {
        let criteria = ContentDirectoryService.textSearchCriteria(#"Miles "Live"\Set"#)
        XCTAssertEqual(criteria, #"dc:title contains "Miles \"Live\"\\Set" or upnp:artist contains "Miles \"Live\"\\Set" or upnp:album contains "Miles \"Live\"\\Set""#)
    }

    func testMalformedBrowseResultsThrowInsteadOfAppearingEmpty() {
        for response in [
            [:],
            ["Result": "<DIDL-Lite><item id='1'/></DIDL"],
            ["Result": "<html/>"],
            ["Result": "<DIDL-Lite/>", "NumberReturned": "1"],
            ["Result": "<DIDL-Lite/>", "TotalMatches": "-1"],
            ["Result": "<DIDL-Lite/>", "NumberReturned": "invalid"]
        ] {
            XCTAssertThrowsError(try ContentDirectoryService.parseResult(response))
        }
    }

    func testEmptyLibraryAndServerLimitedPage() throws {
        let empty = try ContentDirectoryService.parseResult(["Result": "", "NumberReturned": "0", "TotalMatches": "0"])
        XCTAssertTrue(empty.objects.isEmpty)
        let page = try ContentDirectoryService.parseResult([
            "Result": "<DIDL-Lite><item id='1' parentID='0'><title>Track</title></item></DIDL-Lite>",
            "NumberReturned": "1", "TotalMatches": "250"
        ])
        XCTAssertEqual(page.objects.first?.title, "Track")
        XCTAssertEqual(page.numberReturned, 1)
        XCTAssertEqual(page.totalMatches, 250)
    }
}
