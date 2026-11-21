// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>

#pragma comment(lib, "propsys.lib")
#include <utility>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "CreditText.h"
#include "AppIcon.h"
#include "DialogPlacement.h"
#include "FileDetailsWindow.h"
#include "CommandSubmit.h"
#include "OutputsFollow.h"
#include "SnapshotForward.h"
#include "ThemeBrush.h"
#include "CaptionButtons.h"
#include "FilePicker.h"
#include "UiText.h"
#include "TrackColumns.h"
#include "TrimmedText.h"
#include "ArtworkLoader.h"
#include "Common/settings/Settings.h"
#include "Common/util/FolderWalk.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/StartWithWindows.h"
#include "Common/util/Text.h"
#include "Common/Version.h"
#include "TrayIcon.h"
#include "HomeView.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

namespace mainwindow {

// The resource key naming a DSP node.
const wchar_t* dspNodeKey(DspNode node) {
    switch (node) {
    case DspNode::Eq:
        return L"Dsp_NodeEq";
    case DspNode::Crossfeed:
        return L"Dsp_NodeCrossfeed";
    default:
        return L"Dsp_NodeConvolution";
    }
}

// A dialog showing release notes or a changelog section as plain, selectable text.
ContentDialog textDialog(XamlRoot const& root, hstring const& title, hstring const& text) {
    ContentDialog dialog;
    ui::placeDialog(dialog, root);
    dialog.Title(box_value(title));
    TextBlock body;
    body.Text(text);
    body.TextWrapping(TextWrapping::Wrap);
    body.IsTextSelectionEnabled(true);
    ScrollViewer scroll;
    scroll.MaxHeight(420);
    scroll.Content(body);
    dialog.Content(scroll);
    dialog.CloseButtonText(tr(L"Update_Close"));
    dialog.DefaultButton(ContentDialogButton::Close);
    return dialog;
}

void fill(ComboBox const& combo, std::vector<hstring> const& items, int selected) {
    combo.Items().Clear();
    for (auto& s : items) combo.Items().Append(box_value(s));
    combo.SelectedIndex(items.empty() ? -1 : std::clamp(selected, 0, int(items.size()) - 1));
}

// LiveSetting alone announces nothing; Narrator hears a change only when the live region raises it.
void setAnnounced(TextBlock const& block, hstring const& value) {
    if (block.Text() == value) return;
    block.Text(value);
    if (const auto peer = Automation::Peers::FrameworkElementAutomationPeer::FromElement(block))
        peer.RaiseAutomationEvent(Automation::Peers::AutomationEvents::LiveRegionChanged);
}

void setBadgeTip(FrameworkElement const& badge, hstring const& name, hstring const& tip) {
    Automation::AutomationProperties::SetName(badge, name);
    ToolTipService::SetToolTip(badge, tip.empty() ? nullptr : box_value(tip));
}

hstring decibels(double db) {
    wchar_t text[32];
    swprintf_s(text, L"%.1f dB", db);
    return text;
}

} // namespace mainwindow

namespace {

// Output details arrive as one line of ", " and "; " separated clauses; the status area shows one per line.
hstring clauseLines(const std::string& detail) {
    std::wstring text(ui::utf8(detail));
    for (const wchar_t* separator : {L"; ", L", "})
        for (size_t at = text.find(separator); at != std::wstring::npos; at = text.find(separator, at + 1))
            text.replace(at, 2, L"\n");
    return hstring(text);
}

// A long settings note shows its first sentence and a link that shows the rest, and then hides it again.
void foldNote(TextBlock const& note) {
    const std::wstring whole(note.Text());
    size_t end = whole.find(L'。');
    end = std::min(end, whole.find(L". "));
    if (end == std::wstring::npos || end + 2 >= whole.size()) return;
    Documents::Run text;
    Documents::Run label;
    Documents::Hyperlink link;
    link.Inlines().Append(label);
    note.Inlines().Clear();
    note.Inlines().Append(text);
    note.Inlines().Append(link);
    const auto show = [text, label, first = whole.substr(0, end + 1), whole](bool open) {
        text.Text(hstring((open ? whole : first) + L" "));
        label.Text(tr(open ? L"Settings_LessInfo" : L"Settings_MoreInfo"));
    };
    show(false);
    link.Click([show, open = false](auto&&, auto&&) mutable { show(open = !open); });
}

// WinUI shows a box's text note on one line, cut off at the box's edge; as a text block of its own it wraps.
void wrapNotes(UIElement const& element) {
    const auto wrap = [](IInspectable const& description) -> IInspectable {
        const hstring text = unbox_value_or<hstring>(description, {});
        if (text.empty()) return description;
        TextBlock note;
        note.Text(text);
        note.TextWrapping(TextWrapping::Wrap);
        return note;
    };
    if (const auto box = element.try_as<ComboBox>())
        box.Description(wrap(box.Description()));
    else if (const auto number = element.try_as<NumberBox>())
        number.Description(wrap(number.Description()));
    else if (const auto panel = element.try_as<Panel>())
        for (const auto& child : panel.Children()) wrapNotes(child);
    else if (const auto border = element.try_as<Border>()) {
        if (border.Child()) wrapNotes(border.Child());
    } else if (const auto content = element.try_as<ContentControl>()) {
        if (const auto inner = content.Content().try_as<UIElement>()) wrapNotes(inner);
    }
}

const wchar_t* bypassTip(const camel::audio::OutputPlan& plan) {
    return plan.remodulate ? L"Badge_DspBypassedRemodulatedTip" : L"Badge_DspBypassedTip";
}

// Folders, with a Windows library expanded to the folders it holds now (later library changes are not followed).
std::vector<std::wstring> pickMusicFolders(HWND owner) {
    std::vector<std::wstring> folders;
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_ALLNONSTORAGEITEMS | FOS_ALLOWMULTISELECT);
    winrt::com_ptr<IShellItem> music;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_MusicLibrary, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(music.put()))))
        dialog->SetFolder(music.get());
    if (FAILED(dialog->Show(owner))) return folders;
    winrt::com_ptr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(items.put()))) return folders;
    const auto add = [&](IShellItem* folder) {
        PWSTR path = nullptr;
        if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            folders.emplace_back(path);
            CoTaskMemFree(path);
        }
    };
    const auto forEach = [](IShellItemArray* array, auto&& visit) {
        DWORD count = 0;
        array->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            winrt::com_ptr<IShellItem> item;
            if (SUCCEEDED(array->GetItemAt(i, item.put()))) visit(item.get());
        }
    };
    forEach(items.get(), [&](IShellItem* item) {
        winrt::com_ptr<IShellLibrary> library;
        winrt::com_ptr<IShellItemArray> members;
        if (SUCCEEDED(SHLoadLibraryFromItem(item, STGM_READ, IID_PPV_ARGS(library.put()))) &&
            SUCCEEDED(library->GetFolders(LFF_FORCEFILESYSTEM, IID_PPV_ARGS(members.put()))))
            forEach(members.get(), add);
        else
            add(item);
    });
    return folders;
}

} // namespace

