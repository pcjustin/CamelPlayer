// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <list>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_set>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Server/Api/ServerApi.h"

namespace ui {
namespace detail {
// The thumbnails already decoded, newest first, used on the UI thread; their keys can be asked from any thread, so a
// cover already decoded is not read from its file again.
struct DecodedCovers {
    struct Entry {
        std::string key;
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap;
    };
    std::list<Entry> entries;
    std::mutex mutex;
    std::unordered_set<std::string> keys;

    bool has(const std::string& key) {
        std::scoped_lock lock(mutex);
        return keys.contains(key);
    }
    // On the UI thread: the decoded cover for `key`, moved to the front, or null.
    winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage find(const std::string& key) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key; });
        if (found == entries.end()) return nullptr;
        entries.splice(entries.begin(), entries, found);
        return found->bitmap;
    }
    void add(const std::string& key, winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& bitmap) {
        entries.remove_if([&](const Entry& entry) { return entry.key == key; });
        entries.push_front({key, bitmap});
        std::scoped_lock lock(mutex);
        keys.insert(key);
        size_t bytes = 0;
        for (auto it = entries.begin(); it != entries.end();) {
            bytes += size_t(camel::server::ArtworkCache::kPixels) * camel::server::ArtworkCache::kPixels * 4;
            if (bytes > (32ull << 20)) {
                keys.erase(it->key);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }
};
inline DecodedCovers& decodedCovers() {
    static DecodedCovers covers;
    return covers;
}
} // namespace detail

// Called on the UI thread; only generated cache files reach XAML. The file is read into memory off the UI thread:
// opening each thumbnail through StorageFile on the UI thread stalled fast album-wall scrolling for up to 1 s, mostly
// in the kernel's file-system path (docs/Plan.md §12.1).
inline winrt::fire_and_forget loadArtwork(winrt::Microsoft::UI::Xaml::Controls::Image image,
                                          std::shared_ptr<camel::server::IServer> server, camel::server::ArtworkRequest request,
                                          camel::server::CancellationToken token) {
    using namespace winrt;
    using namespace Microsoft::UI::Xaml::Media::Imaging;
    auto weak = make_weak(image);
    auto context = apartment_context();
    image.Source(nullptr);
    image = nullptr;
    try {
        co_await resume_background();
        const auto ref = server->artwork(request, token);
        if (!ref || token.cancelled()) co_return;
        auto& covers = detail::decodedCovers();
        if (covers.has(ref->key)) {
            co_await context;
            if (token.cancelled() || !weak.get()) co_return;
            if (const auto bitmap = covers.find(ref->key)) {
                if (auto target = weak.get()) target.Source(bitmap);
                co_return;
            }
            co_await resume_background(); // dropped from the decoded ones meanwhile: read like any other
        }
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        {
            std::ifstream in(ref->path, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
            if (bytes.empty()) co_return;
            Windows::Storage::Streams::DataWriter writer(stream);
            writer.WriteBytes(bytes);
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
        }
        co_await context;
        if (token.cancelled() || !weak.get()) co_return;
        if (const auto bitmap = covers.find(ref->key)) {
            if (auto target = weak.get()) target.Source(bitmap);
            co_return;
        }
        BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(stream);
        if (token.cancelled()) co_return;
        if (auto target = weak.get()) target.Source(bitmap);
        covers.add(ref->key, bitmap);
    } catch (...) {
    }
}
} // namespace ui
