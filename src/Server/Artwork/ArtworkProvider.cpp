// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Server/Artwork/ArtworkProvider.h"

#include <cwctype>
#include <exception>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <taglib/flacfile.h>
#include <taglib/wavfile.h>
#include <taglib/aifffile.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/dsffile.h>
#include <taglib/dsdifffile.h>
#include "Engine/decode/FormatProbe.h"
#include <taglib/tfilestream.h>
#include <taglib/tvariant.h>

#include "Common/util/Log.h"

namespace camel::server {
namespace {

constexpr std::wstring_view kNames[] = {L"cover", L"folder", L"front"};
constexpr std::pair<std::wstring_view, const char*> kExtensions[] = {
    {L".jpg", "image/jpeg"}, {L".jpeg", "image/jpeg"}, {L".png", "image/png"}, {L".webp", "image/webp"}};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

// The best-ranked cover image directly in `folder`.
std::optional<ArtworkCandidate> folderImageFile(const std::filesystem::path& folder, const std::string& source,
                                            const CancellationToken& token) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return std::nullopt;
    std::optional<std::pair<size_t, std::filesystem::path>> best; // rank, path
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (token.cancelled()) return std::nullopt;
        if (!entry.is_regular_file(ec)) continue;
        if (const auto rank = folderArtworkRank(entry.path()); rank && (!best || rank->first < best->first))
            best = {rank->first, entry.path()};
    }
    if (!best) return std::nullopt;
    const auto size = std::filesystem::file_size(best->second, ec);
    if (ec || size == 0 || size > kMaxArtworkBytes) return std::nullopt;
    ArtworkCandidate candidate;
    candidate.mime = kExtensions[best->first % std::size(kExtensions)].second;
    candidate.source = source;
    candidate.file = best->second;
    return candidate;
}

class ImageStream : public TagLib::FileStream {
public:
    ImageStream(const std::filesystem::path& path, const CancellationToken& token)
        : FileStream(path.c_str(), true), token_(token) {}
    TagLib::ByteVector readBlock(size_t length) override {
        if (token_.cancelled() || length > budget_) throw std::runtime_error("artwork metadata read limit");
        budget_ -= length;
        return FileStream::readBlock(length);
    }

private:
    size_t budget_ = 64ull << 20;
    CancellationToken token_;
};
class EmbeddedProvider final : public IArtworkProvider {
public:
    std::string id() const override { return "embedded"; }
    bool needsNetwork() const override { return false; }
    std::vector<ArtworkCandidate> find(const ArtworkQuery& query, const CancellationToken& token) override {
        const auto& path = query.firstTrack;
        if (path.empty() || token.cancelled()) return {};
        ImageStream stream(path, token);
        std::unique_ptr<TagLib::File> file;
        switch (audio::probeFormat(path)) {
        case audio::AudioFormat::Flac:
            file = std::make_unique<TagLib::FLAC::File>(&stream, false);
            break;
        case audio::AudioFormat::Wav:
            file = std::make_unique<TagLib::RIFF::WAV::File>(&stream, false);
            break;
        case audio::AudioFormat::Aiff:
            file = std::make_unique<TagLib::RIFF::AIFF::File>(&stream, false);
            break;
        case audio::AudioFormat::Alac:
        case audio::AudioFormat::Aac:
            file = std::make_unique<TagLib::MP4::File>(&stream, false);
            break;
        case audio::AudioFormat::Mp3:
            file = std::make_unique<TagLib::MPEG::File>(&stream, false);
            break;
        case audio::AudioFormat::Dsd: {
            const auto header = stream.readBlock(4);
            stream.seek(0);
            if (header == TagLib::ByteVector("DSD ", 4))
                file = std::make_unique<TagLib::DSF::File>(&stream, false);
            else
                file = std::make_unique<TagLib::DSDIFF::File>(&stream, false);
            break;
        }
        }
        if (!file || !file->isValid()) return {};
        const auto pictures = file->complexProperties("PICTURE");
        for (const auto& picture : pictures) {
            const auto type = picture.value("pictureType").toString();
            if (type != "Front Cover" && !type.isEmpty()) continue;
            const auto bytes = picture.value("data").toByteVector();
            if (bytes.isEmpty() || bytes.size() > kMaxArtworkBytes) continue;
            ArtworkCandidate out;
            out.source = "embedded";
            out.bytes.assign(bytes.begin(), bytes.end());
            return {std::move(out)};
        }
        return {};
    }
};

class FolderProvider final : public IArtworkProvider {
public:
    explicit FolderProvider(bool parent) : parent_(parent) {}
    std::string id() const override { return parent_ ? "parent-folder" : "folder"; }
    bool needsNetwork() const override { return false; }
    std::vector<ArtworkCandidate> find(const ArtworkQuery& query, const CancellationToken& token) override {
        auto folder = query.albumFolder;
        if (parent_) {
            if (!isDiscFolderName(folder.filename().wstring())) return {};
            folder = folder.parent_path();
        }
        if (auto image = folderImageFile(folder, id(), token)) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(image->file, ec);
            if (ec || !size || size > kMaxArtworkBytes) return {};
            std::ifstream input(image->file, std::ios::binary);
            image->bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            if (image->bytes.size() != size) return {};
            return {std::move(*image)};
        }
        return {};
    }