void MainWindow::InitializeComponent() {
    MainWindowT<MainWindow>::InitializeComponent();
    AppWindow().Resize({1180, 780});
    if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
        const double scale = ::GetDpiForWindow(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id())) / 96.0;
        presenter.PreferredMinimumWidth(static_cast<int32_t>(860 * scale));
        presenter.PreferredMinimumHeight(static_cast<int32_t>(600 * scale));
        presenter.Maximize(); // starts maximized; restoring returns to the size above
    }
    ui::setAppIcon(AppWindow());
    std::wstring executablePath(32768, L'\0');
    const DWORD executablePathLength =
        ::GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (executablePathLength > 0 && executablePathLength < executablePath.size()) {
        executablePath.resize(executablePathLength);
        const auto iconPath = std::filesystem::path(executablePath).parent_path() / L"assets" / L"CamelPlayer.ico";
        std::error_code error;
        if (std::filesystem::exists(iconPath, error)) {
            ImageIconSource icon;
            icon.ImageSource(Media::Imaging::BitmapImage(Windows::Foundation::Uri(iconPath.wstring())));
            AppTitleBar().IconSource(icon);
        }
    }
    if (!Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported())
        ThemeBackground().Visibility(Visibility::Visible);
    // Column widths dragged in any track list are kept in the settings once the drag ends.
    ui::trackColumns().Committed([weak = get_weak()](auto&&, auto&&) {
        auto self = weak.get();
        if (!self || self->settings_.trackColumns == ui::trackColumns().columns()) return;
        self->settings_.trackColumns = ui::trackColumns().columns();
        self->applySettingsNow();
    });
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());
    AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
    colorCaptionButtons();
    Content().as<FrameworkElement>().ActualThemeChanged([this](auto&&, auto&&) {
        colorCaptionButtons();
        repaintTheme();
        fileDetails_.setTheme(Content().as<FrameworkElement>().ActualTheme());
    });
    ui::setFileDetailsOpener([this](const std::string& path, hstring const& title) { showFileDetails(path, title); });
    get_self<LibraryView>(Library())->setBackChanged([this](bool) { showTitleBack(); });
    Library().RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) { showTitleBack(); });
    addSettingsLinks();
    SettingsScroller().SizeChanged([this](auto&&, auto&&) { markSettingsSection(); });
    waveformBar_ = std::make_unique<ui::WaveformBar>(WaveformLayer(), PositionSlider());
    // Resting the pointer on the position slider shows the time there.
    positionTip_ = ToolTip();
    positionTip_.Placement(Controls::Primitives::PlacementMode::Mouse);
    ToolTipService::SetToolTip(PositionSlider(), positionTip_);
    eqEditor_ = std::make_unique<ui::EqEditor>(EqCurve(), EqBands(), EqAddBandButton(),
                                               [this](const camel::EqSettings&, bool final) { eqChanged(final); });
    // The acrylic over the lyrics' cover keeps what it last saw beneath it, so a new cover is shown by hiding it a moment.
    LyricsBackdrop().RegisterPropertyChangedCallback(Image::SourceProperty(), [this](auto&&, auto&&) {
        if (!LyricsBackdrop().Source()) return;
        LyricsBackdrop().Visibility(Visibility::Collapsed);
        LyricsAcrylic().Visibility(Visibility::Collapsed);
        DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()] {
            if (const auto self = weak.get()) {
                self->LyricsBackdrop().Visibility(Visibility::Visible);
                self->LyricsAcrylic().Visibility(Visibility::Visible);
            }
        });
    });
    // Scrolling the lyrics by hand stops them following the music for a few seconds.
    const auto scrolledByHand = [this] {
        lyricsScrolledAt_ = std::chrono::steady_clock::now();
        lyricsCentered_ = false;
    };
    LyricsScroller().AddHandler(UIElement::PointerWheelChangedEvent(),
                                box_value(Input::PointerEventHandler([scrolledByHand](auto&&, auto&&) { scrolledByHand(); })),
                                true);
    LyricsScroller().DirectManipulationStarted([scrolledByHand](auto&&, auto&&) { scrolledByHand(); });
    Content().AddHandler(UIElement::PointerPressedEvent(),
                         box_value(Input::PointerEventHandler([this](auto&&, Input::PointerRoutedEventArgs const& e) {
                             if (e.GetCurrentPoint(nullptr).Properties().IsXButton1Pressed() && goBack()) e.Handled(true);
                         })),
                         true);

    if (!server_) server_ = createLocalServer();
    settings_ = server_->snapshot().settings;
    server_->setDiscordTexts({winrt::to_string(tr(L"Lamp_BitPerfect")), winrt::to_string(tr(L"Lamp_Lossless")),
                              winrt::to_string(tr(L"Lamp_Processed")), winrt::to_string(tr(L"Lamp_Lossy"))});
    auto dq = DispatcherQueue();
    // The Server reports the position with every engine poll, 20 times a second; the 250 ms refresh shows it, so an event
    // that moves only the position is left to it, and to nothing while the window is minimized or hidden. It is not even
    // sent to the UI thread; one the window then finds moving only the position (against what it shows) is dropped.
    server_->setListener([dq, weak = get_weak(), forward = std::make_shared<ui::SnapshotForward>()](const Event& event) {
        if (!forward->forward(event)) return;
        dq.TryEnqueue([weak, kind = event.kind, snapshot = event.snapshot] {
            if (auto self = weak.get(); self && (kind == EventKind::CommandCompleted || !self->movesOnlyThePosition(snapshot)))
                self->applySnapshot(snapshot);
        });
    });
    using namespace std::chrono_literals;
    uiTimer_ = dq.CreateTimer();
    uiTimer_.Interval(250ms);
    uiTimer_.Tick([this](auto&&, auto&&) { refreshFromServer(); });
    uiTimer_.Start();
    eqTimer_ = dq.CreateTimer();
    eqTimer_.Interval(100ms);
    eqTimer_.IsRepeating(false);
    eqTimer_.Tick([this](auto&&, auto&&) { settingsChanged(); });
    seekTimer_ = dq.CreateTimer();
    seekTimer_.Interval(200ms);
    seekTimer_.IsRepeating(false);
    seekTimer_.Tick([this](auto&&, auto&&) {
        if (!server_) return;
        const auto s = server_->snapshot().playback;
        if (s.source) {
            Command command;
            command.kind = CommandKind::Seek;
            command.frame = secondsToFrames(pendingSeek_, s.source->sampleRate);
            submitCommand(std::move(command));
        }
    });

    const auto setButtonTip = [](const auto& button, const wchar_t* key) {
        ToolTipService::SetToolTip(button, box_value(tr(key)));
        Automation::AutomationProperties::SetName(button, tr(key));
    };
    setButtonTip(PlayPauseButton(), L"Tip_Play");
    StopMenuItem().Text(tr(L"Tip_Stop"));
    setButtonTip(PreviousButton(), L"Tip_Previous");
    setButtonTip(NextButton(), L"Tip_Next");
    setButtonTip(ShuffleButton(), L"Tip_Shuffle");
    setButtonTip(RepeatButton(), L"Tip_RepeatOff");
    setButtonTip(QueueToggleButton(), L"Tip_Queue");
    for (const TextBlock& note :
         {VolumeNote(), ReplayGainNote(), CrossfadeNote(), DspOrderNote(), EqNote(), CrossfeedNote(), ConvolutionNote()})
        foldNote(note);
    wrapNotes(SettingsScroller().Content().as<UIElement>());

    populateSettings();
    const ui::PlaylistMenu playlistMenu = [this](MenuFlyout const& menu, const std::function<ui::SourceFetch()>& selection) {
        extendPlaylistMenu(menu, selection);
    };
    get_self<LibraryView>(Library())->attach(
        server_, [this](Command command) { submitCommand(std::move(command)); }, playlistMenu,
        [this](ui::SourceFetch fetch, std::string name) { runPlaylistAction(std::move(fetch), std::nullopt, std::move(name)); },
        [this] { return Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()); },
        [this](bool allow) {
            settings_.artistPictures = allow ? camel::OnlineLookup::Allowed : camel::OnlineLookup::Refused;
            applySettingsNow();
        },
        [this] { showSelectionChanged(); });
    get_self<PlaylistView>(Playlists())
        ->attach(server_,
                 {[this](Command command) { submitCommand(std::move(command)); }, [this] { refreshPlaylists(); },
                  [this](int64_t playlist) {
                      // A playlist just imported is listed in the navigation once it is refreshed.
                      const auto tag = playlist ? L"playlist:" + to_hstring(playlist) : hstring(L"playlists");
                      refreshPlaylists([this, tag] { selectPage(tag); });
                  },
                  [this](hstring const& title, hstring const& message, bool error) { showPlaylistNotice(title, message, error); },
                  [this] { return Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()); }, playlistMenu});
    noticeTimer_ = DispatcherQueue().CreateTimer();
    noticeTimer_.Interval(std::chrono::seconds(10));
    noticeTimer_.Tick([this](auto&&, auto&&) {
        noticeTimer_.Stop();
        PlaylistNotice().IsOpen(false);
    });
    PlaylistNotice().Closed([this](auto&&, auto&&) { noticeUndo_.reset(); });
    refreshPlaylists();
    // Like "View all", an album or artist opened from Home or Listening keeps Albums unselected and Back returns to
    // that page.
    const auto leaveHome = [this] {
        Nav().SelectedItem(nullptr);
        Home().Visibility(Visibility::Collapsed);
        Library().Visibility(Visibility::Visible);
        return [this, tag = shownTag_] { returnToPage(tag); };
    };
    get_self<HomeView>(Home())->attach(
        server_,
        {[this, leaveHome](int64_t album, hstring const& title) {
             get_self<LibraryView>(Library())->showAlbum(album, title, leaveHome());
         },
         [this, leaveHome](hstring const& artist) { get_self<LibraryView>(Library())->showArtist(artist, leaveHome()); },
         [this](hstring const& list, hstring const& title, ui::BrowseFetch fetch) { viewAll(list, title, std::move(fetch)); },
         [this](Command command) { submitCommand(std::move(command)); }, [this] { showPage(L"listening"); }});
    std::ifstream queueState(camel::dataDirectory() / L"queue-panel-open");
    queueState >> queueOpen_;
    QueueList().ItemsSource(*queueItems_);
    updateQueuePanel();
    Nav().SizeChanged([this](auto&&, auto&&) { fitNavigationPane(); });
    if (camel::measure::measuring()) {
        camel::log::info("measure: main window built %.0f ms after process start", camel::measure::sinceProcessStart());
        Media::CompositionTarget::Rendering([this](auto&&, auto&&) { recordFrame(); });
    }
    ContentLayout().Loaded([this](auto&&, auto&&) {
        if (camel::measure::measuring())
            camel::log::info("measure: main window loaded %.0f ms after process start", camel::measure::sinceProcessStart());
        repaintTheme(); // what was painted before the window had a theme
        fitStatus();
        if (openInterfaceCard_) showSettingsCard(InterfaceCard());
        showFirstRunGuideIfDue(); // a dialog needs a XamlRoot
        showUpdateResult();
        showDataRecovery();
        checkInstallation(false);
    });
    Nav().SelectedItem(Nav().MenuItems().GetAt(0));
    Command refresh;
    refresh.kind = CommandKind::RefreshDevices;
    submitCommand(std::move(refresh));

    // Handlers run after the tray window's message, since Exit destroys the tray icon.
    const auto open = [this] {
        DispatcherQueue().TryEnqueue([weak = get_weak()] {
            if (auto self = weak.get()) self->showFromTray();
        });
    };
    const auto exit = [this] {
        DispatcherQueue().TryEnqueue([weak = get_weak()] {
            if (auto self = weak.get()) {
                self->exiting_ = true;
                self->Close();
            }
        });
    };
    const auto dark = [this] { return Content().as<FrameworkElement>().ActualTheme() == ElementTheme::Dark; };
    tray_ = std::make_unique<ui::TrayIcon>(ui::TrayIcon::Handlers{open, exit, dark}, std::wstring(tr(L"Tray_Open")),
                                           std::wstring(tr(L"Tray_Exit")));
    // The window carries the app's AUMID too: the media session takes its app from the window it was created for.
    if (winrt::com_ptr<IPropertyStore> properties; SUCCEEDED(SHGetPropertyStoreForWindow(
            Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), IID_PPV_ARGS(properties.put())))) {
        PROPVARIANT id;
        if (SUCCEEDED(InitPropVariantFromString(L"CamelPlayer.CamelPlayer", &id))) {
            properties->SetValue(PKEY_AppUserModel_ID, id);
            PropVariantClear(&id);
        }
    }
    if (server_) {
        media_ = std::make_unique<ui::MediaControls>(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), DispatcherQueue(),
                                                     ui::MediaControls::Handlers{[this](CommandKind kind) { submitKind(kind); },
                                                                                 [this](double seconds) {
                                                                                     const auto s = server_->snapshot().playback;
                                                                                     if (!s.source) return;
                                                                                     Command command;
                                                                                     command.kind = CommandKind::Seek;
                                                                                     command.frame = secondsToFrames(
                                                                                         seconds, s.source->sampleRate);
                                                                                     submitCommand(std::move(command));
                                                                                 }},
                                                     server_);
        hotkeys_ = std::make_unique<ui::GlobalHotkeys>([this](ui::GlobalHotkeys::Action action) {
            using Action = ui::GlobalHotkeys::Action;
            submitKind(action == Action::PlayPause  ? CommandKind::TogglePause
                       : action == Action::Previous ? CommandKind::Previous
                                                    : CommandKind::Next);
        });
        applyHotkeys();
    }
    updateTimer_ = DispatcherQueue().CreateTimer();
    updateTimer_.Interval(std::chrono::seconds(10)); // the startup check waits until startup work has settled
    updateTimer_.Tick([this](auto&&, auto&&) {
        updateTimer_.Interval(std::chrono::hours(24));
        if (settings_.checkUpdates) checkForUpdates(false);
    });
    updateTimer_.Start();
    AppWindow().Changed([this](auto&&, auto&&) {
        const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        const bool minimized = presenter && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized;
        if (minimized == minimized_ || inTray_) return;
        minimized_ = minimized;
        setRefreshing(!minimized);
    });
    // Closing the bar brings no new snapshot, so the dot that keeps a playback error is shown here.
    ErrorBar().Closed(
        [this](auto&&, auto&&) { ErrorDot().Visibility(shown_.playback.error ? Visibility::Visible : Visibility::Collapsed); });
    AppWindow().Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& e) {
        if (exiting_ || settings_.closeAction == CloseAction::Exit) return;
        e.Cancel(true);
        hideToTray();
    });

    Closed([this](auto&&, auto&&) {
        fileDetails_.close(); // the app ends once its last window closes
        tray_.reset();
        artworkToken_.cancel();
        updateToken_.cancel();
        // A timer that ticked on would reach the closed window's elements, which throw.
        updateTimer_.Stop();
        seekTimer_.Stop();
        eqTimer_.Stop();
        noticeTimer_.Stop();
        setRefreshing(false); // the pages' timers and the UI timer
        if (replaced_) return;
        server_->setListener({});
        Command pause; // silences the device first; a stop would also forget the position the next start restores
        pause.kind = CommandKind::Pause;
        submitCommand(std::move(pause));
        server_.reset();
    });
    refreshFromServer();
}

void MainWindow::hideToTray() {
    inTray_ = true;
    AppWindow().Hide();
    setRefreshing(false);
    if (server_ && server_->snapshot().playback.state == PlaybackState::Playing) {
        Command pause;
        pause.kind = CommandKind::Pause;
        submitCommand(std::move(pause));
    }
    if (!settings_.trayNoticeShown && server_) {
        tray_->notify(std::wstring(tr(L"Tray_NoticeTitle")), std::wstring(tr(L"Tray_NoticeText")));
        settings_.trayNoticeShown = true;
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = settings_;
        submitCommand(std::move(command));
    }
}

// The switch follows Windows' own record, which its startup settings may have turned off; CamelPlayer then leaves it.
void MainWindow::showStartWithWindows() {
    const auto state = camel::startup::state();
    const bool wasPopulating = std::exchange(populating_, true);
    StartWithWindowsSwitch().IsOn(state.on());
    StartWithWindowsSwitch().IsEnabled(!state.disabledInWindows);
    StartWithWindowsNote().Visibility(state.disabledInWindows ? Visibility::Visible : Visibility::Collapsed);
    populating_ = wasPopulating;
}

void MainWindow::OnStartWithWindowsToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    wchar_t program[MAX_PATH];
    const bool written = StartWithWindowsSwitch().IsOn() && GetModuleFileNameW(nullptr, program, MAX_PATH)
                             ? camel::startup::enable(program)
                             : camel::startup::disable();
    showStartWithWindows();
    // The switch springs back to what the Run key holds; this says why.
    StartWithWindowsError().Visibility(written ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::startInTray() {
    inTray_ = true;
    AppWindow().Hide(); // maximizing at construction showed it, before anything was drawn
    setRefreshing(false);
}

void MainWindow::showFromTray() {
    const bool wasInTray = inTray_;
    inTray_ = false;
    AppWindow().Show();
    if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized)
        presenter.Restore();
    Activate();
    SetForegroundWindow(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()));
    if (wasInTray) setRefreshing(true);
}

void MainWindow::setRefreshing(bool refreshing) {
    get_self<LibraryView>(Library())->setActive(refreshing);
    get_self<HomeView>(Home())->setActive(refreshing);
    if (!refreshing) {
        uiTimer_.Stop();
        return;
    }
    uiTimer_.Start();
    refreshFromServer();
}

void MainWindow::refreshFromServer() {
    if (!server_) return;
    applySnapshot(server_->snapshot());
    showScanProgress();
    showLoudnessProgress();
    showDiscordStatus();
}

