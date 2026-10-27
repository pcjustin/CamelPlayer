// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// gen-library: a synthetic music library for the docs/Plan.md §12.1 UI measurements.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Commands.h"
#include "Engine/testing/TestAudio.h"
#include "Common/util/Text.h"

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace camelcli {
namespace {

struct FlacTemplate {
    std::vector<uint8_t> streamInfo; // block body
    std::vector<uint8_t> frames;
};

FlacTemplate flacTemplate(const std::filesystem::path& scratch) {
    const auto path = scratch / L"template.flac";
    camel::audio::testaudio::writeFlacSamples(path, std::vector<int32_t>(size_t{44100} / 4 * 2, 0), 44100, 16, 2);
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    std::filesystem::remove(path);
    FlacTemplate result;
    size_t at = 4;
    for (bool last = false; !last && at + 4 <= bytes.size();) {
        last = bytes[at] & 0x80;
        const uint8_t type = bytes[at] & 0x7F;
        const size_t length = size_t(bytes[at + 1]) << 16 | size_t(bytes[at + 2]) << 8 | bytes[at + 3];
        if (type == 0) result.streamInfo.assign(bytes.begin() + ptrdiff_t(at + 4), bytes.begin() + ptrdiff_t(at + 4 + length));
        at += 4 + length;
    }
    result.frames.assign(bytes.begin() + ptrdiff_t(at), bytes.end());
    if (result.streamInfo.size() != 34 || result.frames.empty()) throw std::runtime_error("unexpected template FLAC");
    return result;
}

void writeTagged(const std::filesystem::path& path, const FlacTemplate& flac, const std::vector<std::string>& comments) {
    std::vector<uint8_t> out{'f', 'L', 'a', 'C', 0x00, 0, 0, 34};
    out.insert(out.end(), flac.streamInfo.begin(), flac.streamInfo.end());
    std::vector<uint8_t> body;
    const auto u32 = [&](size_t v) {
        for (int i = 0; i < 4; ++i) body.push_back(uint8_t(v >> (8 * i)));
    };
    const std::string vendor = "camelcli gen-library";
    u32(vendor.size());
    body.insert(body.end(), vendor.begin(), vendor.end());
    u32(comments.size());
    for (const auto& c : comments) {
        u32(c.size());
        body.insert(body.end(), c.begin(), c.end());
    }
    out.insert(out.end(), {uint8_t(0x80 | 4), uint8_t(body.size() >> 16), uint8_t(body.size() >> 8), uint8_t(body.size())});
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), flac.frames.begin(), flac.frames.end());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

void writeCover(IWICImagingFactory* factory, const std::filesystem::path& path, uint32_t seed) {
    constexpr UINT size = 600;
    std::vector<uint8_t> pixels(size_t{size} * size * 3);
    const auto r = uint8_t(seed * 73), g = uint8_t(seed * 151), b = uint8_t(seed * 199);
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x) {
            uint8_t* p = &pixels[(size_t{y} * size + x) * 3];
            p[0] = uint8_t(b + x / 3);
            p[1] = uint8_t(g + y / 3);
            p[2] = uint8_t(r + ((x ^ y) & 0x3F));
        }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(size, size)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(size, size * 3, UINT(pixels.size()), pixels.data())) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit()))
        throw std::runtime_error("cannot write cover " + camel::toUtf8(path.wstring()));
}

// Deterministic names mixing Latin, Traditional Chinese and Japanese, so search and sorting see varied text.
std::string word(uint32_t n) {
    static const std::array<const char*, 24> words{
        "Blue", "Night", "River", "Echo", "Garden", "Silver", "Morning", "Song",   "Light", "Winter", "Ocean", "Dream",
        "月光", "海風",  "夜曲",  "春雨", "星空",   "花園",   "さくら",  "ひかり", "そら",  "Café",   "Étude", "Nocturne"};
    return words[n % words.size()];
}

} // namespace

// gen-library <dir> [--tracks N] [--per-album N]: <dir>\music\<artist>\<album>\NN - <title>.flac, each album with a
// distinct 600 px cover.jpg; every file is a 0.25 s silent 16/44.1 FLAC with full tags.
int cmdGenLibrary(const Args& args) {
    const auto pos = positional(args);
    if (pos.empty()) {
        std::puts("usage: gen-library <dir> [--tracks N] [--per-album N]");
        return 2;
    }
    const uint32_t tracks = option(args, L"--tracks") ? uint32_t(std::stoul(*option(args, L"--tracks"))) : 100000;
    const uint32_t perAlbum = option(args, L"--per-album") ? uint32_t(std::stoul(*option(args, L"--per-album"))) : 12;
    const std::filesystem::path music = std::filesystem::path(pos[0]) / L"music";
    std::filesystem::create_directories(music);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        throw std::runtime_error("WIC unavailable");
    const auto flac = flacTemplate(music);
    static const std::array<const char*, 12> genres{"Jazz", "Classical", "Rock",  "Pop",      "Soundtrack", "Electronic",
                                                    "Folk", "Blues",     "J-Pop", "Mandopop", "Ambient",    "Soul"};
    const uint32_t albums = (tracks + perAlbum - 1) / perAlbum;
    const uint32_t artists = std::max(1u, albums / 5);
    uint32_t written = 0;
    for (uint32_t album = 0; album < albums && written < tracks; ++album) {
        const uint32_t artist = album % artists;
        const std::string artistName = word(artist) + " " + word(artist / 24 + 7) + " " + std::to_string(artist);
        const std::string albumName = word(album * 7 + 3) + " " + word(album / 3 + 11) + " " + std::to_string(album);
        const auto folder = music / camel::toWide(artistName) / camel::toWide(albumName);
        std::filesystem::create_directories(folder);
        writeCover(factory.Get(), folder / L"cover.jpg", album);
        const std::string year = std::to_string(1960 + album % 66);
        for (uint32_t n = 1; n <= perAlbum && written < tracks; ++n, ++written) {
            const std::string title = word(written * 5 + n) + " " + word(written / 7 + 2) + " " + std::to_string(written);
            char number[8];
            std::snprintf(number, sizeof number, "%02u", n);
            writeTagged(folder / camel::toWide(std::string(number) + " - " + title + ".flac"), flac,
                        {"TITLE=" + title, "ARTIST=" + artistName, "ALBUMARTIST=" + artistName, "ALBUM=" + albumName,
                         "TRACKNUMBER=" + std::to_string(n), "DATE=" + year,
                         std::string("GENRE=") + genres[album % genres.size()],
                         "COMPOSER=" + word(album % 40 + 1) + " Composer"});
        }
        if (album % 500 == 0) std::printf("\r%u / %u tracks", written, tracks);
    }
    std::printf("\r%u tracks in %u albums by %u artists under %s\n", written, albums, artists,
                camel::toUtf8(music.wstring()).c_str());
    return 0;
}

} // namespace camelcli
