// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "FileDetailsWindow.h"

#include <shellapi.h>
#include <shlobj.h>
#include <winrt/Microsoft.UI.Interop.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "ThemeBrush.h"
#include "UiText.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using camel::server::FileDetails;

namespace ui {

namespace {

std::wstring wide(const std::string& text) {
    return std::wstring(utf8(text));
}

// The field's name in the interface's language with the tag's own key, or the key alone when the field has no name.
hstring fieldName(const std::string& field, const std::string& key, bool withKey = true) {
    std::wstring name;
    if (!field.empty()) {
        const auto colon = field.find(':');
        const std::wstring resource = L"FileField_" + wide(field.substr(0, colon));
        if (const auto text = tr(resource.c_str()); text != resource) {
            name = text;
            if (colon != std::string::npos) name += L" \u00B7 " + wide(field.substr(colon + 1));
        }
    }
    if (name.empty()) return utf8(key);
    return hstring(withKey ? name + L" (" + wide(key) + L")" : name);
}

hstring joined(const std::vector<std::string>& values, const wchar_t* separator) {
    std::wstring out;
    for (const auto& value : values) {
        if (!out.empty()) out += separator;
        out += wide(value);
    }
    return hstring(out);
}

hstring printed(const wchar_t* pattern, double value) {
    wchar_t text[64];
    swprintf_s(text, pattern, value);
    return text;
}

hstring sampleRate(const FileDetails& d) {
    if (d.format == "DSD") {
        wchar_t text[64];
        swprintf_s(text, L"%.4g MHz (DSD%u)", d.sampleRate / 1e6, d.sampleRate / 44100);
        return text;
    }
    return printed(L"%.4g kHz", d.sampleRate / 1000.0);
}

hstring fileSize(uint64_t bytes) {
    const double mb = double(bytes) / (1024.0 * 1024.0);
    const auto amount = mb >= 1 ? printed(L"%.1f MB", mb) : printed(L"%.1f KB", double(bytes) / 1024.0);
    return amount + L" (" + format(L"FileDetails_Bytes", {count(int64_t(bytes))}) + L")";
}

hstring localTime(int64_t unixSeconds) {
    const uint64_t ticks = uint64_t(unixSeconds + 11'644'473'600LL) * 10'000'000ULL;
    const FILETIME utc{DWORD(ticks & 0xFFFFFFFF), DWORD(ticks >> 32)};
    SYSTEMTIME universal{}, local{};
    if (!FileTimeToSystemTime(&utc, &universal) || !SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local)) return {};
    wchar_t text[32];
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
               local.wSecond);
    return text;
}

void addGain(std::vector<std::pair<hstring, hstring>>& rows, hstring const& name, const std::optional<double>& value, bool peak) {
    if (value) rows.emplace_back(name, peak ? printed(L"%.6f", *value) : printed(L"%+.2f dB", *value));
}

FileDetailsOpener& opener() {
    static FileDetailsOpener current;
    return current;
}

} // namespace

void setFileDetailsOpener(FileDetailsOpener open) {
    opener() = std::move(open);
}

void openFileDetails(const std::string& pathUtf8, hstring const& title) {
    if (opener() && !pathUtf8.empty()) opener()(pathUtf8, title);
}

std::vector<MenuFlyoutItemBase> appendFileDetailsMenu(MenuFlyout const& menu, std::function<void()> open) {
    MenuFlyoutItem entry;
    entry.Text(tr(L"Library_MenuFileDetails"));
    FontIcon icon;
    icon.Glyph(L"\xE946");
    entry.Icon(icon);
    entry.KeyboardAcceleratorTextOverride(L"Alt+Enter"); // the main window opens the focused row's details
    entry.Click([open = std::move(open)](auto&&, auto&&) { open(); });
    std::vector<MenuFlyoutItemBase> added{MenuFlyoutSeparator(), entry};
    for (const auto& item : added) menu.Items().Append(item);
    return added;
}

void FileDetailsWindow::show(HWND owner, hstring const& title, ElementTheme theme, Read read) {
    if (!window_) create(owner, theme);
    title_ = title;
    details_.reset();
    render();
    window_.Activate();
    const uint64_t generation = ++*generation_;
    const std::weak_ptr<uint64_t> alive = generation_;
    auto queue = window_.DispatcherQueue();
    std::thread([this, alive, generation, queue, read = std::move(read)] {
        FileDetails details;
        try {
            details = read();
        } catch (const std::exception& e) {
            details.problem = FileDetails::Problem::Unreadable;
            details.reason = e.what();
        }
        queue.TryEnqueue([this, alive, generation, details = std::move(details)]() mutable {
            const auto current = alive.lock();
            if (!current || *current != generation || !window_) return;
            details_ = std::move(details);
            render();
        });
    }).detach();
}