void MainWindow::showScanProgress() {
    LibraryScanProgress progress;
    try {
        progress = server_->libraryScanProgress();
    } catch (const std::exception&) {
        return; // no library configured
    }
    // A scan already running leaves Scan now unavailable until it ends.
    ScanNowButton().IsEnabled(progress.scans == 0);
    std::wstring text;
    if (progress.scans && progress.finishing == progress.scans) {
        text = tr(L"Library_Finishing");
    } else if (progress.scans) {
        text = ui::format(L"Library_Scanning", {ui::count(progress.checked), ui::count(progress.found)});
    }
    if (text == shownScan_) return;
    shownScan_ = hstring(text);
    ScanText().Text(shownScan_);
    ScanText().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::submitCommand(Command command) {
    if (!server_) return;
    if (command.kind == CommandKind::QueueReplace || command.kind == CommandKind::OpenAndPlay) {
        const TrackSource* chosen = command.source                           ? &*command.source
                                    : command.index < command.sources.size() ? &command.sources[command.index]
                                                                             : nullptr;
        if (repeatedPlay_.repeated(
                {command.kind, command.index, command.sources.size(), chosen ? chosen->pathUtf8() : std::string{}},
                std::chrono::steady_clock::now(), std::chrono::milliseconds(GetDoubleClickTime())))
            return;
    } else {
        repeatedPlay_.other();
    }
    if (const auto stamp = ui::submitChecked(*server_, std::move(command))) cursor_.expect(*stamp);
}

bool MainWindow::movesOnlyThePosition(const Snapshot& snapshot) const {
    return ui::movesOnlyThePosition(snapshot, shown_);
}

// A device scan can change what the Server keeps: the outputs, and the parameters when the Windows default moved on.
void MainWindow::showOutputsOf(const SettingsDto& server) {
    switch (ui::followServerOutputs(settings_, server, editingOutput_.has_value())) {
    case ui::OutputsRefill::Settings:
        populateSettings();
        break;
    case ui::OutputsRefill::Outputs:
        populateOutputs();
        break;
    case ui::OutputsRefill::None:
        break;
    }
}

void MainWindow::applySnapshot(const Snapshot& snapshot) {
    if (snapshot.playback.playerVolume != playerVolume_ || snapshot.playback.playerMuted != playerMuted_ ||
        snapshot.playback.playerVolumeDb != playerVolumeDb_) {
        playerVolume_ = snapshot.playback.playerVolume;
        playerVolumeDb_ = snapshot.playback.playerVolumeDb;
        playerMuted_ = snapshot.playback.playerMuted;
        showVolume();
    }
    // The receiver waits while it opens for the command just sent, whose snapshot the cursor waits for.
    if (snapshot.playback.receiverAsking != shownAsking_) {
        shownAsking_ = snapshot.playback.receiverAsking;
        if (!shownAsking_.empty())
            showReceiverAsking();
        else if (ErrorBar().Severity() == InfoBarSeverity::Informational)
            ErrorBar().IsOpen(false);
    }
    if (!server_ || !cursor_.accept(snapshot)) return;
    // While a track plays, most snapshots differ from the one shown in its position alone: only what shows the
    // position is redrawn for them, not the whole bar and the signal path.
    auto moved = shown_;
    moved.revision = snapshot.revision;
    moved.playback.position = snapshot.playback.position;
    const bool onlyMoved = moved == snapshot;
    shown_ = snapshot;
    if (onlyMoved) {
        const auto [pos, total] = showPosition(snapshot.playback);
        if (media_) media_->showPosition(pos, total);
        return;
    }
    if (snapshot.devices.revision != shownDevicesRevision_ || snapshot.settings.airplayGroup != settings_.airplayGroup ||
        snapshot.settings.airplay.deviceId != settings_.airplay.deviceId) {
        shownDevicesRevision_ = snapshot.devices.revision;
        showOutputsOf(snapshot.settings);
    }
    if (snapshot.playback.receivers != receivers_) {
        receivers_ = snapshot.playback.receivers;
        showReceiverVolumes();
    }
    if (const auto& left = snapshot.playback.receiverLeft; left && left->serial != shownReceiverLeft_) {
        shownReceiverLeft_ = left->serial;
        const hstring why = ui::playerErrorTitle(left->code, left->detail);
        ReceiverBar().Title(ui::format(L"Output_ReceiverLeft", {std::wstring(ui::utf8(left->name))}));
        ReceiverBar().Message(why.empty() ? ui::errorTitle(left->code) : why);
        ReceiverBar().IsOpen(true);
    }
    const auto& s = snapshot.playback;
    if (artworkPath_ != s.pathUtf8) {
        artworkPath_ = s.pathUtf8;
        artworkToken_.cancel();
        artworkToken_ = {};
        for (const auto& image : {PlaybackCover(), LyricsBackdrop(), LyricsNoneCover()}) {
            image.Source(nullptr);
            if (!artworkPath_.empty()) ui::loadArtwork(image, server_, {ArtworkKind::Track, 0, artworkPath_}, artworkToken_);
        }
    }
    if (s.pathUtf8 != lyricsPath_) loadLyrics(s.pathUtf8);

    // The play button and the repeat button are named for what they do or show now. As in Roon, the play button alone
    // shows whether it plays: a ring turns around it while a slow source is buffered, and a dot on it stays for an error
    // once its bar is closed; its tip says how long playback waits, or what failed.
    const bool buffering = !s.error && s.bufferingSeconds;
    const Symbol symbol = s.state == PlaybackState::Playing || s.bufferingSeconds ? Symbol::Pause : Symbol::Play;
    if (PlayPauseIcon().Symbol() != symbol) PlayPauseIcon().Symbol(symbol);
    const hstring action = tr(symbol == Symbol::Pause ? L"Tip_Pause" : L"Tip_Play");
    const hstring playTip = s.error     ? ui::errorTitle(*s.error)
                            : buffering ? ui::format(L"State_Buffering", {std::to_wstring(*s.bufferingSeconds)})
                                        : action;
    if (action != shownPlayName_ || playTip != shownPlayTip_) {
        shownPlayName_ = action;
        shownPlayTip_ = playTip;
        setBadgeTip(PlayPauseButton(), action, playTip);
        // The tip shows only under the mouse; what it says beyond the action reaches a screen reader with the button.
        Automation::AutomationProperties::SetHelpText(PlayPauseButton(), playTip == action ? hstring{} : playTip);
    }
    // What the play button shows, said as it changes, as the state badge did.
    setAnnounced(PlaybackStateText(), s.error || buffering                ? playTip
                                      : s.state == PlaybackState::Playing ? tr(L"State_Playing")
                                      : s.state == PlaybackState::Paused  ? tr(L"State_Paused")
                                                                          : tr(L"State_Stopped"));
    BufferingRing().IsActive(buffering);
    // The waveform appears once the Server has it: made at once for the playing track, asked for every 2 seconds. Stopped,
    // there is none: the plain slider shows until a track plays or pauses again.
    if (const std::string shown = s.state == PlaybackState::Stopped ? std::string() : s.pathUtf8; shown != waveformPath_) {
        waveformPath_ = shown;
        waveformBar_->clear();
        waveformAsked_ = {};
    }
    const auto [pos, total] = showPosition(s);
    const bool hasTrack = !s.pathUtf8.empty();
    PlayPauseButton().IsEnabled(hasTrack);
    PreviousButton().IsEnabled(hasTrack);
    NextButton().IsEnabled(hasTrack);
    CoverButton().IsEnabled(hasTrack);
    PositionSlider().IsEnabled(hasTrack);
    ShuffleButton().IsChecked(snapshot.queue.shuffle);
    RepeatButton().IsChecked(snapshot.queue.repeat != RepeatMode::Off);
    if (snapshot.queue.repeat != shownRepeat_) {
        shownRepeat_ = snapshot.queue.repeat;
        RepeatIcon().Symbol(shownRepeat_ == RepeatMode::One ? Symbol::RepeatOne : Symbol::RepeatAll);
        const hstring tip = tr(shownRepeat_ == RepeatMode::Off   ? L"Tip_RepeatOff"
                               : shownRepeat_ == RepeatMode::One ? L"Tip_RepeatOne"
                                                                 : L"Tip_RepeatAll");
        setBadgeTip(RepeatButton(), tip, tip);
    }
    if (camel::measure::measuring() && !queueShownMeasured_ && !snapshot.queue.entries.empty()) {
        queueShownMeasured_ = true;
        camel::log::info("measure: queue of %zu entries shown %.0f ms after process start", snapshot.queue.entries.size(),
                         camel::measure::sinceProcessStart());
    }
    if (shownQueueRevision_ != snapshot.queue.revision || shownQueueMarks_ != snapshot.queue.marks) {
        shownQueueRevision_ = snapshot.queue.revision;
        shownQueueMarks_ = snapshot.queue.marks;
        shownQueueEntryId_ = snapshot.queueEntryId;
        const bool windowed = snapshot.queue.entries.size() > ui::QueueCollection::kShown;
        QueueCountText().Text(
            ui::format(windowed ? L"Queue_CountWindowed" : L"Library_Tracks",
                       {ui::count(int64_t(snapshot.queue.entries.size())), ui::count(int64_t(ui::QueueCollection::kShown))}));
        QueueEmptyText().Visibility(snapshot.queue.entries.empty() ? Visibility::Visible : Visibility::Collapsed);
        queueItems_->update(snapshot.queue.entries, snapshot.queueEntryId);
    } else if (shownQueueEntryId_ != snapshot.queueEntryId) {
        shownQueueEntryId_ = snapshot.queueEntryId;
        queueItems_->setCurrent(snapshot.queueEntryId);
    }

    const auto currentEntry = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                           [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    // Library and playlist rows mark the track the queue is on, until playback stops.
    const bool onTrack = s.state != PlaybackState::Stopped && currentEntry != snapshot.queue.entries.end();
    // The playing track's album and the folders on its path are marked too; the folders are read once per track.
    const int64_t playingFile = onTrack ? currentEntry->file : 0;
    std::vector<int64_t> playingFolders =
        playingFile == ui::nowPlaying().file ? ui::nowPlaying().folders : std::vector<int64_t>{};
    if (const auto was = ui::setNowPlaying(
            {playingFile, s.state == PlaybackState::Paused, onTrack ? currentEntry->albumId : 0, std::move(playingFolders)})) {
        showNowPlaying(*was);
        if (playingFile != 0 && playingFile != was->file) readPlayingFolders(playingFile);
    }
    if (s.pathUtf8.empty()) {
        TitleText().Text(tr(L"NoFile"));
        showPlayerArtist({});
        AlbumLinkText().Text(L"");
        AlbumLink().Visibility(Visibility::Collapsed);
    } else {
        const std::filesystem::path p(toWide(s.pathUtf8));
        TitleText().Text(currentEntry != snapshot.queue.entries.end() && !currentEntry->title.empty()
                             ? ui::utf8(currentEntry->title)
                             : hstring(p.stem().wstring()));
        showPlayerArtist(currentEntry == snapshot.queue.entries.end() ? hstring{} : ui::utf8(currentEntry->artist));
        AlbumLinkText().Text(currentEntry == snapshot.queue.entries.end() ? hstring{} : ui::utf8(currentEntry->album));
        AlbumLink().Visibility(currentEntry != snapshot.queue.entries.end() && !currentEntry->album.empty()
                                   ? Visibility::Visible
                                   : Visibility::Collapsed);
    }

    const bool marked = !s.pathUtf8.empty() && currentEntry != snapshot.queue.entries.end() && currentEntry->file;
    PlaybackFavoriteButton().IsEnabled(marked);
    playbackFile_ = marked ? currentEntry->file : 0;
    showPlaybackFavorite(marked && currentEntry->favorite);

    if (media_) {
        const bool known = currentEntry != snapshot.queue.entries.end();
        const std::string title = known && !currentEntry->title.empty()
                                      ? currentEntry->title
                                      : toUtf8(std::filesystem::path(toWide(s.pathUtf8)).stem().wstring());
        media_->update(s, title, known ? currentEntry->artist : std::string{}, known ? currentEntry->album : std::string{}, pos,
                       total);
    }

    // Bit-perfect indicator and signal path (docs/Plan.md §5.2, §16.2)
    bool dspActive = false;
    const hstring decoder = ui::utf8(decoderName(s.decoder));
    if (s.output && s.source) {
        // A lossy source has no PCM depth of its own; its decoded output is passed on unchanged (docs/Plan.md §5.2).
        // Processing outranks the lossy-source badge, which must not hide it.
        const bool lossy = s.format && isLossy(*s.format);
        const OutputPlan plan = s.plan.value_or(OutputPlan{*s.output});
        switch (plan.state) {
        case PipelineState::BitPerfect:
            lampColor_ = L"SystemFillColorSuccessBrush";
            // HQPlayer processes what it is sent: only CamelPlayer's part is bit-perfect (docs/Plan.md §5.11).
            setAnnounced(LampText(), tr(s.backend == Backend::HQPlayer ? L"Lamp_BitPerfectToHQPlayer" : L"Lamp_BitPerfect"));
            break;
        case PipelineState::LosslessConversion:
            lampColor_ = L"SystemFillColorCautionBrush";
            setAnnounced(LampText(), tr(L"Lamp_Lossless"));
            break;
        case PipelineState::Processed:
            lampColor_ = L"SystemFillColorCriticalBrush";
            setAnnounced(LampText(), tr(L"Lamp_Processed"));
            break;
        case PipelineState::LossySource:
            lampColor_ = L"SystemFillColorNeutralBrush";
            setAnnounced(LampText(), tr(L"Lamp_Lossy"));
            break;
        }
        // Source lossiness is its own field; a processed lossy file shows both (docs/Plan.md §5.2).
        SourceBadge().Visibility(lossy && plan.state != PipelineState::LossySource ? Visibility::Visible : Visibility::Collapsed);
        // DSD the modulator makes has passed the DSP chain.
        const bool raw = (plan.dsd == DsdPath::Dop || plan.dsd == DsdPath::Native) && !plan.modulatorOrder;
        BypassBadge().Visibility(raw ? Visibility::Visible : Visibility::Collapsed);
        BypassText().Text((plan.dsd == DsdPath::Dop ? hstring(L"DoP") : hstring(L"DSD")) + L" · " +
                          tr(L"SignalPath_DspBypassed"));
        setBadgeTip(BypassBadge(), BypassText().Text(), tr(bypassTip(plan)));
        const hstring source = ui::utf8(s.format ? formatName(*s.format) : "") +
                               (plan.dsd != DsdPath::None ? L""
                                : lossy                   ? L" → " + to_hstring(s.source->bitsPerSample) + L"-bit PCM"
                                                          : L" " + to_hstring(s.source->bitsPerSample) + L"-bit");
        PathRouteText().Text(source + L" → " + ui::utf8(s.outputMode) + L" · " + ui::utf8(describe(*s.output)));
        // One row per step that changes or carries the signal, in the Engine's order (docs/Plan.md §8.1).
        std::vector<PathStep> steps;
        PathStage stage = PathStage::Source;
        const auto add = [&](hstring name, hstring detail, PathState state) {
            steps.push_back({std::move(name), std::move(detail), state, stage});
        };
        wchar_t text[128];
        const uint32_t bits = s.source->bitsPerSample;
        if (s.format == AudioFormat::Dsd) // the decoder counts byte frames of eight DSD samples
            swprintf_s(text, L"DSD%u · %.4f MHz · %u ch", s.source->sampleRate * 8 / 44100, s.source->sampleRate * 8 / 1e6,
                       s.source->channels);
        else if (lossy)
            swprintf_s(text, L"%s · %.1f kHz · %u ch", ui::utf8(formatName(*s.format)).c_str(), s.source->sampleRate / 1000.0,
                       s.source->channels);
        else
            swprintf_s(text, L"%s · %.1f kHz · %u-bit · %u ch", ui::utf8(s.format ? formatName(*s.format) : "").c_str(),
                       s.source->sampleRate / 1000.0, bits, s.source->channels);
        const PathState sourceState = lossy ? PathState::Lossy : PathState::BitPerfect;
        add(tr(L"SignalPath_File"), text, sourceState);
        // The WAVE decoders read a DTS-CD bitstream that the Rust DTS core then decodes.
        const hstring reader = s.format == AudioFormat::Dts ? decoder + L" → DTS core" : decoder;
        add(tr(L"SignalPath_Decoder"), lossy ? reader + L" · " + to_hstring(bits) + L"-bit PCM" : reader, sourceState);
        stage = PathStage::Processing;
        if (plan.duplicate) add(tr(L"SignalPath_Duplicate"), L"2 ch", PathState::BitPerfect);
        if (plan.downmix) add(tr(L"SignalPath_Downmix"), to_hstring(s.source->channels) + L"→2 ch", PathState::Processed);
        const bool decimated = plan.dsd == DsdPath::Pcm || (plan.modulatorOrder && bits == 1);
        if (decimated) add(tr(L"SignalPath_DsdToPcm"), L"352.8 kHz", PathState::Processed);
        if (plan.remodulate) {
            const uint32_t outBytes = plan.dsd == DsdPath::Dop ? s.output->sampleRate * 2 : s.output->sampleRate;
            swprintf_s(text, L"DSD%u → DSD%u", s.source->sampleRate * 8 / 44100, outBytes * 8 / 44100);
            add(tr(L"SignalPath_DsdRemodulate"), text, PathState::Processed);
        }
        // The decimator always produces 352.8 kHz; soxr then takes it to the output rate, or to the 352.8 kHz the
        // modulator starts from.
        const double from = decimated ? 352.8 : s.source->sampleRate / 1000.0;
        const double to = plan.modulatorOrder ? 352.8 : s.output->sampleRate / 1000.0;
        if (plan.resample && from != to) {
            swprintf_s(text, L"%.1f→%.1f kHz", from, to);
            add(tr(L"SignalPath_Resample"), text, PathState::Processed);
        }
        // The track's ReplayGain, the chain's nodes in order, then the volume; raw DSD bypasses them all.
        if (!raw) {
            if (plan.gainDb != 0.0) {
                swprintf_s(text, L"%+.1f dB", plan.gainDb);
                add(tr(L"SignalPath_ReplayGain"), text, PathState::Processed);
            }
            for (const auto node : plan.dsp)
                add(tr(node == DspNode::Eq          ? L"SignalPath_Eq"
                       : node == DspNode::Crossfeed ? L"SignalPath_Crossfeed"
                                                    : L"SignalPath_Convolution"),
                    {}, PathState::Processed);
            if (plan.muted) {
                add(tr(L"SignalPath_Muted"), {}, PathState::Processed);
            } else if (plan.volumeDb != 0.0) {
                swprintf_s(text, L"%.1f dB", plan.volumeDb);
                add(tr(L"SignalPath_Volume"), text, PathState::Processed);
            }
            // Only track changes are mixed; between them the path is as shown.
            if (plan.crossfadeSeconds > 0.0) {
                swprintf_s(text, L"%.1f", plan.crossfadeSeconds);
                add(tr(L"SignalPath_Crossfade"), ui::format(L"SignalPath_AtTrackChanges", {text}), PathState::AtTrackChanges);
            }
        }
        const bool processed = !plan.dsp.empty() || plan.gainDb != 0.0 || plan.volumeDb != 0.0 || plan.muted;
        // The chain's output is quantized once, at the device depth up to 24 bits.
        const hstring deviceDepth = to_hstring(std::min(validBits(s.output->type), 24u)) + L"-bit";
        const uint32_t container = bytesPerSample(s.output->type) * 8;
        stage = PathStage::Output;
        const PathState rawState = plan.remodulate || plan.modulatorOrder ? PathState::Processed : PathState::BitPerfect;
        if (plan.modulatorOrder) {
            const uint32_t outBytes = plan.dsd == DsdPath::Dop ? s.output->sampleRate * 2 : s.output->sampleRate;
            add(tr(L"SignalPath_Modulator"),
                ui::format(L"SignalPath_ModulatorDetail",
                           {std::to_wstring(plan.modulatorOrder), L"DSD" + std::to_wstring(outBytes * 8 / 44100)}),
                PathState::Processed);
        }
        const hstring rawDetail = plan.modulatorOrder ? hstring{} : tr(L"SignalPath_DspBypassed");
        if (plan.dsd == DsdPath::Dop)
            add(L"DoP", rawDetail, rawState);
        else if (plan.dsd == DsdPath::Native)
            add(tr(L"SignalPath_NativeDsd"), rawDetail, rawState);
        else if (plan.widen)
            add(tr(L"SignalPath_Float"), L"float32",
                processed || plan.dsd == DsdPath::Pcm ? PathState::Processed : PathState::Lossless);
        else if (processed || plan.dsd == DsdPath::Pcm)
            add(tr(L"SignalPath_Dither"), deviceDepth, PathState::Processed);
        else if (plan.reduce || plan.resample || plan.downmix)
            add(tr(L"SignalPath_Dither"), to_hstring(plan.reduce ? 16 : bits) + L"-bit", PathState::Processed);
        else if (lossy && s.format != AudioFormat::Adpcm) // ADPCM decodes to integers, which reach the device unchanged
            add(tr(L"SignalPath_Dither"), to_hstring(bits) + L"-bit", PathState::Lossy);
        else if (container > bits)
            add(tr(L"SignalPath_Pad"), to_hstring(bits) + L"→" + to_hstring(container) + L"-bit", PathState::BitPerfect);
        const PathState reached = plan.state == PipelineState::BitPerfect           ? PathState::BitPerfect
                                  : plan.state == PipelineState::LosslessConversion ? PathState::Lossless
                                  : plan.state == PipelineState::LossySource        ? PathState::Lossy
                                                                                    : PathState::Processed;
        if (s.backend == Backend::AirPlay && !s.receivers.empty()) {
            for (const auto& receiver : s.receivers) {
                add(tr(L"SignalPath_Receiver"), ui::utf8(receiver.name) + L" · " + ui::utf8(describe(*s.output)), reached);
                if (receiver.channels != AirPlayChannels::Stereo)
                    add(tr(L"SignalPath_OneChannel"),
                        tr(receiver.channels == AirPlayChannels::Left ? L"Channels_LeftBoth" : L"Channels_RightBoth"),
                        PathState::Processed);
            }
        } else if (s.backend == Backend::HQPlayer) {
            // What leaves CamelPlayer, then what HQPlayer makes of it (docs/Plan.md §5.11).
            add(tr(L"SignalPath_ToHQPlayer"), ui::utf8(s.outputMode) + L" · " + ui::utf8(describe(*s.output)), reached);
            add(tr(L"SignalPath_HQPlayer"),
                s.outputDetail.empty() ? tr(L"SignalPath_HQPlayerProcessed") : ui::utf8(s.outputDetail), PathState::Processed);
            add(tr(L"SignalPath_HQPlayerGapless"), tr(L"SignalPath_HQPlayerRateChange"), PathState::AtTrackChanges);
        } else {
            add(tr(s.backend == Backend::Network ? L"SignalPath_Renderer" : L"SignalPath_Device"),
                ui::utf8(s.outputMode) + L" · " + ui::utf8(describe(*s.output)), reached);
        }
        if (s.backend == Backend::Network && !settings_.network.gapless)
            add(tr(L"SignalPath_GaplessOff"), {}, PathState::AtTrackChanges);
        if (s.playerVolume)
            add(tr(L"SignalPath_PlayerVolume"),
                s.playerMuted      ? tr(L"SignalPath_Muted")
                : s.playerVolumeDb ? decibels(*s.playerVolumeDb)
                                   : to_hstring(*s.playerVolume),
                reached);
        showSignalPath(std::move(steps));
        dspActive = processed;
    } else {
        lampColor_ = L"ControlStrongFillColorDisabledBrush";
        setAnnounced(LampText(), {});
        SourceBadge().Visibility(Visibility::Collapsed);
        BypassBadge().Visibility(Visibility::Collapsed);
        PathRouteText().Text(ui::utf8(backendName(s.backend)) + L" · " + decoder);
        showSignalPath({});
    }
    // As in Roon, the lamp tells how a stream plays, so it shows only while one is open.
    LampButton().Visibility(s.output && s.source ? Visibility::Visible : Visibility::Collapsed);
    Lamp().Fill(themeBrush(lampColor_));
    PathVerdictDot().Fill(themeBrush(lampColor_));
    PathVerdictText().Text(LampText().Text());
    Automation::AutomationProperties::SetName(LampButton(), LampText().Text());
    dspColor_ = dspActive ? L"AccentTextFillColorPrimaryBrush" : L"TextFillColorPrimaryBrush";
    DspButtonText().Foreground(themeBrush(dspColor_));
    // Badge tooltips need a pointer; the signal path panel repeats them for keyboard and touch, with why the change into
    // the track did not crossfade.
    std::wstring note = BypassBadge().Visibility() == Visibility::Visible && s.plan ? std::wstring(tr(bypassTip(*s.plan))) : L"";
    if (const auto skip = s.plan ? s.plan->fadeSkip : camel::audio::FadeSkip::None; skip != camel::audio::FadeSkip::None) {
        const hstring reason = tr(skip == camel::audio::FadeSkip::RawDsd   ? L"Crossfade_SkipRawDsd"
                                  : skip == camel::audio::FadeSkip::Format ? L"Crossfade_SkipFormat"
                                                                           : L"Crossfade_SkipNoPreload");
        // A sentence after a full-width full stop needs no space.
        note += (note.empty() || note.ends_with(L'。') ? L"" : L" ") + std::wstring(reason);
    }
    SignalNoteText().Text(note);
    SignalNoteText().Visibility(note.empty() ? Visibility::Collapsed : Visibility::Visible);
    std::wstring status = std::wstring(LampText().Text()) + L"|" + std::wstring(BypassText().Text());
    status += SourceBadge().Visibility() == Visibility::Visible ? L"|lossy" : L"";
    status += BypassBadge().Visibility() == Visibility::Visible ? L"|bypass" : L"";
    if (status != shownStatus_) {
        shownStatus_ = std::move(status);
        fitStatus();
    }

    // Errors: open the bar once per new problem so a dismissed bar stays closed
    if (s.revision != shownRevision_) {
        shownRevision_ = s.revision;
        if (s.error) {
            showError(*s.error, s.errorDetail, snapshot.settings.backend);
        } else if (!s.receiverAsking.empty()) {
            showReceiverAsking();
        } else if (buffering) {
            // Pause ends the wait, and with it the bar.
            ErrorBar().Severity(InfoBarSeverity::Warning);
            ErrorBar().Title(ui::format(L"State_Buffering", {std::to_wstring(*s.bufferingSeconds)}));
            ErrorBar().Message(tr(L"State_BufferingDetail"));
            ErrorBar().IsClosable(false);
            ErrorBar().IsOpen(true);
        } else {
            ErrorBar().IsOpen(false);
        }
    }
    ErrorDot().Visibility(s.error && !ErrorBar().IsOpen() ? Visibility::Visible : Visibility::Collapsed);

    if (snapshot.command.id != shownCommandId_) {
        shownCommandId_ = snapshot.command.id;
        if (snapshot.commandError)
            showError(snapshot.commandError->code, snapshot.commandError->detail, snapshot.settings.backend);
    }
    // A pairing, a code shown or a change of the library ends after its command, and says how here.
    if (snapshot.finished.command != shownFinishedId_) {
        shownFinishedId_ = snapshot.finished.command;
        if (pairing_ && snapshot.finished.command == pairing_->command) { // its dialog says how the pairing went
            const auto done = std::move(pairing_->done);
            pairing_.reset();
            done(snapshot.finished.failure);
        } else if (snapshot.finished.failure) {
            showError(snapshot.finished.failure->code, snapshot.finished.failure->detail, snapshot.settings.backend);
        }
    }

    // Status area on the settings page (docs/Plan.md §6)
    StatusGrid().Visibility(s.output ? Visibility::Visible : Visibility::Collapsed);
    StatusIdleText().Visibility(s.output ? Visibility::Collapsed : Visibility::Visible);
    StatusIdleText().Text(tr(L"Status_Idle"));
    if (s.output) {
        StatusOutputText().Text(ui::utf8(describe(*s.output)));
        StatusModeText().Text(ui::utf8(s.outputMode));
        wchar_t ms[32];
        swprintf(ms, 32, L"%.1f", s.output->sampleRate ? 1000.0 * s.bufferFrames / s.output->sampleRate : 0.0);
        StatusBufferText().Text(ui::format(L"Status_BufferValue", {std::to_wstring(s.bufferFrames), ms}));
        StatusUnderrunText().Text(to_hstring(s.underruns));
        StatusDetailText().Text(clauseLines(s.outputDetail));
    }
    showStatusNotices(s);
}

std::pair<double, double> MainWindow::showPosition(const PlaybackSnapshot& s) {
    const uint32_t rate = s.source ? s.source->sampleRate : 0;
    const double pos = rate ? framesToSeconds(s.position, rate) : 0.0;
    const double total = rate ? framesToSeconds(s.total, rate) : 0.0;
    followLyrics(pos);
    if (!waveformPath_.empty() && settings_.waveform.enabled && !waveformBar_->shown() && !waveformFetching_ &&
        std::chrono::steady_clock::now() - waveformAsked_ > std::chrono::seconds(2))
        fetchWaveform();
    positionTotal_ = total;
    waveformBar_->setPlayed(total > 0 ? pos / total : 0.0);
    if (!seekTimer_.IsRunning()) { // do not fight the user while they drag
        sliderUpdating_ = true;
        PositionSlider().Maximum(std::max(1.0, total));
        PositionSlider().Value(std::min(pos, std::max(1.0, total)));
        sliderUpdating_ = false;
    }
    TimeText().Text(ui::formatTime(pos) + L" / " + ui::formatTime(total));
    return {pos, total};
}

void MainWindow::showNowPlaying(const ui::NowPlaying& was) {
    get_self<LibraryView>(Library())->showNowPlaying(was);
    get_self<PlaylistView>(Playlists())->showNowPlaying(was);
}

winrt::fire_and_forget MainWindow::readPlayingFolders(int64_t file) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    const auto server = server_;
    co_await winrt::resume_background();
    auto folders = std::make_shared<std::vector<int64_t>>();
    try {
        *folders = server->fileFolders(file);
    } catch (const std::exception& e) {
        camel::log::warn("window: playing track's folders: %s", e.what());
        co_return;
    }
    queue.TryEnqueue([weak, file, folders] {
        const auto self = weak.get();
        if (!self || ui::nowPlaying().file != file) return;
        auto playing = ui::nowPlaying();
        playing.folders = std::move(*folders);
        if (const auto was = ui::setNowPlaying(std::move(playing))) self->showNowPlaying(*was);
    });
}

void MainWindow::showReceiverAsking() {
    ErrorBar().Severity(InfoBarSeverity::Informational);
    ErrorBar().Title(ui::format(L"State_ReceiverAsking", {std::wstring(ui::utf8(shownAsking_))}));
    ErrorBar().Message(tr(L"State_ReceiverAskingDetail"));
    ErrorBar().IsClosable(false);
    ErrorBar().IsOpen(true);
}

// A network player's reason, or a known one of a device, is said whole in its title; the engine's English detail stays
// in the log.
void MainWindow::showError(ErrorCode code, const std::string& detail, Backend backend) {
    const hstring reason = backend == Backend::HQPlayer ? ui::hqplayerErrorTitle(code)
                           : isNetworkPlayer(backend)   ? ui::playerErrorTitle(code, detail)
                                                        : ui::deviceErrorTitle(code, detail);
    ErrorBar().Severity(InfoBarSeverity::Warning);
    ErrorBar().IsClosable(true);
    ErrorBar().Title(reason.empty() ? ui::errorTitle(code) : reason);
    ErrorBar().Message(reason.empty() ? ui::utf8(detail) : hstring{});
    ErrorBar().IsOpen(true);
}

// Requested ≠ active values and a missing saved device stay visible for as long as they apply.
void MainWindow::showStatusNotices(const PlaybackSnapshot& s) {
    struct Notice {
        InfoBarSeverity severity;
        hstring title, message;
    };
    std::vector<Notice> notices;
    if (s.error == ErrorCode::DeviceNotFound)
        notices.push_back({InfoBarSeverity::Error, tr(L"Status_NotConnected"), ui::utf8(s.errorDetail)});
    for (const auto& adjustment : s.outputAdjustments)
        notices.push_back({InfoBarSeverity::Warning, tr(L"Status_Adjusted"), ui::utf8(adjustment)});
    std::wstring key;
    for (const auto& notice : notices) key += std::wstring(notice.title) + L"\n" + std::wstring(notice.message) + L"\n";
    if (key == shownNotices_) return;
    shownNotices_ = key;
    StatusNotices().Children().Clear();
    for (const auto& notice : notices) {
        InfoBar bar;
        bar.Severity(notice.severity);
        bar.Title(notice.title);
        bar.Message(notice.message);
        bar.IsClosable(false);
        bar.IsOpen(true);
        StatusNotices().Children().Append(bar);
    }
}

void MainWindow::selectPage(hstring const& tag) {
    for (const auto& entry : Nav().MenuItems()) {
        const auto item = entry.try_as<NavigationViewItem>();
        if (!item) continue;
        if (unbox_value_or<hstring>(item.Tag(), L"") == tag) {
            Nav().SelectedItem(item);
            return;
        }
        for (const auto& child : item.MenuItems())
            if (unbox_value_or<hstring>(child.as<NavigationViewItem>().Tag(), L"") == tag) {
                item.IsExpanded(true);
                Nav().SelectedItem(child);
                return;
            }
    }
}

// "View all" on Home or Listening opens the full list in the library pages; Back returns to that page.
void MainWindow::viewAll(hstring const& list, hstring const& title, ui::BrowseFetch fetch) {
    auto server = server_;
    const auto back = [this, tag = shownTag_] { returnToPage(tag); };
    Nav().SelectedItem(nullptr);
    Home().Visibility(Visibility::Collapsed);
    Library().Visibility(Visibility::Visible);
    auto view = get_self<LibraryView>(Library());
    const auto albums = [](const LibraryAlbumPage& page) { return LibraryView::albumRows(page); };
    if (list == L"added") {
        view->showAlbumList(
            tr(L"Home_RecentlyAdded/Text"),
            [server, albums](int64_t offset, int64_t limit, std::optional<uint64_t> revision) {
                return albums(server->browseAlbums(AlbumSort::Added, std::nullopt, offset, limit, revision));
            },
            back);
    } else if (list == L"played") {
        view->showAlbumList(
            tr(L"Home_RecentlyPlayed/Text"),
            [server, albums](int64_t offset, int64_t limit, std::optional<uint64_t>) {
                return albums(server->recentlyPlayedAlbums(offset, limit));
            },
            back);
    } else if (list == L"topAlbums") {
        view->showAlbumList(title, std::move(fetch), back);
    } else if (list == L"topArtists") {
        view->showArtistList(title, std::move(fetch), back);
    } else {
        view->showTrackList(title, std::move(fetch), back);
    }
}

void MainWindow::showPage(hstring const& tag) {
    shownTag_ = tag;
    const bool home = tag == L"home" || tag == L"listening";
    Home().Visibility(home ? Visibility::Visible : Visibility::Collapsed);
    if (home) {
        auto homeView = get_self<HomeView>(Home());
        homeView->show(tag == L"listening");
        homeView->refresh();
    }
    SettingsPanel().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);
    const bool library = tag == L"albums" || tag == L"artists" || tag == L"composers" || tag == L"genres" || tag == L"years" ||
                         tag == L"tracks" || tag == L"folders" || std::wstring_view(tag).starts_with(L"smart:");
    Library().Visibility(library ? Visibility::Visible : Visibility::Collapsed);
    auto view = get_self<LibraryView>(Library());
    if (library)
        view->show(tag, PlaceholderTitle().Text());
    else
        view->hide();
    const std::wstring_view page(tag);
    const bool playlists = page == L"playlists" || page.starts_with(L"playlist:");
    Playlists().Visibility(playlists ? Visibility::Visible : Visibility::Collapsed);
    auto playlistView = get_self<PlaylistView>(Playlists());
    if (page == L"playlists")
        playlistView->showAll();
    else if (playlists)
        playlistView->showPlaylist(std::stoll(std::wstring(page.substr(9))));
    else
        playlistView->hide();
    const bool placeholder = !library && !playlists && tag != L"settings" && !home;
    PlaceholderPanel().Visibility(placeholder ? Visibility::Visible : Visibility::Collapsed);
    if (tag == L"settings") {
        refreshRoots();
        showDataLocation();
        AboutVersionText().Text(L"CamelPlayer " CAMEL_VERSION_STRING);
    }
    showTitleBack();
}

