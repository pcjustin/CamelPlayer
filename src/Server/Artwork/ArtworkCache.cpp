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
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <map>
#include <utility>
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
// Empty when cancelled, unreadable or over the size limit.
std::vector<uint8_t> readImage(const Path& file, const CancellationToken& token) {
    const auto size = std::filesystem::file_size(file);
    if (!size || size > kMaxArtworkBytes) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream input(file, std::ios::binary);
    for (size_t offset = 0; offset < bytes.size();) {
        if (token.cancelled()) return {};
        const auto count = std::min(size_t{64} * 1024, bytes.size() - offset);
        if (!input.read(reinterpret_cast<char*>(bytes.data() + offset), std::streamsize(count))) return {};
        offset += count;
    }
    return bytes;
}
std::optional<ArtworkCandidate> folderImage(const Path& path, const CancellationToken& token) {
    return findFolderArtworkFile(path, token);
}

// The first frame of a picture WIC reads, or a WebP; null when it is neither or too large. `bytes` must outlive it.
ComPtr<IWICBitmapSource> decode(IWICImagingFactory* factory, const std::vector<uint8_t>& bytes) {
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICStream> input;
    check(factory->CreateStream(&input));
    check(input->InitializeFromMemory(const_cast<uint8_t*>(bytes.data()), DWORD(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(factory->CreateDecoderFromStream(input.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame));
        source = frame;
    } else {
        int w = 0, h = 0;
        if (!WebPGetInfo(bytes.data(), bytes.size(), &w, &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
            uint64_t(w) * h > 40000000)
            return nullptr;
        std::unique_ptr<uint8_t, decltype(&WebPFree)> rgba(WebPDecodeBGRA(bytes.data(), bytes.size(), &w, &h), WebPFree);
        if (!rgba) return nullptr;
        ComPtr<IWICBitmap> bitmap;
        check(factory->CreateBitmapFromMemory(UINT(w), UINT(h), GUID_WICPixelFormat32bppBGRA, UINT(w * 4), UINT(w * h * 4),
                                              rgba.get(), &bitmap));
        // WIC copies the pixels into the bitmap.
        source = bitmap;
    }
    UINT width = 0, height = 0;
    check(source->GetSize(&width, &height));
    if (!width || !height || width > 16384 || height > 16384 || uint64_t(width) * height > 40000000) return nullptr;
    return source;
}

void encodeJpeg(IWICImagingFactory* factory, IWICBitmapSource* source, IWICStream* output) {
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(source, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    check(encoder->Initialize(output, WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->WriteSource(converter.Get(), nullptr));
    check(frame->Commit());
    check(encoder->Commit());
}

struct StoredCover {
    std::string key;
    bool embedded = false;
    Path file;
};

// Columns: source, path, size, mtime, volume_serial, file_index, created, volume_key.
StoredCover storedCover(const sqlite::Statement& row) {
    const auto source = row.text(0), path = row.text(1);
    const auto version = library::storedVersion(row, 2);
    Md5 md5;
    md5.update(source.data(), source.size());
    md5.update(path.data(), path.size());
    md5.update(&version.size, sizeof(version.size));
    md5.update(&version.mtime, sizeof(version.mtime));
    md5.update(&version.created, sizeof(version.created));
    md5.update(version.volumeKey.data(), version.volumeKey.size());
    md5.update(version.index.data(), version.index.size());
    return {"s" + toHex(md5.finish()), source == "embedded", Path(toWide(path))};
}

// The library's key for a playback path, worked out without opening it: lowercase, extended-length, a share as UNC.
std::string libraryKey(const Path& path) {
    std::wstring text = path.wstring();
    if (!text.empty()) // as the scanner lowercases its keys
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()), text.data(), int(text.size()),
                      nullptr, nullptr, 0);
    if (text.starts_with(LR"(\\)") && !text.starts_with(LR"(\\?\)"))
        text = LR"(\\?\unc\)" + text.substr(2);
    else if (text.size() > 1 && text[1] == L':')
        text = LR"(\\?\)" + text;
    return toUtf8(text);
}

std::optional<StoredCover> albumCover(sqlite::Database& db, int64_t album) {
    auto row = db.prepare("SELECT c.source,f.path,f.size,f.mtime,f.volume_serial,f.file_index,f.created,f.volume_key "
                          "FROM albums a JOIN covers c ON c.id=a.cover_id JOIN files f ON f.id=c.file_id "
                          "JOIN roots r ON r.id=f.root_id WHERE a.id=? AND f.available=1 AND r.online=1 AND r.removed IS NULL");
    row.bind(1, album);
    if (!row.step()) return std::nullopt;
    return storedCover(row);
}

// The folder's own image, chosen like the scan chooses album covers, or else its first album's cover.
std::optional<StoredCover> folderCover(sqlite::Database& db, int64_t folder) {
    auto images = db.prepare("SELECT 'folder',f.path,f.size,f.mtime,f.volume_serial,f.file_index,f.created,f.volume_key "
                             "FROM files f JOIN roots r ON r.id=f.root_id WHERE f.folder_id=? AND f.kind='image' "
                             "AND f.available=1 AND r.online=1 AND r.removed IS NULL");
    images.bind(1, folder);
    std::optional<StoredCover> best;
    size_t bestRank = SIZE_MAX;
    while (images.step()) {
        const auto rank = folderArtworkRank(Path(toWide(images.text(1))));
        if (rank && rank->first < bestRank && images.integer(2) > 0 && std::cmp_less_equal(images.integer(2), kMaxArtworkBytes)) {
            bestRank = rank->first;
            best = storedCover(images);
        }
    }
    if (best) return best;
    auto child =
        db.prepare("WITH RECURSIVE below(id) AS (SELECT ? UNION ALL SELECT d.id FROM folders d JOIN below b ON d.parent_id=b.id) "
                   "SELECT a.id FROM albums a JOIN below b ON b.id=a.folder_id WHERE a.cover_id IS NOT NULL ORDER BY "
                   "a.sort_key,a.id LIMIT 1");
    child.bind(1, folder);
    return child.step() ? albumCover(db, child.integer(0)) : std::nullopt;
}
} // namespace

std::vector<uint8_t> squarePicture(const std::vector<uint8_t>& image, unsigned pixels) {
    if (image.empty() || image.size() > kMaxArtworkBytes) throw std::invalid_argument("not a picture");
    Apartment apartment;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    const auto source = decode(factory.Get(), image);
    if (!source) throw std::invalid_argument("not a picture");
    UINT width = 0, height = 0;
    check(source->GetSize(&width, &height));
    const UINT side = std::min(width, height);
    const WICRect square{INT((width - side) / 2), height > width ? 0 : INT((height - side) / 2), INT(side), INT(side)};
    ComPtr<IWICBitmapClipper> clipper;
    check(factory->CreateBitmapClipper(&clipper));
    check(clipper->Initialize(source.Get(), &square));
    const UINT size = std::min(side, pixels);
    ComPtr<IWICBitmapScaler> scaler;
    check(factory->CreateBitmapScaler(&scaler));
    check(scaler->Initialize(clipper.Get(), size, size, WICBitmapInterpolationModeFant));
    ComPtr<IStream> memory;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &memory));
    ComPtr<IWICStream> output;
    check(factory->CreateStream(&output));
    check(output->InitializeFromIStream(memory.Get()));
    encodeJpeg(factory.Get(), scaler.Get(), output.Get());
    STATSTG stat{};
    check(memory->Stat(&stat, STATFLAG_NONAME));
    std::vector<uint8_t> jpeg(size_t(stat.cbSize.QuadPart));
    check(memory->Seek({}, STREAM_SEEK_SET, nullptr));
    ULONG read = 0;
    check(memory->Read(jpeg.data(), ULONG(jpeg.size()), &read));
    jpeg.resize(read);
    return jpeg;
}