void FileDetailsWindow::setTheme(ElementTheme theme) {
    if (root_ && root_.RequestedTheme() != theme) root_.RequestedTheme(theme);
}

void FileDetailsWindow::close() {
    if (window_) window_.Close();
}

void FileDetailsWindow::create(HWND owner, ElementTheme theme) {
    window_ = Window();
    window_.Title(tr(L"FileDetails_Title"));
    window_.ExtendsContentIntoTitleBar(true);
    root_ = Grid();
    root_.RequestedTheme(theme);
    root_.RowDefinitions().Append(RowDefinition());
    root_.RowDefinitions().GetAt(0).Height(GridLengthHelper::Auto());
    root_.RowDefinitions().Append(RowDefinition());
    TitleBar bar;
    bar.Title(tr(L"FileDetails_Title"));
    bar.IsTabStop(false);
    root_.Children().Append(bar);
    ScrollViewer scroller;
    Grid::SetRow(scroller, 1);
    content_ = StackPanel();
    content_.Padding({24, 4, 24, 24});
    content_.Spacing(12);
    scroller.Content(content_);
    root_.Children().Append(scroller);
    window_.Content(root_);
    window_.SetTitleBar(bar);
    if (Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported())
        window_.SystemBackdrop(Media::MicaBackdrop());
    else
        root_.Background(themeBrush(L"SolidBackgroundFillColorBaseBrush"));

    const auto app = window_.AppWindow();
    app.TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto icon = std::filesystem::path(executable).parent_path() / L"assets" / L"CamelPlayer.ico";
    if (std::error_code error; std::filesystem::exists(icon, error)) app.SetIcon(icon.c_str());
    // Owned by the main window, so it stays above it and goes with it when it is minimized.
    const HWND own = Microsoft::UI::GetWindowFromWindowId(app.Id());
    SetWindowLongPtrW(own, GWLP_HWNDPARENT, LONG_PTR(owner));
    const double scale = GetDpiForWindow(owner) / 96.0;
    RECT main{};
    GetWindowRect(owner, &main);
    const auto area = Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(
                          Microsoft::UI::GetWindowIdFromWindow(owner), Microsoft::UI::Windowing::DisplayAreaFallback::Nearest)
                          .WorkArea();
    const int margin = int(16 * scale);
    const int width = std::min(int(680 * scale), area.Width - 2 * margin);
    const int height = std::min(int(860 * scale), area.Height - 2 * margin);
    // Beside the main window where there is room, else over its right side.
    int x = area.X + area.Width - main.right >= width ? int(main.right)
                                                      : std::min(int(main.right), area.X + area.Width) - width - margin;
    x = std::clamp(x, area.X, area.X + area.Width - width);
    const int y = std::clamp(int(main.top) + int(64 * scale), area.Y, area.Y + area.Height - height);
    app.MoveAndResize({x, y, width, height});

    colorCaptionButtons();
    root_.ActualThemeChanged([this](auto&&, auto&&) {
        colorCaptionButtons();
        render();
    });
    window_.Closed([this](auto&&, auto&&) {
        ++*generation_;
        window_ = nullptr;
        root_ = nullptr;
        content_ = nullptr;
        details_.reset();
    });
}

void FileDetailsWindow::colorCaptionButtons() {
    const bool dark = root_.ActualTheme() == ElementTheme::Dark;
    const auto bar = window_.AppWindow().TitleBar();
    const Windows::UI::Color foreground = dark ? Windows::UI::Color{255, 255, 255, 255} : Windows::UI::Color{255, 0, 0, 0};
    const Windows::UI::Color transparent{0, 0, 0, 0};
    bar.ButtonBackgroundColor(transparent);
    bar.ButtonInactiveBackgroundColor(transparent);
    bar.ButtonForegroundColor(foreground);
    bar.ButtonHoverForegroundColor(foreground);
    bar.ButtonPressedForegroundColor(foreground);
    bar.ButtonInactiveForegroundColor(dark ? Windows::UI::Color{255, 140, 140, 140} : Windows::UI::Color{255, 120, 120, 120});
    bar.ButtonHoverBackgroundColor(dark ? Windows::UI::Color{24, 255, 255, 255} : Windows::UI::Color{24, 0, 0, 0});
    bar.ButtonPressedBackgroundColor(dark ? Windows::UI::Color{16, 255, 255, 255} : Windows::UI::Color{16, 0, 0, 0});
}