// Library folders as Settings shows them: icon, name and location (docs/Plan.md §8.2).
void MainWindow::showRoots(ListView const& list) {
    list.Items().Clear();
    for (const auto& root : roots_) {
        std::wstring location(ui::utf8(root.location));
        if (!root.online) location += L"  (" + std::wstring(tr(L"Settings_FolderOffline")) + L")";
        TextBlock name;
        name.Text(ui::utf8(root.name));
        TextBlock where;
        where.Text(hstring(location));
        where.TextTrimming(TextTrimming::CharacterEllipsis);
        where.Style(Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>());
        where.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        StackPanel lines;
        lines.VerticalAlignment(VerticalAlignment::Center);
        lines.Children().Append(name);
        lines.Children().Append(where);
        FontIcon icon;
        icon.Glyph(L"\xE8B7");
        icon.FontSize(28);
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(16);
        row.Padding(ThicknessHelper::FromLengths(0, 6, 0, 6));
        row.Children().Append(icon);
        row.Children().Append(lines);
        Automation::AutomationProperties::SetName(row, ui::utf8(root.name) + L", " + hstring(location));
        list.Items().Append(row);
    }
}

winrt::fire_and_forget MainWindow::refreshRoots() {
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    if (!server) co_return;
    co_await winrt::resume_background();
    std::vector<LibraryRootInfo> roots;
    try {
        server->waitIdle();
        roots = server->libraryRoots();
    } catch (const std::exception&) {
        co_return; // no library configured
    }
    queue.TryEnqueue([weak, roots] {
        auto self = weak.get();
        if (!self) return;
        self->roots_ = roots;
        self->showRoots(self->RootList());
        if (self->guideFolders_) self->showRoots(self->guideFolders_);
    });
}

