// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A track's file details (docs/Task-breakdown.md FD-R01): a window of its own beside the main window, one at a time, with
// a section for each tag format and the technical data, names and values in columns of selectable text and Open folder
// beside the path.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Server/Api/ServerApi.h"

namespace ui {

class FileDetailsWindow {
public:
    using Read = std::function<camel::server::FileDetails()>;

    // Shows `title` at once and the details once `read`, run off the UI thread, returns them; the window shows the last
    // track asked for.
    void show(HWND owner, winrt::hstring const& title, winrt::Microsoft::UI::Xaml::ElementTheme theme, Read read);
    void setTheme(winrt::Microsoft::UI::Xaml::ElementTheme theme);
    void close();

private:
    struct Row {
        winrt::hstring name, value;
        std::optional<winrt::hstring> library;
    };
    struct Section {
        winrt::hstring heading;
        std::vector<Row> rows;
    };

    void create(HWND owner, winrt::Microsoft::UI::Xaml::ElementTheme theme);
    void colorCaptionButtons();
    void render();
    winrt::Microsoft::UI::Xaml::Controls::TextBlock text(winrt::hstring const& value);
    winrt::Microsoft::UI::Xaml::UIElement sectionView(const Section& section);
    void openFolder() const;

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel content_{nullptr};
    std::shared_ptr<uint64_t> generation_ = std::make_shared<uint64_t>(0);
    winrt::hstring title_;
    std::optional<camel::server::FileDetails> details_;
};

// Opens the details of the track at `pathUtf8`, titled `title`; the main window sets what opens them.
using FileDetailsOpener = std::function<void(const std::string& pathUtf8, winrt::hstring const& title)>;
void setFileDetailsOpener(FileDetailsOpener opener);
void openFileDetails(const std::string& pathUtf8, winrt::hstring const& title);
// Appends a separator and "File details", which calls `open`; returns both, so a menu can hide them for other rows.
std::vector<winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItemBase>
appendFileDetailsMenu(winrt::Microsoft::UI::Xaml::Controls::MenuFlyout const& menu, std::function<void()> open);

} // namespace ui
