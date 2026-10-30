// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>
#include "Server/Library/LibraryNames.h"

using namespace camel::server;
using Names = std::vector<std::string>;

namespace {

std::string joined(const std::vector<LibraryNamePart>& parts) {
    std::string text;
    for (const auto& part : parts) text += part.text;
    return text;
}

} // namespace

TEST(LibraryNames, AlwaysSplitsSemicolonsCommasAndFeaturedArtists) {
    const LibraryNameSplitter splitter;
    EXPECT_EQ(splitter.names("Academy of Ancient Music; Christopher Hogwood"),
              (Names{"Academy of Ancient Music", "Christopher Hogwood"}));
    EXPECT_EQ(splitter.names("絢香、コブクロ"), (Names{"絢香", "コブクロ"}));
    EXPECT_EQ(splitter.names("A；B"), (Names{"A", "B"}));
    EXPECT_EQ(splitter.names("Lil Nas X feat. Billy Ray Cyrus"), (Names{"Lil Nas X", "Billy Ray Cyrus"}));
    EXPECT_EQ(splitter.names("\"L.E.D\" feat.KOTOKO"), (Names{"\"L.E.D\"", "KOTOKO"}));
    EXPECT_EQ(splitter.names("ACE featuring CHiCO"), (Names{"ACE", "CHiCO"}));
    EXPECT_EQ(splitter.names("A Ft. B"), (Names{"A", "B"}));
    EXPECT_EQ(splitter.names("Aimer with chelly"), (Names{"Aimer", "chelly"}));
    EXPECT_EQ(splitter.names("Daft. Punk"), (Names{"Daft. Punk"}));
    EXPECT_EQ(splitter.names("Without Within"), (Names{"Without Within"}));
}

TEST(LibraryNames, SplitsABracketThatOpensWithFeatOrWith) {
    const LibraryNameSplitter splitter;
    const auto parts = splitter.parts("Diana Panton (feat. Harrison Kennedy)");
    EXPECT_EQ(parts, (std::vector<LibraryNamePart>{
                         {"Diana Panton", true}, {" (feat. ", false}, {"Harrison Kennedy", true}, {")", false}}));
    EXPECT_EQ(splitter.names("milet( with iri)"), (Names{"milet", "iri"}));
}

TEST(LibraryNames, KeepsOtherBracketsWithTheirName) {
    const LibraryNameSplitter splitter({"ACE", "工藤ともり", "CHiCO"});
    EXPECT_EQ(splitter.names("ACE(工藤ともり、CHiCO)"), (Names{"ACE(工藤ともり、CHiCO)"}));
    EXPECT_EQ(splitter.names("B小町 ルビー(CV.伊駒ゆりえ)"), (Names{"B小町 ルビー(CV.伊駒ゆりえ)"}));
    EXPECT_EQ(splitter.names("ACE(工藤ともり、CHiCO)、光田康典"), (Names{"ACE(工藤ともり、CHiCO)", "光田康典"}));
}

TEST(LibraryNames, SplitsAmbiguousSeparatorsOnlyBetweenKnownNames) {
    const LibraryNameSplitter splitter({"Ado", "初音ミク", "Bob Marley", "Calexico", "Iron & Wine", "絢香", "コブクロ",
                                        "Akihiro Namba", "Takeshi Ueda", "Xi", "Sakuzyo", "Barry White", "Tamia"});
    EXPECT_EQ(splitter.names("Ado, 初音ミク"), (Names{"Ado", "初音ミク"}));
    EXPECT_EQ(splitter.names("22/7"), (Names{"22/7"}));
    EXPECT_EQ(splitter.names("3×4×S"), (Names{"3×4×S"}));
    EXPECT_EQ(splitter.names("Bob Marley & The Wailers"), (Names{"Bob Marley & The Wailers"}));
    EXPECT_EQ(splitter.names("Calexico and Iron & Wine"), (Names{"Calexico", "Iron & Wine"}));
    EXPECT_EQ(splitter.names("絢香 x コブクロ"), (Names{"絢香", "コブクロ"}));
    EXPECT_EQ(splitter.names("TOMORROW X TOGETHER"), (Names{"TOMORROW X TOGETHER"}));
    EXPECT_EQ(splitter.names("Akihiro Namba × Takeshi Ueda"), (Names{"Akihiro Namba", "Takeshi Ueda"}));
    EXPECT_EQ(splitter.names("Xi Vs Sakuzyo"), (Names{"Xi", "Sakuzyo"}));
    EXPECT_EQ(splitter.names("Barry White, Tamia and Toots Thielemans"), (Names{"Barry White, Tamia and Toots Thielemans"}));
}

TEST(LibraryNames, SplitsColonsOnlyBetweenKnownNames) {
    const LibraryNameSplitter splitter({"SawanoHiroyuki[nZk]", "mizuki", "Claudio Abbado", "Chicago Symphony Orchestra"});
    EXPECT_EQ(splitter.names("SawanoHiroyuki[nZk]:mizuki"), (Names{"SawanoHiroyuki[nZk]", "mizuki"}));
    EXPECT_EQ(splitter.names("Claudio Abbado: Chicago Symphony Orchestra"),
              (Names{"Claudio Abbado", "Chicago Symphony Orchestra"}));
    EXPECT_EQ(splitter.names("SawanoHiroyuki[nZk]：mizuki"), (Names{"SawanoHiroyuki[nZk]", "mizuki"}));
    EXPECT_EQ(splitter.names("Re:Zero"), (Names{"Re:Zero"}));
}

TEST(LibraryNames, CaseTellsNamesApartButWidthDoesNot) {
    const LibraryNameSplitter cased({"Lisa", "Ado"});
    EXPECT_EQ(cased.names("LiSA & Ado"), (Names{"LiSA & Ado"}));
    const LibraryNameSplitter wide({"ＬｉＳＡ", "Ado"});
    EXPECT_EQ(wide.names("LiSA & Ado"), (Names{"LiSA", "Ado"}));
    EXPECT_EQ(libraryNameKey("ＬｉＳＡ"), libraryNameKey("LiSA"));
    EXPECT_NE(libraryNameKey("LiSA"), libraryNameKey("Lisa"));
}

TEST(LibraryNames, KnowsNamesFromCreditsWithFeaturedArtists) {
    const LibraryNameSplitter splitter({"Aimer with chelly", "Vaundy feat. Aimer", "milet & Aimer"});
    EXPECT_EQ(splitter.names("chelly & Vaundy"), (Names{"chelly", "Vaundy"}));
    EXPECT_EQ(splitter.names("milet & Aimer"), (Names{"milet & Aimer"}));
}

TEST(LibraryNames, PartsGiveBackTheCredit) {
    const LibraryNameSplitter splitter({"Ado", "初音ミク"});
    for (const std::string credit : {"Ado, 初音ミク", " Ado ;  初音ミク ", "Diana Panton (feat. Harrison Kennedy)", "22/7",
                                     "A feat. B with C", "; A", "A feat."}) {
        EXPECT_EQ(joined(splitter.parts(credit)), credit) << credit;
    }
    EXPECT_TRUE(splitter.parts("").empty());
    EXPECT_EQ(splitter.names("A feat."), (Names{"A"}));
}