winrt::fire_and_forget MainWindow::OnAddFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const auto folders = pickMusicFolders(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()));
    if (!server_) co_return;
    for (const auto& folder : folders) {
        Command command;
        command.kind = CommandKind::AddLibraryRoot;
        command.path = toUtf8(folder);
        submitCommand(std::move(command));
    }
    if (!folders.empty()) refreshRoots();
}

void MainWindow::OnScanNowClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    ScanNowButton().IsEnabled(false);
    LibraryView::scanNow(server_, std::nullopt, ContentLayout().XamlRoot());
}

winrt::fire_and_forget MainWindow::OnRemoveFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    const int index = RootList().SelectedIndex();
    if (index < 0 || size_t(index) >= roots_.size() || !server_) co_return;
    const auto root = roots_[size_t(index)];
    auto server = server_;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    LibraryRootContents contents;
    try {
        contents = server->libraryRootContents(root.id);
    } catch (const std::exception& e) {
        camel::log::warn("remove folder: %s", e.what());
        co_return;
    }
    co_await foreground;
    // Removing deletes the folder's tracks with their plays, favorites and playlist entries, so it asks first.
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Settings_RemoveFolderTitle", {std::wstring(ui::utf8(root.name))})));
    TextBlock explanation;
    explanation.Text(
        ui::format(L"Settings_RemoveFolderConfirm", {ui::count(contents.tracks), ui::count(contents.plays),
                                                     ui::count(contents.favorites), ui::count(contents.playlistEntries)}));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(ui::tr(L"Settings_RemoveFolderPrimary"));
    dialog.CloseButtonText(ui::tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    Command command;
    command.kind = CommandKind::RemoveLibraryRoot;
    command.rootId = root.id;
    submitCommand(std::move(command));
    refreshRoots();
}