ArtworkCache::ArtworkCache(Path directory, uintmax_t maxBytes) : directory_(std::move(directory)), maxBytes_(maxBytes) {}

std::optional<ArtworkReference> ArtworkCache::get(const LibraryStore* store, const ArtworkRequest& request,
                                                  const CancellationToken& token) {
    if (token.cancelled()) return {};
    if (request.kind == ArtworkKind::Artist) return store ? artist(*store, request.name, token) : std::nullopt;
    if (store) {
        try {
            if (request.kind != ArtworkKind::Track) {
                if (auto ref = stored(*store, request, token)) return ref;
            } else {
                // A library track shows its album's cover, so the playback bar reads nothing from the storage either.
                auto db = store->openReader();
                auto row = db.prepare("SELECT t.album_id FROM tracks t JOIN files f ON f.id=t.file_id "
                                      "WHERE f.path=? AND f.available=1 AND t.album_id IS NOT NULL LIMIT 1");
                if (row.bind(1, libraryKey(Path(toWide(request.path)))).step())
                    if (auto ref = stored(*store, {ArtworkKind::Album, row.integer(0), {}}, token)) return ref;
            }
        } catch (const std::exception&) {
        }
    }
    std::lock_guard lock(mutex_);
    BackgroundPriority priority;
    auto use = [&](const std::optional<ArtworkCandidate>& candidate) -> std::optional<ArtworkReference> {
        if (!candidate || token.cancelled()) return {};
        try {
            return generate(*candidate, token);
        } catch (const std::exception&) {
            return {};
        }
    };
    auto track = [&](const Path& path) -> std::optional<ArtworkReference> {
        try {
            const auto key = fileKey(path);
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
            if (token.cancelled() || fileKey(path) != key) return {};
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

// Album and folder covers on a NAS were looked up again on every request: the folder listed, the image file opened and
// embedded art read from the track, one request at a time behind the cache lock (docs/Plan.md §12.1).
std::optional<ArtworkReference> ArtworkCache::stored(const LibraryStore& store, const ArtworkRequest& request,
                                                     const CancellationToken& token) {
    auto db = store.openReader();
    const auto cover = request.kind == ArtworkKind::Album ? albumCover(db, request.id) : folderCover(db, request.id);
    if (!cover) return std::nullopt;
    const auto key = cover->key + "-" + std::to_string(kPixels);
    const auto target = directory_ / (key + ".jpg");
    std::error_code ec;
    if (std::filesystem::is_regular_file(target, ec) && std::filesystem::file_size(target, ec) > 0) {
        std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), ec);
        return ArtworkReference{key, target};
    }
    BackgroundPriority priority;
    if (token.cancelled()) return std::nullopt;
    // The source is read before taking the lock, so the covers of several albums are read from a NAS at once.
    ArtworkCandidate image;
    if (cover->embedded) {
        ArtworkQuery query;
        query.firstTrack = cover->file;
        auto images = makeEmbeddedArtworkProvider()->find(query, token);
        if (images.empty()) return std::nullopt;
        image = std::move(images.front());
    } else {
        image.file = cover->file;
        image.bytes = readImage(cover->file, token);
        if (image.bytes.empty()) return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    return generate(image, token, cover->key);
}

// The picture the library keeps for a name; its key follows the picture's last change.
std::optional<ArtworkReference> ArtworkCache::artist(const LibraryStore& store, const std::string& name,
                                                     const CancellationToken& token) {
    try {
        auto db = store.openReader();
        auto row = db.prepare("SELECT changed FROM artist_pictures WHERE name=? AND (chosen IS NOT NULL OR found IS NOT NULL)");
        if (!row.bind(1, name).step()) return std::nullopt;
        const auto changed = row.integer(0);
        Md5 md5;
        md5.update(name.data(), name.size());
        md5.update(&changed, sizeof(changed));
        const auto key = "p" + toHex(md5.finish());
        const auto target = directory_ / (key + "-" + std::to_string(kPixels) + ".jpg");
        std::error_code ec;
        if (std::filesystem::is_regular_file(target, ec) && std::filesystem::file_size(target, ec) > 0) {
            std::filesystem::last_write_time(target, std::filesystem::file_time_type::clock::now(), ec);
            return ArtworkReference{key + "-" + std::to_string(kPixels), target};
        }
        auto picture = db.prepare("SELECT coalesce(chosen,found) FROM artist_pictures WHERE name=?");
        if (!picture.bind(1, name).step() || picture.isNull(0)) return std::nullopt;
        ArtworkCandidate image;
        image.bytes = picture.blob(0);
        std::lock_guard lock(mutex_);
        return generate(image, token, key);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

size_t ArtworkCache::prepare(const LibraryStore& store, const std::function<bool()>& yield, const CancellationToken& token) {
    std::vector<int64_t> albums;
    {
        auto db = store.openReader();
        auto rows = db.prepare("SELECT id FROM albums WHERE cover_id IS NOT NULL ORDER BY sort_key,id");
        while (rows.step()) albums.push_back(rows.integer(0));
    }
    if (yield && yield()) return 0;
    // Four covers at a time: on a NAS each mostly waits for the disk. When `yield` asks for the storage (playback, a
    // scan, a cover being shown), the reads in flight stop at their next chunk.
    auto reading = token.child();
    std::atomic<size_t> next = 0, made = 0;
    constexpr int kWorkers = 4;
    std::vector<std::future<void>> workers;
    workers.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i)
        workers.push_back(std::async(std::launch::async, [&] {
            for (size_t at; !reading.cancelled() && (at = next++) < albums.size();) {
                auto db = store.openReader();
                const auto cover = albumCover(db, albums[at]);
                std::error_code ec;
                if (!cover || std::filesystem::exists(directory_ / (cover->key + "-" + std::to_string(kPixels) + ".jpg"), ec))
                    continue;
                try {
                    if (stored(store, {ArtworkKind::Album, albums[at], {}}, reading)) ++made;
                } catch (const std::exception&) {
                }
            }
        }));
    for (auto& worker : workers)
        while (worker.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            if (yield && yield()) reading.cancel();
    return made;
}

std::optional<ArtworkReference> ArtworkCache::thumbnail(const ArtworkCandidate& image, const CancellationToken& token) {
    std::lock_guard lock(mutex_);
    try {
        return generate(image, token);
    } catch (const std::exception&) {
        return {};
    }
}

std::optional<ArtworkReference> ArtworkCache::generate(const ArtworkCandidate& image, const CancellationToken& token,
                                                       const std::string& stableKey) {
    if (token.cancelled() || image.bytes.size() > kMaxArtworkBytes) return {};
    const auto key = (stableKey.empty() ? sourceKey(image) : stableKey) + "-" + std::to_string(kPixels);
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
        bytes = readImage(image.file, token);
        if (bytes.empty()) return {};
        if (stableKey.empty() && sourceKey(image) + "-" + std::to_string(kPixels) != key) return {};
    }
    if (bytes.empty() || token.cancelled()) return {};
    Apartment apartment;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    const auto source = decode(factory.Get(), bytes);
    if (!source || token.cancelled()) return {};
    UINT width = 0, height = 0;
    check(source->GetSize(&width, &height));
    const double scale = std::min(1.0, double(kPixels) / std::max(width, height));
    ComPtr<IWICBitmapScaler> scaler;
    check(factory->CreateBitmapScaler(&scaler));
    check(scaler->Initialize(source.Get(), std::max(1u, UINT(width * scale)), std::max(1u, UINT(height * scale)),
                             WICBitmapInterpolationModeFant));
    const auto temporary = Path(target.wstring() + L".tmp");
    auto cleanup = wil::scope_exit([&] { std::filesystem::remove(temporary, ec); });
    {
        ComPtr<IWICStream> output;
        check(factory->CreateStream(&output));
        check(output->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE));
        encodeJpeg(factory.Get(), scaler.Get(), output.Get());
    }
    if (token.cancelled()) return {};
    std::filesystem::rename(temporary, target);
    if (diskBytes_) *diskBytes_ += std::filesystem::file_size(target);
    trim();
    if (!std::filesystem::exists(target)) return {};
    return ArtworkReference{key, target};
}

void ArtworkCache::clear() {
    std::lock_guard lock(mutex_);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error))
        std::filesystem::remove_all(entry.path(), error); // a thumbnail being read stays until its reader closes it
    embedded_.clear();
    diskBytes_.reset();
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
