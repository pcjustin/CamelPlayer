// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <list>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Server/Api/ServerApi.h"

namespace ui {
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
        if (!ref || token.cancelled() || !weak.get()) co_return;
        struct Entry {
            std::string key;
            BitmapImage bitmap;
            unsigned pixels;
        };
        static std::list<Entry> cache;
        auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& e) { return e.key == ref->key; });
        if (found != cache.end()) {
            auto bitmap = found->bitmap;
            cache.splice(cache.begin(), cache, found);
            if (auto target = weak.get()) target.Source(bitmap);
            co_return;
        }
        BitmapImage bitmap;
        co_await bitmap.SetSourceAsync(stream);
        if (token.cancelled()) co_return;
        if (auto target = weak.get()) target.Source(bitmap);
        cache.remove_if([&](const auto& entry) { return entry.key == ref->key; });
        cache.push_front({ref->key, bitmap, request.pixels});
        size_t bytes = 0;
        for (auto it = cache.begin(); it != cache.end();) {
            bytes += size_t(it->pixels) * it->pixels * 4;
            if (bytes > (32ull << 20))
                it = cache.erase(it);
            else
                ++it;
        }
    } catch (...) {
    }
}
} // namespace ui