void MainWindow::OnPlayPauseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    if (server_->snapshot().playback.pathUtf8.empty()) return;
    Command command;
    command.kind = CommandKind::TogglePause;
    submitCommand(std::move(command));
}

void MainWindow::OnStopClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    submitCommand(Command{});
}

void MainWindow::submitKind(CommandKind kind) {
    Command command;
    command.kind = kind;
    submitCommand(std::move(command));
}

void MainWindow::applyHotkeys() {
    if (!hotkeys_) return;
    hotkeysTaken_ = hotkeys_->apply(settings_.hotkeys);
    showHotkeys();
}

// One row per action: its combination (click to record a new one), Clear, and whether another program holds it.
void MainWindow::showHotkeys() {
    using Action = ui::GlobalHotkeys::Action;
    HotkeyRows().Children().Clear();
    const std::array<const wchar_t*, ui::GlobalHotkeys::kActions> names{L"Hotkey_PlayPause", L"Hotkey_Previous", L"Hotkey_Next"};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto action = Action(i);
        Grid row;
        row.ColumnSpacing(12);
        for (const auto width : {GridLengthHelper::FromPixels(160), GridLengthHelper::FromPixels(220), GridLengthHelper::Auto(),
                                 GridLengthHelper::FromValueAndType(1, GridUnitType::Star)}) {
            ColumnDefinition column;
            column.Width(width);
            row.ColumnDefinitions().Append(column);
        }
        TextBlock label;
        label.Text(tr(names[i]));
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);

        Button combination;
        const auto current = ui::GlobalHotkeys::describe(ui::GlobalHotkeys::of(settings_.hotkeys, action));
        combination.Content(box_value(capturing_ == action ? tr(L"Hotkey_Press")
                                      : current.empty()    ? tr(L"Hotkey_None")
                                                           : hstring(current)));
        combination.HorizontalAlignment(HorizontalAlignment::Stretch);
        Grid::SetColumn(combination, 1);
        combination.Click([this, action](auto&& sender, auto&&) {
            capturing_ = action;
            hotkeys_->suspend(); // pressing a current combination must reach the box, not play or skip
            sender.template as<Button>().Content(box_value(tr(L"Hotkey_Press")));
        });
        combination.PreviewKeyDown([this, action](auto&&, Input::KeyRoutedEventArgs const& e) {
            if (capturing_ != action) return;
            using Windows::System::VirtualKey;
            const auto key = e.Key();
            e.Handled(true);
            if (key == VirtualKey::Escape) {
                capturing_.reset();
                applyHotkeys();
                return;
            }
            if (key == VirtualKey::Control || key == VirtualKey::Menu || key == VirtualKey::Shift ||
                key == VirtualKey::LeftWindows || key == VirtualKey::RightWindows || key == VirtualKey::LeftControl ||
                key == VirtualKey::RightControl || key == VirtualKey::LeftMenu || key == VirtualKey::RightMenu ||
                key == VirtualKey::LeftShift || key == VirtualKey::RightShift)
                return; // wait for the key that completes the combination
            uint32_t modifiers = 0;
            if (GetKeyState(VK_CONTROL) < 0) modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) < 0) modifiers |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) < 0) modifiers |= MOD_SHIFT;
            if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) modifiers |= MOD_WIN;
            // Without Ctrl, Alt or Win the key would stop typing it in every other program.
            if (!(modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN))) return;
            ui::GlobalHotkeys::set(settings_.hotkeys, action, {modifiers, uint32_t(key)});
            capturing_.reset();
            settingsChanged();
            applyHotkeys();
        });
        // Only this box changes back: rebuilding the rows here, while another box is pressed, would take that box away
        // before its click.
        combination.LostFocus([this, action](auto&& sender, auto&&) {
            if (capturing_ != action) return;
            capturing_.reset();
            hotkeysTaken_ = hotkeys_->apply(settings_.hotkeys);
            const auto text = ui::GlobalHotkeys::describe(ui::GlobalHotkeys::of(settings_.hotkeys, action));
            sender.template as<Button>().Content(box_value(text.empty() ? tr(L"Hotkey_None") : hstring(text)));
        });
        row.Children().Append(combination);

        Button clear;
        clear.Content(box_value(tr(L"Hotkey_Clear")));
        clear.IsEnabled(!current.empty());
        Grid::SetColumn(clear, 2);
        clear.Click([this, action](auto&&, auto&&) {
            ui::GlobalHotkeys::set(settings_.hotkeys, action, {});
            capturing_.reset();
            settingsChanged();
            applyHotkeys();
        });
        row.Children().Append(clear);

        TextBlock status;
        status.Text(hotkeysTaken_[i] ? tr(L"Hotkey_Taken") : hstring{});
        status.TextWrapping(TextWrapping::Wrap);
        status.VerticalAlignment(VerticalAlignment::Center);
        status.Foreground(themeBrush(L"SystemFillColorCautionBrush"));
        Grid::SetColumn(status, 3);
        row.Children().Append(status);
        HotkeyRows().Children().Append(row);
    }
}

void MainWindow::OnWindowKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    using Windows::System::VirtualKey;
    const auto key = e.Key();
    if (key == VirtualKey::Escape && lyricsOpen_) {
        openLyrics(false);
        e.Handled(true);
        return;
    }
    if (key == VirtualKey::Enter && GetKeyState(VK_MENU) < 0 && !capturing_) {
        // Taken wherever the focus is, so a list does not take it for Enter and play the row.
        if (!e.KeyStatus().WasKeyDown) openFocusedFileDetails();
        e.Handled(true);
        return;
    }
    if (key != VirtualKey::Space && key != VirtualKey::Left && key != VirtualKey::Right) return;
    if (capturing_ || !server_ || !Content().XamlRoot()) return;
    if (Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(Content().XamlRoot()).Size() > 0) return;
    // Alt+Left goes back wherever the focus is, before a focused slider or list takes the arrow key for itself.
    if (key == VirtualKey::Left && GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
        if (!e.KeyStatus().WasKeyDown && goBack()) e.Handled(true);
        return;
    }
    const auto focused = Input::FocusManager::GetFocusedElement(Content().XamlRoot());
    if (focused && (focused.try_as<TextBox>() || focused.try_as<PasswordBox>() || focused.try_as<RichEditBox>() ||
                    focused.try_as<NumberBox>() || focused.try_as<AutoSuggestBox>()))
        return;
    // A control reached by the keyboard takes Space as its own press; one the pointer left focused does not, so Space
    // after a click on Next still plays or pauses.
    if (const auto control = focused.try_as<Control>();
        key == VirtualKey::Space && control && control.FocusState() != FocusState::Pointer &&
        (focused.try_as<Primitives::ButtonBase>() || focused.try_as<ToggleSwitch>() || focused.try_as<ComboBox>() ||
         focused.try_as<Primitives::SelectorItem>()))
        return;
    const bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (GetKeyState(VK_MENU) < 0 || GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) return;
    if (key == VirtualKey::Space && !control && !shift) {
        if (!e.KeyStatus().WasKeyDown) submitKind(CommandKind::TogglePause);
    } else if (control && !shift) {
        if (e.KeyStatus().WasKeyDown) return;
        submitKind(key == VirtualKey::Right ? CommandKind::Next : CommandKind::Previous);
    } else if (control && shift) {
        const auto s = server_->snapshot().playback;
        if (!s.source || !s.source->sampleRate) return;
        const int64_t step = int64_t(s.source->sampleRate) * 10;
        int64_t target = int64_t(s.position) + (key == VirtualKey::Right ? step : -step);
        target = std::max<int64_t>(target, 0);
        if (s.total) target = std::min<int64_t>(target, int64_t(s.total));
        Command command;
        command.kind = CommandKind::Seek;
        command.frame = uint64_t(target);
        submitCommand(std::move(command));
    } else {
        return;
    }
    e.Handled(true);
}

void MainWindow::OnHotkeyDefaultsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.hotkeys = {};
    capturing_.reset();
    settingsChanged();
    applyHotkeys();
}

void MainWindow::OnPreviousClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::Previous;
    submitCommand(std::move(command));
}

void MainWindow::OnNextClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::Next;
    submitCommand(std::move(command));
}

void MainWindow::OnShuffleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    Command command;
    command.kind = CommandKind::SetShuffle;
    command.shuffle = !server_->snapshot().queue.shuffle;
    submitCommand(std::move(command));
}

void MainWindow::OnRepeatClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    const auto mode = server_->snapshot().queue.repeat;
    Command command;
    command.kind = CommandKind::SetRepeat;
    command.repeat = mode == RepeatMode::Off ? RepeatMode::One : mode == RepeatMode::One ? RepeatMode::All : RepeatMode::Off;
    submitCommand(std::move(command));
}

// Takes effect at once; the window content carries the theme, and the backdrop and caption buttons follow it. Black is
// the dark theme on black surfaces without Mica.
void MainWindow::applyTheme() {
    const bool black = settings_.theme == Theme::Black;
    const auto theme = settings_.theme == Theme::Light    ? ElementTheme::Light
                       : settings_.theme == Theme::System ? ElementTheme::Default
                                                          : ElementTheme::Dark;
    const auto content = Content().as<FrameworkElement>();
    if (black != ui::blackSurfacesShown()) {
        ui::showBlackSurfaces(black);
        const bool mica = !black && Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported();
        SystemBackdrop(mica ? Media::SystemBackdrop(Media::MicaBackdrop()) : nullptr);
        ThemeBackground().Visibility(mica ? Visibility::Collapsed : Visibility::Visible);
    }
    if (content.RequestedTheme() != theme) content.RequestedTheme(theme);
    fileDetails_.setTheme(content.ActualTheme());
}

Media::Brush MainWindow::themeBrush(const wchar_t* key) const {
    return ui::themeBrush(Content().as<FrameworkElement>(), key);
}

// Colors set from code take the theme's brushes again once the window shows another theme.
void MainWindow::repaintTheme() {
    if (waveformBar_) waveformBar_->repaint();
    Lamp().Fill(themeBrush(lampColor_));
    PathVerdictDot().Fill(themeBrush(lampColor_));
    DspButtonText().Foreground(themeBrush(dspColor_));
    if (playbackFavorite_) PlaybackFavoriteIcon().Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
    showSignalPath(std::exchange(shownPath_, {}));
    showHotkeys();
    showRoots(RootList());
    if (eqEditor_) eqEditor_->repaint();
    markSettingsSection();
}

// The minimize, maximize and close buttons sit on our title bar, so they follow the app's light or dark theme.
void MainWindow::colorCaptionButtons() {
    ui::colorCaptionButtons(AppWindow(), Content().as<FrameworkElement>().ActualTheme() == ElementTheme::Dark);
}

void MainWindow::OnPaneToggleRequested(TitleBar const&, Windows::Foundation::IInspectable const&) {
    Nav().IsPaneOpen(!Nav().IsPaneOpen());
}

// Back sits in the title bar, as in other Windows apps, while a library page has somewhere to return to.
void MainWindow::addSettingsLinks() {
    const std::pair<const wchar_t*, FrameworkElement> cards[] = {
        {L"Settings_Library/Text", LibraryCard()},
        {L"Settings_Outputs/Text", OutputsCard()},
        {L"Settings_Playback/Text", PlaybackCard()},
        {L"Settings_Dsp/Text", VolumeCard()},
        {L"Settings_Status/Text", StatusCard()},
        {L"Settings_Decoders/Text", DecodersCard()},
        {L"Settings_Interface/Text", InterfaceCard()},
        {L"Settings_Hotkeys/Text", HotkeysCard()},
        {L"Reset_Title/Text", ResetCard()},
        {L"Settings_About/Text", AboutCard()},
    };
    for (const auto& [title, card] : cards) {
        HyperlinkButton link;
        link.Content(box_value(tr(title)));
        link.Padding(ThicknessHelper::FromLengths(0, 5, 0, 6));
        link.Click([card](auto&&, auto&&) {
            BringIntoViewOptions options;
            options.VerticalAlignmentRatio(0);
            options.AnimationDesired(true);
            card.StartBringIntoView(options);
        });
        SettingsLinks().Children().Append(link);
        settingsLinks_.emplace_back(card, link);
    }
}

// The last card whose top has reached the upper third of the view, or the last card at the end of the scroll.
void MainWindow::markSettingsSection() {
    if (SettingsPanel().Visibility() != Visibility::Visible) return;
    const auto scroller = SettingsScroller();
    size_t current = 0;
    if (scroller.ScrollableHeight() > 0 && scroller.VerticalOffset() >= scroller.ScrollableHeight() - 1) {
        current = settingsLinks_.size() - 1;
    } else {
        for (size_t i = 0; i < settingsLinks_.size(); ++i)
            if (settingsLinks_[i].first.TransformToVisual(scroller).TransformPoint({0, 0}).Y <= scroller.ViewportHeight() / 3)
                current = i;
    }
    for (size_t i = 0; i < settingsLinks_.size(); ++i) {
        const auto& link = settingsLinks_[i].second;
        link.FontWeight(Windows::UI::Text::FontWeight{uint16_t(i == current ? 600 : 400)});
        if (i == current)
            link.Foreground(themeBrush(L"TextFillColorPrimaryBrush"));
        else
            link.ClearValue(Controls::Control::ForegroundProperty());
    }
}

