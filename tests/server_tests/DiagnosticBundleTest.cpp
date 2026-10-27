// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include <gtest/gtest.h>
#include <zlib.h>

#include <fstream>
#include <map>

#include "Server/Support/DiagnosticBundle.h"
#include "Server/Support/ZipWriter.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::server;

namespace {

uint32_t read16(const std::string& s, size_t at) {
    return uint8_t(s[at]) | (uint32_t(uint8_t(s[at + 1])) << 8);
}

uint32_t read32(const std::string& s, size_t at) {
    return read16(s, at) | (read16(s, at + 2) << 16);
}

// Reads every entry through the central directory and inflates it, checking sizes and CRCs on the way.
std::map<std::string, std::string> readZip(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string zip{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const size_t end = zip.size() - 22;
    EXPECT_EQ(read32(zip, end), 0x06054b50u);
    const uint32_t count = read16(zip, end + 10);
    size_t at = read32(zip, end + 16);
    std::map<std::string, std::string> entries;
    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_EQ(read32(zip, at), 0x02014b50u);
        const uint32_t crc = read32(zip, at + 16), compressed = read32(zip, at + 20), size = read32(zip, at + 24);
        const uint32_t nameLength = read16(zip, at + 28), offset = read32(zip, at + 42);
        const std::string name = zip.substr(at + 46, nameLength);
        at += 46 + nameLength + read16(zip, at + 30) + read16(zip, at + 32);
        const size_t data = offset + 30 + read16(zip, offset + 26) + read16(zip, offset + 28);
        std::string out(size, '\0');
        z_stream z{};
        inflateInit2(&z, -MAX_WBITS);
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zip.data() + data));
        z.avail_in = compressed;
        z.next_out = reinterpret_cast<Bytef*>(out.data());
        z.avail_out = size;
        EXPECT_EQ(inflate(&z, Z_FINISH), Z_STREAM_END) << name;
        inflateEnd(&z);
        EXPECT_EQ(crc32(0, reinterpret_cast<const Bytef*>(out.data()), uInt(out.size())), crc) << name;
        entries[name] = out;
    }
    return entries;
}

void writeText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

class FormatsOutput final : public audio::IOutput {
public:
    bool supports(const audio::DeviceFormat& f) override {
        return (f.type == audio::SampleType::Int24In32 && f.sampleRate <= 192000) ||
               (f.type == audio::SampleType::Dsd8 && f.sampleRate == 2822400 / 8);
    }
    void open(const audio::DeviceFormat&, audio::IRenderSource*) override {}
    void start() override {}
    void pause() override {}
    void close() override {}
    audio::OutputStatus status() const override { return {}; }
};

TEST(DiagnosticBundle, ZipEntriesReadBackUnchanged) {
    const auto file = testutil::tempDir() / L"zip-writer.zip";
    const std::string big(200000, 'x');
    ZipWriter zip(file);
    zip.add("a.txt", "hello");
    zip.add("logs/\xE6\x97\xA5.log", big);
    zip.add("empty.txt", "");
    zip.finish();
    const auto entries = readZip(file);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries.at("a.txt"), "hello");
    EXPECT_EQ(entries.at("logs/\xE6\x97\xA5.log"), big);
    EXPECT_EQ(entries.at("empty.txt"), "");
    EXPECT_LT(std::filesystem::file_size(file), 10000u);
}

TEST(DiagnosticBundle, RedactsPathsUpToWhatFollowsThem) {
    EXPECT_EQ(redactPaths("engine: \\\\nas\\music\\A, B\\01 - x.flac via libsndfile, 44.1 kHz / 16, start 0"),
              "engine: <path> via libsndfile, 44.1 kHz / 16, start 0");
    EXPECT_EQ(redactPaths("library: cannot open C:\\Users\\me\\db: locked\r\nnext D:/x y\n"),
              "library: cannot open <path>: locked\r\nnext <path>\n");
    EXPECT_EQ(redactPaths("root unavailable (53): \\\\?\\unc\\tachikoma\\music"), "root unavailable (53): <path>");
    EXPECT_EQ(redactPaths("engine: E:\\a.dsf is reported when playback starts (x)"),
              "engine: <path> is reported when playback starts (x)");
    EXPECT_EQ(redactPaths("44.1 kHz, ratio 1:2, AB:\\ not a drive"), "44.1 kHz, ratio 1:2, AB:\\ not a drive");
}