TextBlock FileDetailsWindow::text(hstring const& value) {
    TextBlock block;
    block.Text(value);
    block.IsTextSelectionEnabled(true);
    block.TextWrapping(TextWrapping::Wrap);
    return block;
}

UIElement FileDetailsWindow::sectionView(const Section& section) {
    StackPanel panel;
    panel.Spacing(6);
    auto heading = text(section.heading);
    heading.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
    panel.Children().Append(heading);
    Border card;
    card.Background(themeBrush(L"CardBackgroundFillColorDefaultBrush"));
    card.BorderBrush(themeBrush(L"CardStrokeColorDefaultBrush"));
    card.BorderThickness({1, 1, 1, 1});
    card.CornerRadius({8, 8, 8, 8});
    card.Padding({16, 12, 16, 12});
    Grid grid;
    grid.ColumnSpacing(16);
    grid.RowSpacing(8);
    ColumnDefinition names, values;
    names.Width({2, GridUnitType::Star});
    values.Width({3, GridUnitType::Star});
    grid.ColumnDefinitions().Append(names);
    grid.ColumnDefinitions().Append(values);
    int32_t index = 0;
    for (const auto& row : section.rows) {
        grid.RowDefinitions().Append(RowDefinition());
        grid.RowDefinitions().GetAt(uint32_t(index)).Height(GridLengthHelper::Auto());
        auto name = text(row.name);
        name.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        Grid::SetRow(name, index);
        grid.Children().Append(name);
        StackPanel cell;
        cell.Spacing(2);
        cell.Children().Append(text(row.value));
        if (row.library) {
            Grid mark;
            mark.ColumnSpacing(6);
            mark.ColumnDefinitions().Append(ColumnDefinition());
            mark.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::Auto());
            mark.ColumnDefinitions().Append(ColumnDefinition());
            FontIcon icon;
            icon.Glyph(L"\xE7BA");
            icon.FontSize(12);
            icon.VerticalAlignment(VerticalAlignment::Top);
            icon.Margin({0, 4, 0, 0});
            icon.Foreground(themeBrush(L"SystemFillColorCautionBrush"));
            mark.Children().Append(icon);
            auto library = text(format(L"FileDetails_InLibrary", {std::wstring(*row.library)}));
            library.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
            Grid::SetColumn(library, 1);
            mark.Children().Append(library);
            cell.Children().Append(mark);
        }
        Grid::SetRow(cell, index);
        Grid::SetColumn(cell, 1);
        grid.Children().Append(cell);
        ++index;
    }
    card.Child(grid);
    panel.Children().Append(card);
    return panel;
}