void MainWindow::OnSettingsScrolled(Windows::Foundation::IInspectable const&, ScrollViewerViewChangedEventArgs const&) {
    markSettingsSection();
}

void MainWindow::showTitleBack() {
    AppTitleBar().IsBackButtonVisible(
        lyricsOpen_ || listeningShown() ||
        (Library().Visibility() == Visibility::Visible && get_self<LibraryView>(Library())->canGoBack()));
}

// Listening opens from Home's month card and keeps Home selected in the navigation pane; Back returns to Home.
bool MainWindow::listeningShown() {
    return shownTag_ == L"listening" && Home().Visibility() == Visibility::Visible;
}

void MainWindow::OnTitleBackRequested(TitleBar const&, Windows::Foundation::IInspectable const&) {
    goBack();
}

bool MainWindow::goBack() {
    if (lyricsOpen_) {
        openLyrics(false);
        return true;
    }
    if (!AppTitleBar().IsBackButtonVisible()) return false;
    if (listeningShown())
        showPage(L"home");
    else
        get_self<LibraryView>(Library())->goBack();
    return true;
}

void MainWindow::fitNavigationPane() {
    const bool compact = queueOpen_ && Nav().ActualWidth() > 0 && Nav().ActualWidth() < 1200;
    if (compact == navCompacted_) return;
    navCompacted_ = compact;
    Nav().IsPaneOpen(!compact);
}

void MainWindow::updateQueuePanel() {
    QueuePanel().Visibility(queueOpen_ ? Visibility::Visible : Visibility::Collapsed);
    QueueColumn().Width(GridLengthHelper::FromPixels(queueOpen_ ? 360 : 0));
    QueueToggleButton().IsChecked(queueOpen_);
    fitNavigationPane();
    std::ofstream queueState(camel::dataDirectory() / L"queue-panel-open", std::ios::trunc);
    queueState << queueOpen_;
}

// Each keystroke cancels the previous query; results that arrive late for older text are dropped.
winrt::fire_and_forget MainWindow::OnSearchTextChanged(AutoSuggestBox const& box, AutoSuggestBoxTextChangedEventArgs const& e) {
    if (e.Reason() != AutoSuggestionBoxTextChangeReason::UserInput) co_return;
    searchToken_.cancel();
    searchToken_ = {};
    const uint64_t generation = ++searchGeneration_;
    const int64_t typed = camel::measure::now();
    const hstring text = box.Text();
    const std::string query = camel::toUtf8(std::wstring_view(text.c_str(), text.size()));
    if (query.find_first_not_of(" \t") == std::string::npos || !server_) {
        suggestions_.clear();
        box.ItemsSource(nullptr);
        co_return;
    }
    auto weak = get_weak();
    auto server = server_;
    auto token = searchToken_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    auto results = std::make_shared<LibrarySearchResults>();
    try {
        *results = server->searchLibrary(query, 0, 3, std::nullopt, token, false);
    } catch (const std::exception&) {
        co_return; // cancelled by the next keystroke, or the library changed meanwhile
    }
    const int64_t found = camel::measure::now();
    queue.TryEnqueue([weak, generation, results, text, typed, found] {
        auto self = weak.get();
        if (!self || generation != self->searchGeneration_) return;
        self->showSuggestions(*results, text);
        if (camel::measure::measuring())
            camel::log::info("measure: suggestions for %u characters in %.1f ms (server %.1f ms)", text.size(),
                             camel::measure::milliseconds(typed, camel::measure::now()),
                             camel::measure::milliseconds(typed, found));
    });
}

void MainWindow::showSuggestions(const LibrarySearchResults& results, hstring const& text) {
    const hstring tracksLabel = tr(L"Search_Tracks"), albumsLabel = tr(L"Search_Albums"), artistsLabel = tr(L"Search_Artists");
    suggestions_.clear();
    auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    const auto add = [&](Suggestion suggestion, hstring subtitle, hstring group) {
        auto item = make<BrowseItem>();
        get_self<BrowseItem>(item)->fill({0, {}, suggestion.title, std::move(subtitle), {}, std::move(group)});
        items.Append(item);
        suggestions_.push_back(std::move(suggestion));
    };
    for (const auto& t : results.tracks.tracks)
        add({Suggestion::Kind::Track, t.id, t.path, ui::utf8(t.title)}, ui::utf8(t.artist) + L" · " + ui::utf8(t.album),
            tracksLabel);
    for (const auto& a : results.albums)
        add({Suggestion::Kind::Album, a.id, {}, ui::utf8(a.title)}, ui::utf8(a.artist), albumsLabel);
    for (const auto& name : results.artists) add({Suggestion::Kind::Artist, 0, name, ui::utf8(name)}, {}, artistsLabel);
    add({Suggestion::Kind::All, 0, {}, ui::format(L"Search_All", {text.c_str()})}, {}, {});
    suggestionItems_ = items;
    SearchBox().ItemsSource(items);
}

void MainWindow::OnSearchQuerySubmitted(AutoSuggestBox const& box, AutoSuggestBoxQuerySubmittedEventArgs const& e) {
    searchToken_.cancel();
    uint32_t index = 0;
    if (const auto chosen = e.ChosenSuggestion();
        chosen && suggestionItems_ && suggestionItems_.IndexOf(chosen, index) && index < suggestions_.size()) {
        const Suggestion suggestion = suggestions_[index];
        auto view = get_self<LibraryView>(Library());
        switch (suggestion.kind) {
        case Suggestion::Kind::Track: {
            Command play;
            play.kind = CommandKind::QueueReplace;
            play.sources.emplace_back(suggestion.value);
            play.index = 0;
            camel::measure::markPlayRequested();
            submitCommand(std::move(play));
            return;
        }
        case Suggestion::Kind::Album: {
            // Over the page the search was made from, which Back returns to, as the player's album link does.
            auto leave = openLibraryOverPage();
            const bool keepHistory = !leave;
            view->showAlbum(suggestion.id, suggestion.title, std::move(leave), keepHistory);
            return;
        }
        case Suggestion::Kind::Artist:
            openArtist(suggestion.title);
            return;
        case Suggestion::Kind::All:
            break;
        }
    }
    showSearchResults(box.Text());
}

void MainWindow::showSearchResults(hstring const& query) {
    if (std::wstring_view(query).find_first_not_of(L" \t") == std::wstring_view::npos) return;
    // Back returns to the page the search started from, such as Settings.
    auto leave = openLibraryOverPage();
    Nav().SelectedItem(nullptr);
    get_self<LibraryView>(Library())->showSearch(query, std::move(leave));
}

// Opens Settings at `card`. A page just shown has no layout yet, so the card is scrolled to after the first pass that
// gives it one.
void MainWindow::showSettingsCard(FrameworkElement const& card) {
    const auto bringIntoView = [card] {
        BringIntoViewOptions options;
        options.VerticalAlignmentRatio(0);
        card.StartBringIntoView(options);
    };
    if (SettingsPanel().Visibility() == Visibility::Visible) return bringIntoView();
    returnToPage(L"settings");
    auto pending = std::make_shared<FrameworkElement::LayoutUpdated_revoker>();
    *pending = card.LayoutUpdated(auto_revoke, [pending, bringIntoView](auto&&, auto&&) {
        pending->revoke();
        bringIntoView();
    });
}

void MainWindow::returnToPage(hstring const& tag) {
    if (tag == L"settings") {
        Nav().SelectedItem(Nav().SettingsItem());
    } else if (tag == L"listening") {
        selectPage(L"home");
        showPage(tag);
    } else {
        selectPage(tag);
    }
}

// The playback bar opens the playing album or artist over the current page: Back returns to the library view it
// covered, or through the returned `leave` to any other page.
std::function<void()> MainWindow::openLibraryOverPage() {
    if (Library().Visibility() == Visibility::Visible) return nullptr;
    const hstring tag = shownTag_;
    Nav().SelectedItem(nullptr);
    SettingsPanel().Visibility(Visibility::Collapsed);
    PlaceholderPanel().Visibility(Visibility::Collapsed);
    Home().Visibility(Visibility::Collapsed);
    Playlists().Visibility(Visibility::Collapsed);
    get_self<PlaylistView>(Playlists())->hide();
    Library().Visibility(Visibility::Visible);
    return [this, tag] { returnToPage(tag); };
}

// On a library page, the playing track there; elsewhere its album's page.
void MainWindow::OnGoToPlayingAccelerator(Input::KeyboardAccelerator const&,
                                          Input::KeyboardAcceleratorInvokedEventArgs const& e) {
    e.Handled(true);
    if (ui::nowPlaying().file == 0) return;
    auto* library = get_self<LibraryView>(Library());
    if (Library().Visibility() == Visibility::Visible) return library->goToPlaying();
    library->selectPlayingWhenLoaded();
    OnAlbumLinkClick(nullptr, nullptr);
}

void MainWindow::OnSearchAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& e) {
    SearchBox().Focus(FocusState::Keyboard);
    e.Handled(true);
}

// Frame intervals while CAMELPLAYER_MEASURE is set, summarized every two seconds (docs/Plan.md §12.1 album-wall scrolling).
void MainWindow::recordFrame() {
    const int64_t now = camel::measure::now();
    if (frameLast_) frameIntervals_.push_back(camel::measure::milliseconds(frameLast_, now));
    frameLast_ = now;
    if (!frameWindow_) frameWindow_ = now;
    if (camel::measure::milliseconds(frameWindow_, now) < 2000 || frameIntervals_.empty()) return;
    std::sort(frameIntervals_.begin(), frameIntervals_.end());
    const auto late = std::count_if(frameIntervals_.begin(), frameIntervals_.end(), [](double ms) { return ms > 25.0; });
    camel::log::info("measure: %zu frames, p50 %.1f ms, p99 %.1f ms, max %.1f ms, %lld over 25 ms", frameIntervals_.size(),
                     frameIntervals_[frameIntervals_.size() / 2], frameIntervals_[frameIntervals_.size() * 99 / 100],
                     frameIntervals_.back(), (long long)late);
    frameIntervals_.clear();
    frameWindow_ = now;
}

void MainWindow::applySettingsNow() {
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    populateSettings();
}

// Where CamelPlayer keeps its data (never next to the program, docs/Plan.md release section) and how large it is.
winrt::fire_and_forget MainWindow::showDataLocation() {
    const auto data = camel::dataDirectory();
    DataLocationText().Text(hstring(data.wstring()));
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    const auto size = [](const std::filesystem::path& path) { return camel::sizeOnDisk(path); };
    const uintmax_t library = size(data / L"library.db") + size(data / L"library.db-wal") + size(data / L"library.db-shm");
    const uintmax_t covers = size(data / L"thumbs"), settings = size(camel::settingsPath()), logs = size(data / L"logs");
    queue.TryEnqueue([weak, library, covers, settings, logs] {
        auto self = weak.get();
        if (!self) return;
        const auto bytes = [](uintmax_t n) {
            wchar_t text[32];
            if (n >= (1ull << 30))
                swprintf(text, 32, L"%.1f GB", double(n) / double(1ull << 30));
            else if (n >= (1ull << 20))
                swprintf(text, 32, L"%.1f MB", double(n) / double(1ull << 20));
            else
                swprintf(text, 32, L"%.1f KB", double(n) / 1024.0);
            return std::wstring(text);
        };
        self->DataSizesText().Text(ui::format(L"Reset_Sizes", {bytes(library), bytes(covers), bytes(settings), bytes(logs)}));
    });
}

void MainWindow::OnOpenDataFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    ShellExecuteW(nullptr, L"open", camel::dataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Settings › About (docs/Plan.md §17.5): the diagnostic bundle for GitHub issues, saved where the user picks.
winrt::fire_and_forget MainWindow::OnDiagnosticBundleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!server_) co_return;
    constexpr COMDLG_FILTERSPEC types[] = {{L"Zip", L"*.zip"}};
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::array<wchar_t, 64> name{};
    swprintf(name.data(), name.size(), L"CamelPlayer-diagnostics-%04u%02u%02u-%02u%02u%02u.zip", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    const auto chosen = ui::pickFile(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), types, true, L"zip", name.data());
    if (!chosen) co_return;
    const std::filesystem::path& file = *chosen;
    const auto checked = DiagnosticPathsCheck().IsChecked();
    const bool includePaths = checked && checked.Value();
    DiagnosticButton().IsEnabled(false);
    auto server = server_;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    bool done = true;
    try {
        server->writeDiagnosticBundle(file, includePaths, CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        camel::log::warn("diagnostic bundle: %s", e.what());
        done = false;
    }
    queue.TryEnqueue([weak, done, file] {
        auto self = weak.get();
        if (!self) return;
        self->DiagnosticButton().IsEnabled(true);
        self->showPlaylistNotice(tr(done ? L"Diagnostic_Saved" : L"Diagnostic_Failed"), hstring(file.wstring()), !done);
    });
}

