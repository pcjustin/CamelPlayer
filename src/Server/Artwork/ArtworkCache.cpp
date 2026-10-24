// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "Server/Artwork/ArtworkCache.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wil/resource.h>
#include <webp/decode.h>
#include <algorithm>
#include <fstream>
#include <map>
#include "Common/util/Md5.h"
#include "Server/Library/LibraryIdentity.h"
#include "Common/util/Text.h"

namespace camel::server {
namespace {
using Microsoft::WRL::ComPtr;
using Path = std::filesystem::path;
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("artwork image operation failed");
}
struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};
struct BackgroundPriority {
    bool changed = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
    ~BackgroundPriority() {
        if (changed) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};
std::string digest(const void* bytes, size_t size) {
    Md5 md5;
    md5.update(bytes, size);
    return toHex(md5.finish());
}
std::string fileKey(const Path& path) {
    const auto version = library::fileVersion(path);
    Md5 md5;
    const auto identity = path.u8string();
    md5.update(identity.data(), identity.size());
    md5.update(&version.size, sizeof(version.size));
    md5.update(&version.mtime, sizeof(version.mtime));
    md5.update(&version.created, sizeof(version.created));
    md5.update(version.volumeKey.data(), version.volumeKey.size());
    md5.update(version.index.data(), version.index.size());
    return toHex(md5.finish());
}
std::string sourceKey(const ArtworkCandidate& image) {
    if (!image.bytes.empty()) return digest(image.bytes.data(), image.bytes.size());
    const auto size = std::filesystem::file_size(image.file);
    if (!size || size > kMaxArtworkBytes) throw std::runtime_error("artwork file size limit");
    return fileKey(image.file);
}
std::optional<ArtworkCandidate> folderImage(const Path& path, const CancellationToken& token) {
    return findFolderArtworkFile(path, token);
}
} // namespace

ArtworkCache::ArtworkCache(Path directory, uintmax_t maxBytes) : directory_(std::move(directory)), maxBytes_(maxBytes) {}

std::optional<ArtworkReference> ArtworkCache::get(const LibraryStore* store, const ArtworkRequest& request,
                                                  const CancellationToken& token) {
    std::lock_guard lock(mutex_);
    if (token.cancelled() || (request.pixels != 256 && request.pixels != 600)) return {};
    BackgroundPriority priority;
    auto use = [&](const std::optional<ArtworkCandidate>& candidate) -> std::optional<ArtworkReference> {
        if (!candidate || token.cancelled()) return {};
        try {
            return generate(*candidate, request.pixels, token);
        } catch (const std::exception&) {
            return {};
        }
    };
    auto track = [&](const Path& path) -> std::optional<ArtworkReference> {
        try {
            const auto key = fileKey(path) + ":" + std::to_string(request.pixels);
            const auto found = embedded_.find(key);
            if (found != embedded_.end()) {
                if (!found->second) return {};
                std::error_code ec;
                if (std::filesystem::exists(found->second->path, ec)) {
                    std::filesystem::last_write_time(found->second->path, std::filesystem::file_time_type::clock::now(), ec);
                    return found->second;
                }
            }
            ArtworkQuery query;
            query.firstTrack = path;
            auto images = makeEmbeddedArtworkProvider()->find(query, token);
            if (token.cancelled() || fileKey(path) + ":" + std::to_string(request.pixels) != key) return {};
            auto result = images.empty() ? std::nullopt : use(std::move(images.front()));
            if (embedded_.size() >= 256) embedded_.erase(embedded_.begin());
            if (!token.cancelled() && (images.empty() || result)) embedded_[key] = result;
            return result;
        } catch (const std::exception&) {
            return {};
        }
    };
    try {
        Path folder, first;
        int64_t album = request.id;
        if (request.kind == ArtworkKind::Track) {
            first = Path(toWide(request.path));
            if (auto ref = track(first)) return ref;
            folder = first.parent_path();
            album = 0;
            if (store) {
                auto db = store->openReader();
                auto row = db.prepare(
                    "SELECT t.album_id FROM tracks t JOIN files f ON f.id=t.file_id WHERE f.path=? AND f.available=1 LIMIT 1");
                auto key = library::storageKey(first);
                row.bind(1, toUtf8(key));
                if (row.step()) album = row.integer(0);
            }
        } else if (request.kind == ArtworkKind::Folder && store) {
            auto db = store->openReader();
            auto row = db.prepare(
                "SELECT d.path FROM folders d JOIN roots r ON r.id=d.root_id WHERE d.id=? AND r.online=1 AND r.removed IS NULL");
            row.bind(1, request.id);
            if (!row.step()) return {};
            folder = Path(toWide(row.text(0)));
            if (auto ref = use(folderImage(folder, token))) return ref;
            auto child = db.prepare(
                "WITH RECURSIVE below(id) AS (SELECT ? UNION ALL SELECT d.id FROM folders d JOIN below b ON d.parent_id=b.id) "
                "SELECT a.id FROM albums a JOIN below b ON b.id=a.folder_id ORDER BY a.sort_key,a.id LIMIT 1");
            child.bind(1, request.id);
            if (!child.step()) return {};
            album = child.integer(0);
        }
        if (album && store) {
            auto db = store->openReader();
            auto row = db.prepare("SELECT d.path FROM albums a JOIN folders d ON d.id=a.folder_id JOIN roots r ON r.id=d.root_id "
                                  "WHERE a.id=? AND r.online=1 AND r.removed IS NULL");
            row.bind(1, album);
            if (!row.step()) return {};
            folder = Path(toWide(row.text(0)));
            auto source =
                db.prepare("SELECT f.path FROM tracks t JOIN files f ON f.id=t.file_id WHERE t.album_id=? AND f.available=1 "
                           "ORDER BY t.disc_number,t.track_number,t.id LIMIT 1");
            source.bind(1, album);
            if (source.step()) {
                first = Path(toWide(source.text(0)));
                folder = first.parent_path();
            }
        }
        if (auto ref = use(folderImage(folder, token))) return ref;
        if (!first.empty())
            if (auto ref = track(first)) return ref;
        if (isDiscFolderName(folder.filename().wstring())) return use(folderImage(folder.parent_path(), token));
    } catch (const std::exception&) {
    }
    return {};
}