void FileDetailsWindow::render() {
    if (!content_) return;
    content_.Children().Clear();
    auto title = text(title_);
    title.Style(Application::Current().Resources().Lookup(box_value(L"SubtitleTextBlockStyle")).as<Style>());
    content_.Children().Append(title);
    if (!details_) {
        StackPanel reading;
        reading.Orientation(Orientation::Horizontal);
        reading.Spacing(12);
        ProgressRing ring;
        ring.Width(20);
        ring.Height(20);
        reading.Children().Append(ring);
        reading.Children().Append(text(tr(L"FileDetails_Reading")));
        content_.Children().Append(reading);
        return;
    }
    const auto& d = *details_;
    std::vector<Section> sections;
    Grid path;
    path.ColumnSpacing(12);
    path.ColumnDefinitions().Append(ColumnDefinition());
    path.ColumnDefinitions().Append(ColumnDefinition());
    path.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());
    auto pathText = text(utf8(d.path));
    pathText.VerticalAlignment(VerticalAlignment::Center);
    path.Children().Append(pathText);
    Button open;
    StackPanel label;
    label.Orientation(Orientation::Horizontal);
    label.Spacing(8);
    FontIcon folder;
    folder.Glyph(L"\xE838");
    folder.FontSize(16);
    label.Children().Append(folder);
    TextBlock openText;
    openText.Text(tr(L"FileDetails_OpenFolder"));
    label.Children().Append(openText);
    open.Content(label);
    Automation::AutomationProperties::SetName(open, tr(L"FileDetails_OpenFolder"));
    open.IsEnabled(d.folderPresent);
    open.VerticalAlignment(VerticalAlignment::Top);
    open.Click([this](auto&&, auto&&) { openFolder(); });
    Grid::SetColumn(open, 1);
    path.Children().Append(open);
    content_.Children().Append(path);
    if (d.problem != FileDetails::Problem::None) {
        InfoBar problem;
        problem.Severity(InfoBarSeverity::Warning);
        problem.IsClosable(false);
        problem.IsOpen(true);
        problem.Message(d.problem == FileDetails::Problem::Missing ? tr(L"FileDetails_Missing")
                                                                   : format(L"FileDetails_Unreadable", {wide(d.reason)}));
        content_.Children().Append(problem);
    }

    for (const auto& tags : d.sections) {
        Section section{utf8(tags.format), {}};
        for (const auto& tag : tags.tags) {
            std::optional<hstring> library;
            if (tag.library) library = tag.library->empty() ? tr(L"FileDetails_None") : utf8(*tag.library);
            section.rows.push_back({fieldName(tag.field, tag.key), joined(tag.values, L"\n"), library});
        }
        sections.push_back(std::move(section));
    }
    if (!d.library.empty()) {
        Section section{tr(L"FileDetails_Library"), {}};
        for (const auto& tag : d.library)
            section.rows.push_back({fieldName(tag.field, tag.key, false), joined(tag.values, L"\n"), std::nullopt});
        sections.push_back(std::move(section));
    }
    std::vector<std::pair<hstring, hstring>> technical;
    if (!d.format.empty()) technical.emplace_back(tr(L"FileDetails_Format"), utf8(d.format));
    if (d.sampleRate) technical.emplace_back(tr(L"FileDetails_SampleRate"), sampleRate(d));
    if (d.bitsPerSample) technical.emplace_back(tr(L"FileDetails_BitDepth"), hstring(std::to_wstring(d.bitsPerSample) + L"-bit"));
    if (d.channels) technical.emplace_back(tr(L"FileDetails_Channels"), hstring(std::to_wstring(d.channels)));
    if (d.lengthMs) technical.emplace_back(tr(L"FileDetails_Length"), hstring(formatTime(double(d.lengthMs) / 1000.0)));
    if (d.frames) technical.emplace_back(tr(L"FileDetails_Frames"), hstring(count(int64_t(d.frames))));
    if (d.bitrateKbps) technical.emplace_back(tr(L"FileDetails_Bitrate"), hstring(count(d.bitrateKbps) + L" kbps"));
    if (d.size) technical.emplace_back(tr(L"FileDetails_Size"), fileSize(*d.size));
    if (d.modified) technical.emplace_back(tr(L"FileDetails_Modified"), localTime(*d.modified));
    addGain(technical, tr(L"FileField_REPLAYGAIN_TRACK_GAIN"), d.trackGain, false);
    addGain(technical, tr(L"FileField_REPLAYGAIN_TRACK_PEAK"), d.trackPeak, true);
    addGain(technical, tr(L"FileField_REPLAYGAIN_ALBUM_GAIN"), d.albumGain, false);
    addGain(technical, tr(L"FileField_REPLAYGAIN_ALBUM_PEAK"), d.albumPeak, true);
    addGain(technical, tr(L"FileDetails_AnalysisTrackGain"), d.analysisTrackGain, false);
    addGain(technical, tr(L"FileDetails_AnalysisTrackPeak"), d.analysisTrackPeak, true);
    addGain(technical, tr(L"FileDetails_AnalysisAlbumGain"), d.analysisAlbumGain, false);
    addGain(technical, tr(L"FileDetails_AnalysisAlbumPeak"), d.analysisAlbumPeak, true);
    if (!technical.empty()) {
        Section section{tr(L"FileDetails_Technical"), {}};
        for (auto& [name, value] : technical) section.rows.push_back({name, value, std::nullopt});
        sections.push_back(std::move(section));
    }
    if (d.problem == FileDetails::Problem::None && d.sections.empty())
        content_.Children().Append(text(tr(L"FileDetails_NoTags")));
    for (const auto& section : sections) content_.Children().Append(sectionView(section));
}

// File Explorer opens the folder with the file selected, or the folder alone once the file is gone.
void FileDetailsWindow::openFolder() const {
    if (!details_) return;
    const std::filesystem::path file(wide(details_->path));
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str())) {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
        return;
    }
    ShellExecuteW(nullptr, L"open", file.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace ui