private:
    bool parent_;
};

bool isAsciiLetter(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

// Optional spaces, optional zeros, then at least one digit.
bool numberFollows(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) i++;
    return i < s.size() && s[i] >= L'0' && s[i] <= L'9';
}

} // namespace

void ArtworkChain::add(std::unique_ptr<IArtworkProvider> provider) {
    providers_.push_back(std::move(provider));
}

std::optional<ArtworkCandidate> ArtworkChain::first(const ArtworkQuery& query, const CancellationToken& token,
                                                    bool allowNetwork) const {
    for (const auto& provider : providers_) {
        if (token.cancelled()) return std::nullopt;
        if (provider->needsNetwork() && !allowNetwork) continue;
        try {
            auto candidates = provider->find(query, token);
            if (!candidates.empty()) return std::move(candidates.front());
        } catch (const std::exception& e) {
            log::warn("artwork: provider %s failed: %s", provider->id().c_str(), e.what());
        }
    }
    return std::nullopt;
}

std::vector<std::string> ArtworkChain::ids() const {
    std::vector<std::string> out;
    out.reserve(providers_.size());
    for (const auto& provider : providers_) out.push_back(provider->id());
    return out;
}

std::unique_ptr<IArtworkProvider> makeFolderArtworkProvider() {
    return std::make_unique<FolderProvider>(false);
}

std::optional<std::pair<size_t, const char*>> folderArtworkRank(const std::filesystem::path& file) {
    const std::wstring stem = lower(file.stem().wstring());
    const std::wstring ext = lower(file.extension().wstring());
    for (size_t n = 0; n < std::size(kNames); n++)
        for (size_t e = 0; e < std::size(kExtensions); e++)
            if (stem == kNames[n] && ext == kExtensions[e].first)
                return std::pair{n * std::size(kExtensions) + e, kExtensions[e].second};
    return std::nullopt;
}

std::optional<ArtworkCandidate> findFolderArtworkFile(const std::filesystem::path& folder, const CancellationToken& token) {
    return folderImageFile(folder, "folder", token);
}

std::unique_ptr<IArtworkProvider> makeParentFolderArtworkProvider() {
    return std::make_unique<FolderProvider>(true);
}

std::unique_ptr<IArtworkProvider> makeEmbeddedArtworkProvider() {
    return std::make_unique<EmbeddedProvider>();
}

ArtworkChain localArtworkChain() {
    ArtworkChain chain;
    chain.add(makeFolderArtworkProvider());
    chain.add(makeEmbeddedArtworkProvider());
    chain.add(makeParentFolderArtworkProvider());
    return chain;
}

bool isDiscFolderName(std::wstring_view name) {
    const std::wstring folded = lower(std::wstring(name));
    for (size_t i = 0; i < folded.size(); i++) {
        if (folded[i] == L'碟' && numberFollows(std::wstring_view(folded).substr(i + 1))) return true;
        if (i > 0 && isAsciiLetter(folded[i - 1])) continue;
        for (std::wstring_view word : {L"cd", L"disc", L"disk"})
            if (std::wstring_view(folded).substr(i, word.size()) == word &&
                numberFollows(std::wstring_view(folded).substr(i + word.size())))
                return true;
    }
    return false;
}

std::wstring albumTitleWithoutDiscSuffix(std::wstring_view title) {
    std::wstring result(title);
    while (!result.empty() && std::iswspace(result.back())) result.pop_back();
    size_t end = result.size();
    if (end && (result[end - 1] == L')' || result[end - 1] == L']')) --end;
    size_t suffix = std::wstring::npos;
    for (size_t i = 0; i < end; ++i) {
        const auto candidate = std::wstring_view(result).substr(i, end - i);
        const auto folded = lower(std::wstring(candidate));
        const auto numbered = [&](size_t marker) {
            size_t n = marker;
            while (n < folded.size() && std::iswspace(folded[n])) ++n;
            const size_t first = n;
            while (n < folded.size() && folded[n] >= L'0' && folded[n] <= L'9') ++n;
            return n > first && n == folded.size();
        };
        const bool discSuffix = (folded.starts_with(L"cd") && numbered(2)) || (folded.starts_with(L"disc") && numbered(4)) ||
                                (folded.starts_with(L"disk") && numbered(4)) || (folded.starts_with(L"碟") && numbered(1));
        if ((i == 0 || std::iswspace(result[i - 1]) || result[i - 1] == L'(' || result[i - 1] == L'[') && discSuffix) {
            suffix = i;
            break;
        }
    }
    if (suffix == std::wstring::npos) return result;
    while (suffix && std::iswspace(result[suffix - 1])) --suffix;
    if (suffix && (result[suffix - 1] == L'(' || result[suffix - 1] == L'[')) --suffix;
    result.resize(suffix);
    while (!result.empty() && std::iswspace(result.back())) result.pop_back();
    return result;
}

} // namespace camel::server