TEST(DiagnosticBundle, ProbesEveryRateAndNativeDsdForAsio) {
    FormatsOutput output;
    Settings settings;
    settings.backend = Backend::Asio;
    const auto text = probeCapabilities(output, settings);
    EXPECT_NE(text.find("192000\t-\t-\tyes\t-\t-"), std::string::npos) << text;
    EXPECT_NE(text.find("384000\t-\t-\t-\t-\t-"), std::string::npos) << text;
    EXPECT_NE(text.find("DSD64 yes DSD128 -"), std::string::npos) << text;
    settings.backend = Backend::Wasapi;
    EXPECT_EQ(probeCapabilities(output, settings).find("DSD64"), std::string::npos);
}

TEST(DiagnosticBundle, LeavesPathsOutUnlessIncluded) {
    const auto data = testutil::tempDir() / L"diagnostic-data";
    std::filesystem::remove_all(data);
    writeText(data / L"settings.json", R"({"backend":"wasapi","lastFile":"C:\\Music\\secret.flac"})");
    writeText(data / L"logs" / L"CamelPlayer-20261027.log", "a\nengine: C:\\Music\\secret.flac via dr_flac\n");
    writeText(data / L"logs" / L"CamelPlayer-20261028.log", "b\n");
    writeText(data / L"crashdumps" / L"crash-20261020-010101.dmp", "old");
    writeText(data / L"crashdumps" / L"crash-20261026-061058.dmp", "new");

    DiagnosticInput input;
    input.dataDirectory = data;
    input.appVersion = "0.1.0";
    input.snapshot.playback.pathUtf8 = "C:\\Music\\secret.flac";
    input.snapshot.devices.wasapi = {{"{0.0.0}.{id}", "Speakers (DAVE)"}};
    input.roots = {{1, R"(\\nas\music)", "music", "nas", true, 0}};
    input.capabilities = "probed\n";
    const auto file = testutil::tempDir() / L"diagnostic.zip";

    writeDiagnosticBundle(file, input);
    auto entries = readZip(file);
    EXPECT_EQ(entries.size(), 8u);
    for (const auto& [name, text] : entries) EXPECT_EQ(text.find("secret"), std::string::npos) << name;
    EXPECT_EQ(entries.at("logs/CamelPlayer-20261027.log"), "a\nengine: <path> via dr_flac\n");
    EXPECT_EQ(entries.at("crashdumps/crash-20261026-061058.dmp"), "new");
    EXPECT_FALSE(entries.contains("crashdumps/crash-20261020-010101.dmp"));
    EXPECT_NE(entries.at("system.txt").find("CamelPlayer 0.1.0"), std::string::npos);
    EXPECT_NE(entries.at("devices.txt").find("Speakers (DAVE)"), std::string::npos);
    EXPECT_NE(entries.at("devices.txt").find("probed"), std::string::npos);
    EXPECT_NE(entries.at("library-folders.txt").find("Root 1: online, network"), std::string::npos);
    EXPECT_EQ(entries.at("library-folders.txt").find("nas"), std::string::npos);

    input.includePaths = true;
    writeDiagnosticBundle(file, input);
    entries = readZip(file);
    EXPECT_NE(entries.at("settings.json").find("secret.flac"), std::string::npos);
    EXPECT_NE(entries.at("logs/CamelPlayer-20261027.log").find("secret.flac"), std::string::npos);
    EXPECT_NE(entries.at("playback.txt").find("secret.flac"), std::string::npos);
    EXPECT_NE(entries.at("library-folders.txt").find("\\\\nas\\music"), std::string::npos);
}

} // namespace