// Settings › Reset (docs/Plan.md §8.2): every reset says what it changes and asks first; resetting everything also
// needs a checked acknowledgement because play statistics and playlists go with it.
winrt::fire_and_forget MainWindow::OnResetClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    auto strong = get_strong();
    const hstring kind = unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L"");
    const std::wstring key = kind == L"playback"     ? L"Playback"
                             : kind == L"interface"  ? L"Interface"
                             : kind == L"library"    ? L"Library"
                             : kind == L"covers"     ? L"Covers"
                             : kind == L"statistics" ? L"Statistics"
                                                     : L"All";
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr((L"Reset_" + key + L"Title/Text").c_str())));
    StackPanel content;
    content.Spacing(12);
    TextBlock explanation;
    explanation.Text(tr((L"Reset_" + key + L"Confirm").c_str()));
    explanation.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(explanation);
    CheckBox understood{nullptr};
    if (kind == L"all") {
        understood = CheckBox();
        understood.Content(box_value(tr(L"Reset_AllAcknowledge")));
        content.Children().Append(understood);
    }
    dialog.Content(content);
    dialog.PrimaryButtonText(tr(kind == L"all" ? L"Reset_AllPrimary" : L"Reset_Confirm"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (understood) {
        dialog.IsPrimaryButtonEnabled(false);
        understood.Checked([dialog](auto&&, auto&&) { dialog.IsPrimaryButtonEnabled(true); });
        understood.Unchecked([dialog](auto&&, auto&&) { dialog.IsPrimaryButtonEnabled(false); });
    }
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;

    if (kind == L"playback") {
        settings_ = settingsDto(camel::resetPlayback(nativeSettings(settings_)));
        applySettingsNow();
    } else if (kind == L"interface") {
        camel::startup::disable();
        settings_ = settingsDto(camel::resetInterface(nativeSettings(settings_)));
        queueOpen_ = false;
        updateQueuePanel();
        applySettingsNow();
        changeLanguage();
    } else if (kind == L"statistics") {
        Command command;
        command.kind = CommandKind::ResetAllPlayStatistics;
        submitCommand(std::move(command));
    } else if (kind == L"library" || kind == L"covers") {
        auto server = server_;
        auto queue = DispatcherQueue();
        const bool library = kind == L"library";
        co_await winrt::resume_background();
        try {
            if (library)
                server->rereadLibrary();
            else
                server->clearArtworkCache();
        } catch (const std::exception& e) {
            camel::log::warn("reset: %s", e.what());
        }
        queue.TryEnqueue([weak = get_weak()] {
            if (auto self = weak.get()) self->showDataLocation();
        });
    } else {
        // The next instance removes the data before its Server opens it, then starts like a new installation.
        std::ofstream(camel::dataDirectory() / L"reset-pending") << 1;
        std::wstring executable(32768, L'\0');
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
        ShellExecuteW(nullptr, L"open", executable.c_str(), L"--after-reset", nullptr, SW_SHOWNORMAL);
        exiting_ = true;
        Close();
    }
}

void MainWindow::OnAlbumLinkClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    const auto snapshot = server_->snapshot();
    const auto current = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                      [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    if (current == snapshot.queue.entries.end() || current->albumId <= 0) return;
    auto leave = openLibraryOverPage();
    const bool keepHistory = !leave;
    get_self<LibraryView>(Library())->showAlbum(current->albumId, ui::utf8(current->album), std::move(leave), keepHistory);
}

// The credit shows as plain text at once and gains its links when the library has split it into names.
void MainWindow::showPlayerArtist(hstring const& artist) {
    if (artist == shownArtist_) return;
    shownArtist_ = artist;
    const auto open = [weak = get_weak()](hstring const& name) {
        if (auto window = weak.get()) window->openArtist(name);
    };
    const auto show = [this, open](const std::vector<LibraryNamePart>& parts) {
        ui::showCreditButtons(
            ArtistCredit(), parts, open,
            ArtistCredit().Resources().Lookup(box_value(L"CreditSeparatorStyle")).as<Microsoft::UI::Xaml::Style>(), 12);
        fitPlayerArtist();
    };
    show({{winrt::to_string(artist), false}});
    ToolTipService::SetToolTip(ArtistCredit(), artist.empty() ? nullptr : box_value(artist));
    ArtistCredit().Visibility(artist.empty() ? Visibility::Collapsed : Visibility::Visible);
    if (artist.empty() || !server_) return;
    [](winrt::weak_ref<MainWindow> weak, std::shared_ptr<IServer> server, Microsoft::UI::Dispatching::DispatcherQueue queue,
       hstring artist, std::function<void(const std::vector<LibraryNamePart>&)> show) -> winrt::fire_and_forget {
        co_await winrt::resume_background();
        std::vector<LibraryNamePart> parts;
        try {
            parts = server->creditParts({winrt::to_string(artist)}).front();
        } catch (const std::exception& e) {
            camel::log::warn("player bar credit: %s", e.what());
            co_return;
        }
        queue.TryEnqueue([weak, artist, parts, show] {
            auto self = weak.get();
            if (self && self->shownArtist_ == artist) show(parts);
        });
    }(get_weak(), server_, DispatcherQueue(), artist, show);
}

void MainWindow::openArtist(hstring const& artist) {
    auto leave = openLibraryOverPage();
    const bool keepHistory = !leave;
    get_self<LibraryView>(Library())->showArtist(artist, std::move(leave), keepHistory);
}

void MainWindow::OnSliderValueChanged(Windows::Foundation::IInspectable const&,
                                      Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (sliderUpdating_ || !server_) return;
    pendingSeek_ = e.NewValue();
    seekTimer_.Stop();
    seekTimer_.Start(); // seek once the user pauses dragging
}

void MainWindow::OnNavSelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& e) {
    if (navUpdating_) return;
    if (e.IsSettingsSelected()) {
        SearchBox().Text(L"");
        showPage(L"settings");
        return;
    }
    auto item = e.SelectedItemContainer();
    if (!item) return;
    SearchBox().Text(L"");
    const hstring tag = unbox_value_or<hstring>(item.Tag(), L"albums");
    PlaceholderTitle().Text(unbox_value_or<hstring>(item.Content(), L""));
    showPage(tag);
}

void MainWindow::OnSettingChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnNumberChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnLanguageChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    const int i = LanguageCombo().SelectedIndex();
    settings_.language = i == 1 ? "zh-TW" : i == 2 ? "en-US" : "";
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    changeLanguage();
}

// Takes effect at once (docs/Plan.md §8.5): after this handler, as the control that called it goes with the window.
void MainWindow::changeLanguage() {
    DispatcherQueue().TryEnqueue([weak = get_weak()] {
        const auto self = weak.get();
        if (!self) return;
        try {
            // An empty override fails at run time, so following Windows names Windows's language.
            winrt::Microsoft::Windows::Globalization::ApplicationLanguages::PrimaryLanguageOverride(
                self->settings_.language.empty() ? Windows::System::UserProfile::GlobalizationPreferences::Languages().GetAt(0)
                                                 : to_hstring(self->settings_.language));
        } catch (const hresult_error& e) {
            camel::log::warn("language override failed: 0x%08X", static_cast<unsigned>(e.code()));
            return;
        }
        ui::reloadText();
        self->replaceOnceIdle(self->server_);
    });
}

// The window in the new language takes over once the Server has done what was asked of it, waited for off the UI thread:
// a long command, or a library change waiting for the library, leaves the window answering meanwhile.
winrt::fire_and_forget MainWindow::replaceOnceIdle(std::shared_ptr<camel::server::IServer> server) {
    const auto weak = get_weak();
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    server->waitIdle();
    co_await foreground;
    const auto self = weak.get();
    if (!self) co_return;
    self->replaced_ = true;
    self->exiting_ = true;
    self->tray_.reset();
    self->hotkeys_.reset();
    self->media_.reset();
    server->setListener({});
    self->replaceWindow_(server);
}

void MainWindow::takeOverFrom(MainWindow& old) {
    using Microsoft::UI::Windowing::OverlappedPresenter;
    using Microsoft::UI::Windowing::OverlappedPresenterState;
    const auto before = old.AppWindow();
    AppWindow().MoveAndResize({before.Position().X, before.Position().Y, before.Size().Width, before.Size().Height});
    if (const auto presenter = before.Presenter().try_as<OverlappedPresenter>();
        presenter && presenter.State() == OverlappedPresenterState::Maximized)
        AppWindow().Presenter().as<OverlappedPresenter>().Maximize();
    openInterfaceCard_ = true;
}

// The pane toggle and title on the left and the caption buttons on the right keep their room.
void MainWindow::OnTitleBarSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const& e) {
    constexpr double kAroundSearch = 420, kSearchMin = 160, kSearchMax = 420;
    SearchBox().Width(std::clamp(e.NewSize().Width - kAroundSearch, kSearchMin, kSearchMax));
}

void MainWindow::OnUpdateSwitchToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnArtistPicturesToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnFolderTreeToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnAnalysisWindowToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnAnalysisTimeChanged(TimePicker const&, TimePickerSelectedValueChangedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnPositionPointerMoved(Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    const double width = PositionSlider().ActualWidth() - 20.0; // the thumb's travel, inside half a thumb at each end
    if (width <= 0 || positionTotal_ <= 0) return;
    const double x = e.GetCurrentPoint(PositionSlider()).Position().X - 10.0;
    positionTip_.Content(box_value(hstring(ui::formatTime(std::clamp(x / width, 0.0, 1.0) * positionTotal_))));
}

winrt::fire_and_forget MainWindow::fetchWaveform() {
    auto strong = get_strong();
    waveformFetching_ = true;
    waveformAsked_ = std::chrono::steady_clock::now();
    const auto path = waveformPath_;
    auto server = server_;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    std::optional<std::vector<uint8_t>> peaks;
    try {
        peaks = server->waveform(path);
    } catch (const std::exception& e) {
        camel::log::warn("waveform: %s", e.what());
    }
    co_await foreground;
    waveformFetching_ = false;
    if (peaks && !peaks->empty() && path == waveformPath_ && settings_.waveform.enabled) waveformBar_->show(std::move(*peaks));
}

void MainWindow::OnFolderPlaysOnToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnDiscordToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    DiscordCoversSwitch().IsEnabled(DiscordSwitch().IsOn());
    settingsChanged();
}

// Whether Discord is reached and how far the cover tunnel is, so a missing activity can be explained.
void MainWindow::showDiscordStatus() {
    const auto status = server_->discordStatus();
    std::wstring text;
    if (status.connection == DiscordConnection::Connected)
        text = std::wstring(tr(L"Discord_Connected"));
    else if (status.connection == DiscordConnection::Searching)
        text = std::wstring(tr(L"Discord_Searching"));
    const wchar_t* cover = status.cover == DiscordCover::Downloading ? L"Discord_CoverDownloading"
                           : status.cover == DiscordCover::Starting  ? L"Discord_CoverStarting"
                           : status.cover == DiscordCover::Ready     ? L"Discord_CoverReady"
                                                                     : nullptr;
    if (cover) text += L" " + std::wstring(tr(cover));
    if (status.cover == DiscordCover::Failed)
        text += L" " + std::wstring(ui::format(L"Discord_CoverFailed", {std::wstring(ui::utf8(status.coverError))}));
    if (DiscordStatusText().Text() == hstring(text)) return;
    DiscordStatusText().Text(hstring(text));
    DiscordStatusText().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::OnStatusAreaSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&) {
    fitStatus();
}

// Where the badges do not fit the lamp's words give way, then the lamp and DSP buttons draw in their sides; the lamp's dot
// stays and opens the signal path.
void MainWindow::fitStatus() {
    const auto fits = [this] {
        StatusBadges().Measure({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
        return StatusBadges().DesiredSize().Width <= StatusArea().ActualWidth();
    };
    const auto sides = [this](double width) {
        for (const auto& button : {LampButton(), DspButton()}) button.Padding(ThicknessHelper::FromLengths(width, 2, width, 2));
    };
    LampText().Visibility(Visibility::Visible);
    sides(10);
    if (StatusArea().ActualWidth() == 0 || fits()) return;
    LampText().Visibility(Visibility::Collapsed);
    if (!fits()) sides(4);
}

void MainWindow::OnTextTrimmedChanged(TextBlock const& text, IsTextTrimmedChangedEventArgs const&) {
    ui::tipWhenTrimmed(text);
}

void MainWindow::OnArtistCreditSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const& e) {
    if (e.PreviousSize().Width != e.NewSize().Width) fitPlayerArtist();
}

// The credit gets the width of the column it is in; a panel of links reports its own width, wider than they are.
void MainWindow::fitPlayerArtist() {
    ui::fitCreditButtons(ArtistCredit(), PlayerTextColumn().ActualWidth(),
                         ArtistCredit().Resources().Lookup(box_value(L"CreditSeparatorStyle")).as<Microsoft::UI::Xaml::Style>(),
                         12);
}

} // namespace winrt::CamelPlayer::implementation