std::optional<ArtworkReference> ArtworkCache::thumbnail(const ArtworkCandidate& image, unsigned pixels,
                                                        const CancellationToken& token) {
    std::lock_guard lock(mutex_);
    try {
        return generate(image, pixels, token);
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<ArtworkReference> ArtworkCache::generate(const ArtworkCandidate& image, unsigned pixels,
                                                       const CancellationToken& token) {
    if (token.cancelled() || (pixels != 256 && pixels != 600) || image.bytes.size() > kMaxArtworkBytes) return {};
    const auto key = sourceKey(image) + "-" + std::to_string(pixels);
    const auto target = directory_ / (key + ".jpg");
    std::filesystem::create_directories(directory_);
    if (!diskBytes_) trim();
    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec) && std::filesystem::file_size(target, ec) > 0) {
        std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), ec);
        return ArtworkReference{key, target};
    }
    auto bytes = image.bytes;
    if (bytes.empty()) {
        const auto size = std::filesystem::file_size(image.file);
        if (!size || size > kMaxArtworkBytes) return {};
        bytes.resize(size_t(size));
        std::ifstream input(image.file, std::ios::binary);
        for (size_t offset = 0; offset < bytes.size();) {
            if (token.cancelled()) return {};
            const auto count = std::min<size_t>(64 * 1024, bytes.size() - offset);
            if (!input.read(reinterpret_cast<char*>(bytes.data() + offset), std::streamsize(count))) return {};
            offset += count;
        }
        if (sourceKey(image) + "-" + std::to_string(pixels) != key) return {};
    }
    if (bytes.empty() || token.cancelled()) return {};
    Apartment apartment;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICStream> input;
    check(factory->CreateStream(&input));
    check(input->InitializeFromMemory(bytes.data(), DWORD(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame));
        source = frame;
    } else {
        int w = 0, h = 0;
        if (!WebPGetInfo(bytes.data(), bytes.size(), &w, &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
            uint64_t(w) * h > 40000000)
            return {};
        std::unique_ptr<uint8_t, decltype(&WebPFree)> rgba(WebPDecodeBGRA(bytes.data(), bytes.size(), &w, &h), WebPFree);
        if (!rgba) return {};
        ComPtr<IWICBitmap> bitmap;
        check(factory->CreateBitmapFromMemory(UINT(w), UINT(h), GUID_WICPixelFormat32bppBGRA, UINT(w * 4), UINT(w * h * 4),
                                              rgba.get(), &bitmap));
        // WIC copies the pixels into the bitmap.
        source = bitmap;
    }
    UINT width = 0, height = 0;
    check(source->GetSize(&width, &height));
    if (!width || !height || width > 16384 || height > 16384 || uint64_t(width) * height > 40000000 || token.cancelled())
        return {};
    const double scale = std::min(1.0, double(pixels) / std::max(width, height));
    ComPtr<IWICBitmapScaler> scaler;
    check(factory->CreateBitmapScaler(&scaler));
    check(scaler->Initialize(source.Get(), std::max(1u, UINT(width * scale)), std::max(1u, UINT(height * scale)),
                             WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(scaler.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    const auto temporary = Path(target.wstring() + L".tmp");
    auto cleanup = wil::scope_exit([&] { std::filesystem::remove(temporary, ec); });
    {
        ComPtr<IWICStream> output;
        check(factory->CreateStream(&output));
        check(output->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
        check(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        check(encoder->CreateNewFrame(&frame, nullptr));
        check(frame->Initialize(nullptr));
        check(frame->WriteSource(converter.Get(), nullptr));
        check(frame->Commit());
        check(encoder->Commit());
    }
    if (token.cancelled()) return {};
    std::filesystem::rename(temporary, target);
    if (diskBytes_) *diskBytes_ += std::filesystem::file_size(target);
    trim();
    if (!std::filesystem::exists(target)) return {};
    return ArtworkReference{key, target};
}

void ArtworkCache::trim() {
    if (diskBytes_ && *diskBytes_ <= maxBytes_) return;
    std::vector<std::filesystem::directory_entry> files;
    uintmax_t bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".jpg") continue;
        files.push_back(entry);
        bytes += entry.file_size();
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
    for (const auto& entry : files) {
        if (bytes <= maxBytes_) break;
        const auto size = entry.file_size();
        std::error_code ec;
        if (std::filesystem::remove(entry.path(), ec)) bytes -= size;
    }
    diskBytes_ = bytes;
}
} // namespace camel::server
