// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>

#pragma comment(lib, "propsys.lib")
#include <thread>
#include <utility>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "CreditText.h"
#include "DialogPlacement.h"
#include "FileDetailsWindow.h"
#include "OutputsFollow.h"
#include "ThemeBrush.h"
#include "UiText.h"
#include "TrackColumns.h"
#include "TrimmedText.h"
#include "ArtworkLoader.h"
#include "Common/settings/EqText.h"
#include "Common/settings/Settings.h"
#include "Common/util/FolderWalk.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/StartWithWindows.h"
#include "Server/Library/Lyrics.h"
#include "Common/util/Text.h"
#include "Common/Version.h"
#include "Engine/Dsp.h"
#include "Server/Network/Firewall.h"
#include "Server/Playback/Outputs.h"
#include "TrayIcon.h"
#include "HomeView.xaml.h"
#include "TrackMarks.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::themeBrush;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

namespace {

std::filesystem::path programDirectory() {
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size())));
    return std::filesystem::path(executable).parent_path();
}

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

std::wstring kilohertz(uint32_t rate) {
    wchar_t text[16];
    swprintf_s(text, L"%g", rate / 1000.0);
    return text;
}

hstring impulseInfoText(const ImpulseInfo& info, uint32_t rate) {
    const wchar_t* layout = info.channels == 1   ? L"Convolution_Mono"
                            : info.channels == 2 ? L"Convolution_Stereo"
                                                 : L"Convolution_TrueStereo";
    wchar_t seconds[16];
    swprintf_s(seconds, L"%.2f", double(info.frames) / rate);
    std::wstring rates;
    for (const auto r : info.rates) rates += (rates.empty() ? L"" : L", ") + kilohertz(r);
    hstring text =
        ui::format(L"Convolution_Info", {std::wstring(tr(layout)), std::to_wstring(info.frames), seconds, kilohertz(rate)}) +
        L" " + ui::format(L"Convolution_Rates", {rates});
    if (info.converted) text = text + L" " + ui::format(L"Convolution_Converted", {kilohertz(info.sourceRate)});
    return text;
}

hstring impulseProblemText(ImpulseProblem problem) {
    switch (problem) {
    case ImpulseProblem::Layout:
        return tr(L"Convolution_Layout");
    case ImpulseProblem::TooLong:
        return tr(L"Convolution_TooLong");
    default:
        return tr(L"Convolution_Unreadable");
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

constexpr std::optional<SampleType> kFormatChoices[] = {std::nullopt, SampleType::Int16, SampleType::Int24, SampleType::Int24In32,
                                                        SampleType::Int32};

int formatIndex(std::optional<SampleType> t) {
    for (int i = 0; i < 5; i++)
        if (kFormatChoices[i] == t) return i;
    return 0;
}

void fill(ComboBox const& combo, std::vector<hstring> const& items, int selected) {
    combo.Items().Clear();
    for (auto& s : items) combo.Items().Append(box_value(s));
    combo.SelectedIndex(items.empty() ? -1 : std::clamp(selected, 0, int(items.size()) - 1));
}

double numberOr(NumberBox const& box, double fallback) {
    double v = box.Value();
    return std::isnan(v) ? fallback : v;
}

// Output details arrive as one line of ", " and "; " separated clauses; the status area shows one per line.
hstring clauseLines(const std::string& detail) {
    std::wstring text(ui::utf8(detail));
    for (const wchar_t* separator : {L"; ", L", "})
        for (size_t at = text.find(separator); at != std::wstring::npos; at = text.find(separator, at + 1))
            text.replace(at, 2, L"\n");
    return hstring(text);
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

constexpr auto kPlayerVolumeSettles = std::chrono::milliseconds(2000);

std::vector<hstring> rateItems() {
    std::vector<hstring> items{tr(L"Rate_FollowFile")};
    for (uint32_t rate : kFixedRates) {
        wchar_t text[32];
        swprintf(text, 32, L"%.1f kHz", rate / 1000.0);
        items.emplace_back(text);
    }
    return items;
}

int rateIndex(uint32_t rate) {
    for (size_t i = 0; i < std::size(kFixedRates); i++)
        if (kFixedRates[i] == rate) return int(i + 1);
    return 0;
}

uint32_t rateAt(int index) {
    return index > 0 && size_t(index) <= std::size(kFixedRates) ? kFixedRates[size_t(index) - 1] : 0;
}

std::vector<hstring> dsdRateItems() {
    std::vector<hstring> items;
    for (uint32_t rate : kDsdPcmRates) {
        wchar_t text[32];
        swprintf(text, 32, L"%.1f kHz", rate / 1000.0);
        items.emplace_back(text);
    }
    return items;
}

int dsdRateIndex(uint32_t rate) {
    const auto at = std::find(std::begin(kDsdPcmRates), std::end(kDsdPcmRates), rate);
    return at == std::end(kDsdPcmRates) ? 1 : int(at - std::begin(kDsdPcmRates)); // 176.4 kHz by default
}

uint32_t dsdRateAt(int index) {
    return kDsdPcmRates[std::clamp<size_t>(size_t(index), 0, std::size(kDsdPcmRates) - 1)];
}

const wchar_t* bypassTip(const camel::audio::OutputPlan& plan) {
    return plan.remodulate ? L"Badge_DspBypassedRemodulatedTip" : L"Badge_DspBypassedTip";
}

std::vector<hstring> maxDsdItems(hstring follow) {
    std::vector<hstring> items{std::move(follow)};
    for (uint32_t rate : kMaxDsdRates) items.emplace_back(L"DSD" + to_hstring(rate));
    return items;
}

int maxDsdIndex(uint32_t rate) {
    const auto at = std::find(std::begin(kMaxDsdRates), std::end(kMaxDsdRates), rate);
    return at == std::end(kMaxDsdRates) ? 0 : int(at - std::begin(kMaxDsdRates)) + 1;
}

uint32_t maxDsdAt(int index) {
    return index > 0 && size_t(index) <= std::size(kMaxDsdRates) ? kMaxDsdRates[size_t(index) - 1] : 0;
}

void showTiming(NumberBox const& predecode, NumberBox const& silence, NumberBox const& release, const PlaybackTiming& t) {
    predecode.Value(t.predecodeSeconds);
    silence.Value(t.formatChangeSilenceMs);
    release.Value(t.releaseAfterPauseSeconds);
}

void readTiming(NumberBox const& predecode, NumberBox const& silence, NumberBox const& release, PlaybackTiming& t) {
    t.predecodeSeconds = uint32_t(std::clamp(numberOr(predecode, t.predecodeSeconds), 1.0, 60.0));
    t.formatChangeSilenceMs = uint32_t(std::clamp(numberOr(silence, t.formatChangeSilenceMs), 0.0, 2000.0));
    t.releaseAfterPauseSeconds = uint32_t(std::clamp(numberOr(release, t.releaseAfterPauseSeconds), 0.0, 3600.0));
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
    std::wstring executablePath(32768, L'\0');
    const DWORD executablePathLength =
        ::GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (executablePathLength > 0 && executablePathLength < executablePath.size()) {
        executablePath.resize(executablePathLength);
        const auto iconPath = std::filesystem::path(executablePath).parent_path() / L"assets" / L"CamelPlayer.ico";
        std::error_code error;
        if (std::filesystem::exists(iconPath, error)) {
            AppWindow().SetIcon(iconPath.c_str());
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
    ui::setTheme(Content().as<FrameworkElement>().ActualTheme());
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
    // that moves only the position is left to it, and to nothing while the window is minimized or hidden.
    server_->setListener([dq, weak = get_weak()](const Event& event) {
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
    setButtonTip(StopButton(), L"Tip_Stop");
    setButtonTip(PreviousButton(), L"Tip_Previous");
    setButtonTip(NextButton(), L"Tip_Next");
    setButtonTip(ShuffleButton(), L"Tip_Shuffle");
    setButtonTip(RepeatButton(), L"Tip_RepeatOff");
    setButtonTip(QueueToggleButton(), L"Tip_Queue");
    for (const TextBlock note :
         {VolumeNote(), ReplayGainNote(), CrossfadeNote(), DspOrderNote(), EqNote(), CrossfeedNote(), ConvolutionNote()})
        foldNote(note);

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
    tray_ = std::make_unique<ui::TrayIcon>(ui::TrayIcon::Handlers{open, exit}, std::wstring(tr(L"Tray_Open")),
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
        auto& saved = ui::settingsToSave(settings_, editingOutput_ ? &editingOutput_->real : nullptr);
        saved.trayNoticeShown = true;
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = saved;
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
    if (StartWithWindowsSwitch().IsOn() && GetModuleFileNameW(nullptr, program, MAX_PATH))
        camel::startup::enable(program);
    else
        camel::startup::disable();
    showStartWithWindows();
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

void MainWindow::populateSettings() {
    populating_ = true;
    for (const auto& [combo, format] : decoderCombos()) {
        const auto registered = decodersFor(format);
        std::vector<hstring> names;
        names.reserve(registered.size());
        for (DecoderKind k : registered) names.push_back(ui::utf8(decoderName(k)));
        const auto chosen = std::find(registered.begin(), registered.end(), decoderFor(settings_.decoderByFormat, format));
        fill(combo, names, int(chosen - registered.begin()));
    }
    fill(ConvertCombo(), {tr(L"Convert_Off"), tr(L"Convert_On")}, settings_.convertWhenUnsupported ? 1 : 0);
    fill(ResampleQualityCombo(), {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")},
         int(settings_.resampleQuality));
    fill(ConvertDsdCombo(), {tr(L"Convert_Off"), tr(L"ConvertDsd_On")}, settings_.convertDsdWhenUnsupported ? 1 : 0);
    AutoplaySwitch().IsOn(settings_.autoplay);
    QueueAutoplayItem().IsChecked(settings_.autoplay);
    FolderPlaysOnSwitch().IsOn(settings_.folderPlaysOn);
    AutoUpdateSwitch().IsOn(settings_.checkUpdates);
    ArtistPicturesSwitch().IsOn(settings_.artistPictures == camel::OnlineLookup::Allowed);
    FolderTreeSwitch().IsOn(settings_.folderTree);
    ScanSpeedCombo().SelectedIndex(int(settings_.scanSpeed));
    WaveformSwitch().IsOn(settings_.waveform.enabled);
    AnalysisAnyTimeSwitch().IsOn(settings_.waveform.anyTime);
    AnalysisFromPicker().SelectedTime(Windows::Foundation::TimeSpan(std::chrono::minutes(settings_.waveform.fromMinute)));
    AnalysisToPicker().SelectedTime(Windows::Foundation::TimeSpan(std::chrono::minutes(settings_.waveform.toMinute)));
    AnalysisFromPicker().IsEnabled(!settings_.waveform.anyTime);
    AnalysisToPicker().IsEnabled(!settings_.waveform.anyTime);
    DiscordSwitch().IsOn(settings_.discord.enabled);
    DiscordCoversSwitch().IsOn(settings_.discord.covers);
    DiscordCoversSwitch().IsEnabled(settings_.discord.enabled);
    get_self<LibraryView>(Library())->setFolderTree(settings_.folderTree);
    get_self<LibraryView>(Library())->setFolderPlaysOn(settings_.folderPlaysOn);
    ui::trackColumns().columns(settings_.trackColumns);
    showDsp();
    showVolume();
    showReplayGain();
    showCrossfade();
    applyHotkeys();

    fill(WasapiShareCombo(), {tr(L"Share_Exclusive"), tr(L"Share_Shared")}, settings_.wasapi.shared ? 1 : 0);
    fill(WasapiSharedQualityCombo(), {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")},
         int(settings_.wasapi.sharedResampleQuality));
    fill(WasapiModeCombo(), {tr(L"Mode_Event"), tr(L"Mode_Polling")}, settings_.wasapi.eventDriven ? 0 : 1);
    fill(WasapiPeriodCombo(), {tr(L"Period_Default"), tr(L"Period_Minimum"), tr(L"Period_Custom")}, int(settings_.wasapi.period));
    WasapiPeriodBox().Value(settings_.wasapi.customPeriodMs);
    const std::vector<hstring> formats = {tr(L"Format_Auto"), L"16", L"24", L"24-in-32", L"32"};
    fill(WasapiFormatCombo(), formats, formatIndex(settings_.wasapi.sampleType));
    const auto rates = rateItems();
    const std::vector<hstring> tasks = {L"Pro Audio", L"Playback", tr(L"Mmcss_Disabled")};
    const std::vector<hstring> priorities = {tr(L"Priority_Normal"), tr(L"Priority_High"), tr(L"Priority_Critical")};
    fill(WasapiRateCombo(), rates, rateIndex(settings_.wasapi.fixedRate));
    // WASAPI and KS have no native DSD; a stored Native plays as DoP.
    const std::vector<hstring> dopOrPcm = {tr(L"Dsd_Dop"), tr(L"Dsd_Pcm")};
    const auto dsdRates = dsdRateItems();
    fill(WasapiDsdCombo(), dopOrPcm, settings_.wasapi.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(WasapiDsdRateCombo(), dsdRates, dsdRateIndex(settings_.wasapi.dsdPcmRate));
    auto deviceLimits = rates;
    deviceLimits.front() = tr(L"Limit_FollowDevice");
    const auto deviceDsdLimits = maxDsdItems(tr(L"Limit_FollowDevice"));
    fill(WasapiMaxRateCombo(), deviceLimits, rateIndex(settings_.wasapi.maxSampleRate));
    fill(WasapiMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(settings_.wasapi.maxDsdRate));
    WasapiPollBufferBox().Value(settings_.wasapi.pollBufferMs);
    fill(WasapiMmcssTaskCombo(), tasks, int(settings_.wasapi.mmcssTask));
    fill(WasapiMmcssPriorityCombo(), priorities, int(settings_.wasapi.mmcssPriority));
    showTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), settings_.wasapi.timing);

    AsioBufferBox().Value(double(settings_.asio.bufferSize));
    AsioLeftBox().Value(settings_.asio.outLeft + 1);
    AsioRightBox().Value(settings_.asio.outRight + 1);
    fill(AsioRateCombo(), rates, rateIndex(settings_.asio.fixedRate));
    fill(AsioDsdCombo(), {tr(L"Dsd_Native"), tr(L"Dsd_Dop"), tr(L"Dsd_Pcm")}, int(settings_.asio.dsdOutput));
    fill(AsioDsdRateCombo(), dsdRates, dsdRateIndex(settings_.asio.dsdPcmRate));
    fill(AsioMaxRateCombo(), deviceLimits, rateIndex(settings_.asio.maxSampleRate));
    fill(AsioMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(settings_.asio.maxDsdRate));
    showTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), settings_.asio.timing);

    fill(KsModeCombo(), {tr(L"KsMode_Auto"), tr(L"KsMode_RtEvent"), tr(L"KsMode_RtPoll"), tr(L"KsMode_Standard")},
         int(settings_.ks.mode));
    KsBufferBox().Value(settings_.ks.bufferMs);
    KsPacketCountBox().Value(settings_.ks.packetCount);
    KsPacketMsBox().Value(settings_.ks.packetMs);
    fill(KsFormatCombo(), formats, formatIndex(settings_.ks.sampleType));
    fill(KsRateCombo(), rates, rateIndex(settings_.ks.fixedRate));
    fill(KsDsdCombo(), dopOrPcm, settings_.ks.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(KsDsdRateCombo(), dsdRates, dsdRateIndex(settings_.ks.dsdPcmRate));
    fill(KsMaxRateCombo(), deviceLimits, rateIndex(settings_.ks.maxSampleRate));
    fill(KsMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(settings_.ks.maxDsdRate));
    fill(KsMmcssTaskCombo(), tasks, int(settings_.ks.mmcssTask));
    fill(KsMmcssPriorityCombo(), priorities, int(settings_.ks.mmcssPriority));
    showTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), settings_.ks.timing);

    auto limits = rates;
    limits.front() = tr(L"Limit_FollowPlayer");
    fill(NetworkRateCombo(), limits, rateIndex(settings_.network.maxSampleRate));
    fill(NetworkBitsCombo(), {tr(L"Limit_FollowPlayer"), L"16"}, settings_.network.maxBits ? 1 : 0);
    fill(NetworkDsdCombo(), {tr(L"Dsd_Dsf"), tr(L"Dsd_Pcm")}, settings_.network.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(NetworkDsdRateCombo(), dsdRates, dsdRateIndex(settings_.network.dsdPcmRate));
    fill(NetworkMaxDsdCombo(), maxDsdItems(tr(L"Limit_FollowPlayer")), maxDsdIndex(settings_.network.maxDsdRate));
    NetworkGaplessSwitch().IsOn(settings_.network.gapless);
    showTiming(NetworkPredecodeBox(), NetworkSilenceBox(), NetworkReleaseBox(), settings_.network.timing);
    fill(AirPlayChannelsCombo(), {tr(L"Channels_Stereo"), tr(L"Channels_Left"), tr(L"Channels_Right")},
         int(settings_.airplay.channels));

    const int lang = settings_.language == "zh-TW" ? 1 : settings_.language == "en-US" ? 2 : 0;
    fill(LanguageCombo(), {tr(L"Lang_System"), L"繁體中文", L"English"}, lang);
    fill(CloseActionCombo(), {tr(L"Close_Tray"), tr(L"Close_Exit")}, settings_.closeAction == CloseAction::Exit ? 1 : 0);
    showStartWithWindows();
    fill(ThemeCombo(), {tr(L"Theme_System"), tr(L"Theme_Light"), tr(L"Theme_Dark")}, int(settings_.theme));
    populateOutputs();
    populating_ = false;
    updateBackendGroups();
    applyTheme();
}

namespace {

hstring outputLabel(const camel::server::OutputRow& row) {
    if (!row.alias.empty()) return ui::utf8(row.alias);
    if (row.id.empty()) return tr(L"Device_Default");
    return row.name.empty() ? tr(L"Device_NotConnected") : ui::utf8(row.name);
}

// The same DAC through WASAPI and through ASIO are two entries, told apart by the backend.
hstring outputMenuLabel(const camel::server::OutputRow& row) {
    const hstring label = outputLabel(row);
    if (!row.alias.empty() || row.id.empty()) return label;
    return label + L" (" + ui::utf8(camel::backendName(row.backend)) + L")";
}

const wchar_t* unsupportedReason(const std::string& reason) {
    if (reason == "encrypted") return L"Output_AirPlayEncrypted";
    if (reason == "password") return L"Output_AirPlayPassword";
    if (reason == "format") return L"Output_AirPlayFormat";
    return L"Output_AirPlayRefused";
}

} // namespace

void MainWindow::populateOutputs() {
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto inUse = std::ranges::find_if(rows, &camel::server::OutputRow::selected);
    if (inUse != rows.end()) {
        ToolTipService::SetToolTip(OutputButton(), box_value(outputMenuLabel(*inUse)));
        Automation::AutomationProperties::SetName(OutputButton(),
                                                  ui::format(L"Output_ButtonName", {std::wstring(outputMenuLabel(*inUse))}));
    }
    auto list = OutputList().Children();
    list.Clear();
    StackPanel groupRows{nullptr};
    std::optional<Backend> group;
    for (const auto& row : rows) {
        const bool player = camel::isNetworkPlayer(row.backend);
        if (const Backend rowGroup = player ? Backend::Network : row.backend; rowGroup != group) {
            group = rowGroup;
            // A group in use stays open; one the user opened or closed stays so until the program restarts.
            const bool used = std::ranges::any_of(rows, [&](const camel::server::OutputRow& r) {
                return (camel::isNetworkPlayer(r.backend) ? Backend::Network : r.backend) == rowGroup &&
                       (r.enabled || r.selected);
            });
            const auto remembered = outputGroupsOpen_.find(rowGroup);
            Expander expander;
            expander.Header(box_value(player ? tr(L"Output_NetworkGroup") : ui::utf8(camel::backendName(row.backend))));
            expander.HorizontalAlignment(HorizontalAlignment::Stretch);
            expander.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            expander.IsExpanded(remembered == outputGroupsOpen_.end() ? used : remembered->second);
            expander.Expanding([this, rowGroup](auto&&, auto&&) { outputGroupsOpen_[rowGroup] = true; });
            expander.Collapsed([this, rowGroup](auto&&, auto&&) { outputGroupsOpen_[rowGroup] = false; });
            groupRows = StackPanel{};
            groupRows.Spacing(4);
            expander.Content(groupRows);
            list.Append(expander);
        }
        const hstring label = row.backend == Backend::AirPlay ? outputMenuLabel(row) : outputLabel(row);
        Grid line;
        line.ColumnSpacing(8);
        for (const GridLength width : {GridLength{1, GridUnitType::Star}, GridLength{1, GridUnitType::Auto},
                                       GridLength{1, GridUnitType::Auto}, GridLength{1, GridUnitType::Auto}}) {
            ColumnDefinition column;
            column.Width(width);
            line.ColumnDefinitions().Append(column);
        }
        if (row.selected) { // the output in use stands out of a long list
            line.Background(themeBrush(L"SubtleFillColorSecondaryBrush"));
            line.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            line.Padding(ThicknessHelper::FromLengths(8, 4, 0, 4));
        }
        StackPanel names;
        names.VerticalAlignment(VerticalAlignment::Center);
        TextBlock name;
        name.Text(label);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        if (row.selected) name.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        names.Children().Append(name);
        if (row.selected || !row.connected || !row.unsupported.empty()) {
            TextBlock state;
            state.Text(tr(row.selected     ? L"Output_InUse"
                          : !row.connected ? L"Device_NotConnected"
                                           : unsupportedReason(row.unsupported)));
            state.TextWrapping(TextWrapping::Wrap);
            state.Style(unbox_value<Microsoft::UI::Xaml::Style>(
                Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
            names.Children().Append(state);
        }
        line.Children().Append(names);

        MenuFlyout actions;
        MenuFlyoutItem rename;
        rename.Text(tr(L"Output_Rename"));
        rename.Click([this, row, label](auto&&, auto&&) { askRenameOutput(row.backend, row.id, row.alias, label); });
        actions.Items().Append(rename);
        if (row.saved) {
            MenuFlyoutItem remove;
            remove.Text(tr(L"Output_Delete"));
            remove.Click([this, row, label](auto&&, auto&&) { askDeleteOutput(row.backend, row.id, label); });
            actions.Items().Append(remove);
        }
        if (row.backend != Backend::AirPlay || !row.pair) { // a stereo pair has nothing to set
            Button gear;
            FontIcon gearIcon;
            gearIcon.Glyph(L"\xE713");
            gearIcon.FontSize(14);
            gear.Content(gearIcon);
            gear.VerticalAlignment(VerticalAlignment::Center);
            ToolTipService::SetToolTip(gear, box_value(tr(L"Output_Settings")));
            Automation::AutomationProperties::SetName(
                gear, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_Settings")), std::wstring(label)}));
            gear.Click([this, row, label](auto&&, auto&&) { showOutputSettings(row.backend, row.id, label); });
            Grid::SetColumn(gear, 1);
            line.Children().Append(gear);
        }
        Button more;
        FontIcon moreIcon;
        moreIcon.Glyph(L"\xE712");
        moreIcon.FontSize(14);
        more.Content(moreIcon);
        more.Flyout(actions);
        more.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(more, box_value(tr(L"Output_More")));
        Automation::AutomationProperties::SetName(
            more, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_More")), std::wstring(label)}));
        Grid::SetColumn(more, 2);
        line.Children().Append(more);

        ToggleSwitch enabled;
        enabled.IsOn(row.enabled);
        enabled.IsEnabled(!row.selected && row.unsupported.empty()); // the output in use stays in the menu
        enabled.OnContent(box_value(L""));
        enabled.OffContent(box_value(L""));
        enabled.MinWidth(0);
        enabled.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(enabled, box_value(tr(L"Output_Enabled")));
        Automation::AutomationProperties::SetName(
            enabled, ui::format(L"Output_ButtonOn", {std::wstring(tr(L"Output_Enabled")), std::wstring(label)}));
        enabled.Toggled([this, row](Windows::Foundation::IInspectable const& sender, auto&&) {
            const bool on = sender.as<ToggleSwitch>().IsOn(); // not the switch itself, which the handler would keep
            changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                camel::server::setOutputEnabled(s, row.backend, row.id, on, devices);
            });
            if (camel::isNetworkPlayer(row.backend) && on) askFirewallRule();
        });
        Grid::SetColumn(enabled, 3);
        line.Children().Append(enabled);
        groupRows.Children().Append(line);
    }
    const bool players = std::ranges::any_of(
        rows, [](const camel::server::OutputRow& r) { return camel::isNetworkPlayer(r.backend) && r.enabled; });
    FirewallNotice().IsOpen(players && !camel::server::firewallRuleExists());
}

winrt::fire_and_forget MainWindow::addFirewallRule() {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    camel::server::addFirewallRule();
    queue.TryEnqueue([weak] {
        if (auto self = weak.get()) self->populateOutputs();
    });
}

// The first player enabled says why before Windows asks for an administrator; declining leaves the notice with its fix.
winrt::fire_and_forget MainWindow::askFirewallRule() {
    auto strong = get_strong();
    if (camel::server::firewallRuleExists()) co_return;
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Firewall_Title")));
    TextBlock explanation;
    explanation.Text(tr(L"Firewall_Text"));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(tr(L"Firewall_AddButton/Content"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) == ContentDialogResult::Primary)
        addFirewallRule();
    else
        populateOutputs();
}

void MainWindow::OnAddFirewallRuleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    addFirewallRule();
}

void MainWindow::changeOutputs(std::function<void(camel::Settings&, const DeviceCatalog&)> const& change) {
    auto settings = nativeSettings(settings_);
    change(settings, server_->snapshot().devices);
    settings_ = settingsDto(settings);
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
}

// Any output's parameters, in use or not, in the controls of the settings page; changes apply as they are made.
winrt::fire_and_forget MainWindow::showOutputSettings(Backend backend, std::string id, hstring label) {
    auto strong = get_strong();
    const auto real = nativeSettings(settings_);
    const auto rows = camel::server::outputRows(real, server_->snapshot().devices);
    const bool inUse = std::ranges::any_of(
        rows, [&](const camel::server::OutputRow& r) { return r.selected && r.backend == backend && r.id == id; });
    editingOutput_ = OutputEdit{backend, id, settings_};
    settings_ = settingsDto(camel::server::withOutputParameters(real, backend, id, server_->snapshot().devices));
    populateSettings();
    updateBackendGroups();
    AsioPanelButton().IsEnabled(inUse); // the driver's panel opens for the driver in use
    const auto panel = OutputSettingsPanel();
    OutputSettingsHost().Child(nullptr);
    ScrollViewer scroller;
    scroller.Content(panel);
    panel.Padding(ThicknessHelper::FromLengths(0, 0, 16, 0));
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(label));
    dialog.Content(scroller);
    dialog.CloseButtonText(tr(L"Output_SettingsClose"));
    co_await ui::showDialog(dialog);
    scroller.Content(nullptr);
    OutputSettingsHost().Child(panel);
    settings_ = editingOutput_->real;
    editingOutput_.reset();
    AsioPanelButton().IsEnabled(true);
    populateSettings();
    updateBackendGroups();
}

winrt::fire_and_forget MainWindow::askRenameOutput(Backend backend, std::string id, std::string alias, hstring label) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_RenameTitle", {std::wstring(label)})));
    TextBox name;
    name.Text(ui::utf8(alias));
    name.Header(box_value(tr(L"Output_RenameNote")));
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Output_Rename"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    std::wstring text(name.Text());
    text.erase(0, text.find_first_not_of(L" \t"));
    text.erase(text.find_last_not_of(L" \t") + 1);
    changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
        camel::server::renameOutput(s, backend, id, toUtf8(text), devices);
    });
    populateOutputs();
}

winrt::fire_and_forget MainWindow::askDeleteOutput(Backend backend, std::string id, hstring label) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(ui::format(L"Output_DeleteTitle", {std::wstring(label)})));
    TextBlock explanation;
    explanation.Text(tr(L"Output_DeleteText"));
    explanation.TextWrapping(TextWrapping::Wrap);
    dialog.Content(explanation);
    dialog.PrimaryButtonText(tr(L"Output_Delete"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    changeOutputs(
        [&](camel::Settings& s, const DeviceCatalog& devices) { camel::server::deleteOutput(s, backend, id, devices); });
    populateSettings();
}

// A new menu at every opening, and the mouse chooses on its press: WinUI at times leaves a menu where a click only
// draws the item pressed, for the rest of the session, while the keyboard still chooses.
void MainWindow::OnOutputButtonClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    MenuFlyout menu;
    menu.Placement(Primitives::FlyoutPlacementMode::Top);
    const winrt::weak_ref<MenuFlyout> weakMenu = menu;
    const auto chosen = std::make_shared<bool>(false);
    const auto add = [&](MenuFlyoutItem const& item, std::function<void()> choose) {
        const auto once = [weakMenu, chosen, choose = std::move(choose)] {
            if (*chosen) return;
            *chosen = true;
            if (const auto shown = weakMenu.get()) shown.Hide();
            choose();
        };
        item.Click([once](auto&&, auto&&) { once(); });
        item.AddHandler(UIElement::PointerPressedEvent(),
                        box_value(Input::PointerEventHandler([once](auto&&, Input::PointerRoutedEventArgs const& e) {
                            if (e.Pointer().PointerDeviceType() == Microsoft::UI::Input::PointerDeviceType::Mouse &&
                                e.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed())
                                once();
                        })),
                        true);
        menu.Items().Append(item);
    };
    const auto rows = camel::server::outputRows(nativeSettings(settings_), server_->snapshot().devices);
    const auto grouped = std::ranges::count_if(rows, &camel::server::OutputRow::grouped);
    for (const auto& row : rows) {
        if (!row.enabled) continue;
        if (row.backend == Backend::AirPlay && settings_.backend == Backend::AirPlay) {
            // Receivers join and leave the group in use; the last one stays.
            ToggleMenuFlyoutItem item;
            item.Text(row.connected ? outputMenuLabel(row) : ui::format(L"Output_Offline", {std::wstring(outputMenuLabel(row))}));
            item.IsChecked(row.grouped);
            item.IsEnabled(row.connected && !(row.grouped && grouped < 2));
            add(item, [this, row] {
                changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                    camel::server::setInAirPlayGroup(s, row.id, !row.grouped, devices);
                });
                populateSettings();
            });
            continue;
        }
        RadioMenuFlyoutItem item;
        item.GroupName(L"Outputs");
        item.Text(row.connected ? outputMenuLabel(row) : ui::format(L"Output_Offline", {std::wstring(outputMenuLabel(row))}));
        item.IsChecked(row.selected);
        item.IsEnabled(row.connected);
        add(item, [this, row] {
            if (row.selected) return;
            changeOutputs([&](camel::Settings& s, const DeviceCatalog& devices) {
                camel::server::selectOutput(s, row.backend, row.id, devices);
            });
            populateSettings();
        });
    }
    menu.Items().Append(MenuFlyoutSeparator());
    MenuFlyoutItem open;
    open.Text(tr(L"Output_MenuSettings"));
    add(open, [this] { showSettingsCard(OutputsCard()); });
    menu.ShowAt(OutputButton());
}

std::array<std::pair<ComboBox, AudioFormat>, 3> MainWindow::decoderCombos() {
    return {
        {{DecoderCombo(), AudioFormat::Flac}, {WavDecoderCombo(), AudioFormat::Wav}, {AiffDecoderCombo(), AudioFormat::Aiff}}};
}

void MainWindow::readSettingsFromUi() {
    auto idx = [](ComboBox const& c) { return std::max(0, c.SelectedIndex()); };
    for (const auto& [combo, format] : decoderCombos()) {
        const auto registered = decodersFor(format);
        const DecoderKind chosen = registered[std::min<size_t>(idx(combo), registered.size() - 1)];
        if (chosen != decoderFor(settings_.decoderByFormat, format)) settings_.decoderByFormat[format] = chosen;
    }
    settings_.convertWhenUnsupported = idx(ConvertCombo()) == 1;
    settings_.resampleQuality = ResampleQuality(std::min(idx(ResampleQualityCombo()), 2));
    settings_.convertDsdWhenUnsupported = idx(ConvertDsdCombo()) == 1;
    settings_.autoplay = AutoplaySwitch().IsOn();
    settings_.folderPlaysOn = FolderPlaysOnSwitch().IsOn();
    settings_.checkUpdates = AutoUpdateSwitch().IsOn();
    // Off stays unanswered until it was on, so the artist page still asks.
    if (ArtistPicturesSwitch().IsOn())
        settings_.artistPictures = camel::OnlineLookup::Allowed;
    else if (settings_.artistPictures == camel::OnlineLookup::Allowed)
        settings_.artistPictures = camel::OnlineLookup::Refused;
    settings_.folderTree = FolderTreeSwitch().IsOn();
    if (const int speed = ScanSpeedCombo().SelectedIndex(); speed >= 0) settings_.scanSpeed = camel::ScanSpeed(speed);
    settings_.waveform.enabled = WaveformSwitch().IsOn();
    settings_.waveform.anyTime = AnalysisAnyTimeSwitch().IsOn();
    const auto minuteOf = [](TimePicker const& picker, int fallback) {
        const auto time = picker.SelectedTime();
        return time ? int(std::chrono::duration_cast<std::chrono::minutes>(time.Value()).count()) % 1440 : fallback;
    };
    settings_.waveform.fromMinute = minuteOf(AnalysisFromPicker(), settings_.waveform.fromMinute);
    settings_.waveform.toMinute = minuteOf(AnalysisToPicker(), settings_.waveform.toMinute);
    AnalysisFromPicker().IsEnabled(!settings_.waveform.anyTime);
    AnalysisToPicker().IsEnabled(!settings_.waveform.anyTime);
    if (!settings_.waveform.enabled) waveformBar_->clear();
    settings_.discord.enabled = DiscordSwitch().IsOn();
    settings_.discord.covers = DiscordCoversSwitch().IsOn();
    settings_.eq.enabled = EqSwitch().IsOn();
    if (eqEditor_) settings_.eq.bands = eqEditor_->settings().bands;
    settings_.crossfeed.enabled = CrossfeedSwitch().IsOn();
    settings_.convolution.enabled = ConvolutionSwitch().IsOn();
    settings_.volume.mode = chosenVolumeMode();
    settings_.volume.db = std::round(VolumeSlider().Value() * 2) / 2;
    auto& rg = settings_.replayGain;
    if (ReplayGainModeCombo().SelectedIndex() >= 0) rg.mode = camel::ReplayGainMode(ReplayGainModeCombo().SelectedIndex());
    const auto preamp = [](double value, double fallback) {
        return std::isnan(value) ? fallback
                                 : std::clamp(std::round(value * 10) / 10, camel::ReplayGainSettings::kMinPreampDb,
                                              camel::ReplayGainSettings::kMaxPreampDb);
    };
    rg.preampDb = preamp(ReplayGainPreampBox().Value(), rg.preampDb);
    rg.noValuePreampDb = preamp(ReplayGainNoValueBox().Value(), rg.noValuePreampDb);
    rg.preventClipping = ReplayGainClipSwitch().IsOn();
    settings_.crossfade.enabled = CrossfadeSwitch().IsOn();
    if (const double seconds = CrossfadeSecondsBox().Value(); !std::isnan(seconds))
        settings_.crossfade.seconds =
            std::clamp(std::round(seconds * 2) / 2, camel::CrossfadeSettings::kMinSeconds, camel::CrossfadeSettings::kMaxSeconds);

    settings_.wasapi.shared = idx(WasapiShareCombo()) == 1;
    settings_.wasapi.sharedResampleQuality = ResampleQuality(std::min(idx(WasapiSharedQualityCombo()), 2));
    settings_.wasapi.eventDriven = idx(WasapiModeCombo()) == 0;
    settings_.wasapi.period = WasapiPeriod(std::min(idx(WasapiPeriodCombo()), 2));
    settings_.wasapi.customPeriodMs = numberOr(WasapiPeriodBox(), settings_.wasapi.customPeriodMs);
    settings_.wasapi.sampleType = kFormatChoices[std::min(idx(WasapiFormatCombo()), 4)];
    settings_.wasapi.fixedRate = rateAt(idx(WasapiRateCombo()));
    settings_.wasapi.dsdOutput = idx(WasapiDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Dop;
    settings_.wasapi.dsdPcmRate = dsdRateAt(idx(WasapiDsdRateCombo()));
    settings_.wasapi.maxSampleRate = rateAt(idx(WasapiMaxRateCombo()));
    settings_.wasapi.maxDsdRate = maxDsdAt(idx(WasapiMaxDsdCombo()));
    settings_.wasapi.pollBufferMs = std::clamp(numberOr(WasapiPollBufferBox(), settings_.wasapi.pollBufferMs), 20.0, 2000.0);
    settings_.wasapi.mmcssTask = MmcssTask(std::min(idx(WasapiMmcssTaskCombo()), 2));
    settings_.wasapi.mmcssPriority = MmcssPriority(std::min(idx(WasapiMmcssPriorityCombo()), 2));
    readTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), settings_.wasapi.timing);

    settings_.asio.bufferSize = long(numberOr(AsioBufferBox(), double(settings_.asio.bufferSize)));
    settings_.asio.outLeft = int(numberOr(AsioLeftBox(), settings_.asio.outLeft + 1)) - 1;
    settings_.asio.outRight = int(numberOr(AsioRightBox(), settings_.asio.outRight + 1)) - 1;
    settings_.asio.fixedRate = rateAt(idx(AsioRateCombo()));
    settings_.asio.dsdOutput = DsdOutput(std::min(idx(AsioDsdCombo()), 2));
    settings_.asio.dsdPcmRate = dsdRateAt(idx(AsioDsdRateCombo()));
    settings_.asio.maxSampleRate = rateAt(idx(AsioMaxRateCombo()));
    settings_.asio.maxDsdRate = maxDsdAt(idx(AsioMaxDsdCombo()));
    readTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), settings_.asio.timing);

    settings_.ks.mode = KsMode(std::min(idx(KsModeCombo()), 3));
    settings_.ks.bufferMs = numberOr(KsBufferBox(), settings_.ks.bufferMs);
    settings_.ks.packetCount = int(numberOr(KsPacketCountBox(), settings_.ks.packetCount));
    settings_.ks.packetMs = numberOr(KsPacketMsBox(), settings_.ks.packetMs);
    settings_.ks.sampleType = kFormatChoices[std::min(idx(KsFormatCombo()), 4)];
    settings_.ks.fixedRate = rateAt(idx(KsRateCombo()));
    settings_.ks.dsdOutput = idx(KsDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Dop;
    settings_.ks.dsdPcmRate = dsdRateAt(idx(KsDsdRateCombo()));
    settings_.ks.maxSampleRate = rateAt(idx(KsMaxRateCombo()));
    settings_.ks.maxDsdRate = maxDsdAt(idx(KsMaxDsdCombo()));
    settings_.ks.mmcssTask = MmcssTask(std::min(idx(KsMmcssTaskCombo()), 2));
    settings_.ks.mmcssPriority = MmcssPriority(std::min(idx(KsMmcssPriorityCombo()), 2));
    readTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), settings_.ks.timing);

    settings_.network.maxSampleRate = rateAt(idx(NetworkRateCombo()));
    settings_.network.maxBits = idx(NetworkBitsCombo()) == 1 ? 16 : 0;
    settings_.network.dsdOutput = idx(NetworkDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Native;
    settings_.network.dsdPcmRate = dsdRateAt(idx(NetworkDsdRateCombo()));
    settings_.network.maxDsdRate = maxDsdAt(idx(NetworkMaxDsdCombo()));
    settings_.network.gapless = NetworkGaplessSwitch().IsOn();
    readTiming(NetworkPredecodeBox(), NetworkSilenceBox(), NetworkReleaseBox(), settings_.network.timing);
    settings_.airplay.channels = AirPlayChannels(std::clamp(idx(AirPlayChannelsCombo()), 0, 2));
    settings_.closeAction = idx(CloseActionCombo()) == 1 ? CloseAction::Exit : CloseAction::Tray;
    settings_.theme = Theme(std::clamp(idx(ThemeCombo()), 0, 2));
}

void MainWindow::OnAutoplayToggled(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    if (populating_) return;
    const bool on = sender.as<ToggleSwitch>().IsOn();
    populating_ = true;
    AutoplaySwitch().IsOn(on);
    QueueAutoplayItem().IsChecked(on);
    populating_ = false;
    settingsChanged();
}

void MainWindow::OnNetworkGaplessToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnQueueAutoplayClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    AutoplaySwitch().IsOn(QueueAutoplayItem().IsChecked());
}

void MainWindow::settingsChanged() {
    if (populating_ || !server_) return;
    readSettingsFromUi();
    if (editingOutput_) {
        auto real = nativeSettings(editingOutput_->real);
        camel::server::keepOutputParameters(real, nativeSettings(settings_), editingOutput_->backend, editingOutput_->id,
                                            server_->snapshot().devices);
        editingOutput_->real = settingsDto(real);
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = editingOutput_->real;
        submitCommand(std::move(command));
        updateBackendGroups();
        return;
    }
    applyTheme();
    get_self<LibraryView>(Library())->setFolderTree(settings_.folderTree);
    get_self<LibraryView>(Library())->setFolderPlaysOn(settings_.folderPlaysOn);
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    updateBackendGroups();
}

void MainWindow::updateBackendGroups() {
    WasapiGroup().Visibility(settings_.backend == Backend::Wasapi ? Visibility::Visible : Visibility::Collapsed);
    AsioGroup().Visibility(settings_.backend == Backend::Asio ? Visibility::Visible : Visibility::Collapsed);
    KsGroup().Visibility(settings_.backend == Backend::Ks ? Visibility::Visible : Visibility::Collapsed);
    NetworkGroup().Visibility(settings_.backend == Backend::Network ? Visibility::Visible : Visibility::Collapsed);
    AirPlayGroup().Visibility(settings_.backend == Backend::AirPlay ? Visibility::Visible : Visibility::Collapsed);
    WasapiPeriodBox().IsEnabled(settings_.wasapi.period == WasapiPeriod::Custom);
    WasapiRateCombo().IsEnabled(!settings_.wasapi.shared); // the mixer sets the rate in shared mode
    WasapiMaxRateCombo().IsEnabled(!settings_.wasapi.shared);
    WasapiMaxDsdCombo().IsEnabled(!settings_.wasapi.shared);
    WasapiPollBufferBox().IsEnabled(!settings_.wasapi.eventDriven);
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
        text = tr(L"Library_Scanning");
        for (const auto& [marker, n] : {std::pair{L"{0}", progress.checked}, std::pair{L"{1}", progress.found}})
            if (const auto at = text.find(marker); at != std::wstring::npos) text.replace(at, 3, ui::count(int64_t(n)));
    }
    if (text == shownScan_) return;
    shownScan_ = hstring(text);
    ScanText().Text(shownScan_);
    ScanText().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::submitCommand(Command command) {
    if (!server_) return;
    // Lists play on a single click, so a double click sends the same play twice; the second would only restart the
    // track it has just started, cutting its crossfade.
    if (command.kind == CommandKind::QueueReplace || command.kind == CommandKind::OpenAndPlay) {
        const TrackSource* chosen = command.source                           ? &*command.source
                                    : command.index < command.sources.size() ? &command.sources[command.index]
                                                                             : nullptr;
        PlayRequest play{command.kind, command.index, command.sources.size(), chosen ? chosen->pathUtf8() : std::string{}};
        const auto now = std::chrono::steady_clock::now();
        const bool repeated = play == lastPlay_ && now - lastPlayAt_ < std::chrono::milliseconds(GetDoubleClickTime());
        lastPlay_ = std::move(play);
        lastPlayAt_ = now;
        if (repeated) return;
    }
    cursor_.expect(server_->submit(std::move(command)));
}

bool MainWindow::movesOnlyThePosition(const Snapshot& snapshot) const {
    const auto &now = snapshot.playback, &before = shown_.playback;
    return snapshot.queueEntryId == shown_.queueEntryId && snapshot.queue.revision == shown_.queue.revision &&
           snapshot.queue.marks == shown_.queue.marks && snapshot.queue.upcomingEntryId == shown_.queue.upcomingEntryId &&
           snapshot.devices.revision == shown_.devices.revision && snapshot.commandError == shown_.commandError &&
           now.revision == before.revision && now.state == before.state && now.pathUtf8 == before.pathUtf8 &&
           now.total == before.total && now.underruns == before.underruns;
}

// A device scan can change what the Server keeps: the outputs, and the parameters when the Windows default moved on.
void MainWindow::showOutputsOf(const SettingsDto& server) {
    switch (ui::followServerOutputs(settings_, editingOutput_ ? &editingOutput_->real : nullptr, server)) {
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
    if (snapshot.playback.playerVolume != playerVolume_ || snapshot.playback.playerMuted != playerMuted_) {
        playerVolume_ = snapshot.playback.playerVolume;
        playerMuted_ = snapshot.playback.playerMuted;
        showVolume();
    }
    if (!server_ || !cursor_.accept(snapshot)) return;
    shown_ = snapshot;
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
        const hstring why = ui::playerErrorTitle(left->code);
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
    const uint32_t rate = s.source ? s.source->sampleRate : 0;
    const double pos = rate ? framesToSeconds(s.position, rate) : 0.0;
    const double total = rate ? framesToSeconds(s.total, rate) : 0.0;
    if (s.pathUtf8 != lyricsPath_) loadLyrics(s.pathUtf8);
    followLyrics(pos);

    // The play button and the repeat button are named for what they do or show now.
    if (const Symbol symbol = s.state == PlaybackState::Playing || s.bufferingSeconds ? Symbol::Pause : Symbol::Play;
        PlayPauseIcon().Symbol() != symbol) {
        PlayPauseIcon().Symbol(symbol);
        const hstring tip = tr(symbol == Symbol::Pause ? L"Tip_Pause" : L"Tip_Play");
        setBadgeTip(PlayPauseButton(), tip, tip);
    }
    // The waveform appears once the Server has it: made at once for the playing track, asked for every 2 seconds.
    if (s.pathUtf8 != waveformPath_) {
        waveformPath_ = s.pathUtf8;
        waveformBar_->clear();
        waveformAsked_ = {};
    }
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
    const bool hasTrack = !s.pathUtf8.empty();
    PlayPauseButton().IsEnabled(hasTrack);
    StopButton().IsEnabled(hasTrack);
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
        std::wstring count(tr(windowed ? L"Queue_CountWindowed" : L"Library_Tracks"));
        if (const auto at = count.find(L"{0}"); at != std::wstring::npos)
            count.replace(at, 3, ui::count(int64_t(snapshot.queue.entries.size())));
        if (const auto at = count.find(L"{1}"); at != std::wstring::npos)
            count.replace(at, 3, ui::count(int64_t(ui::QueueCollection::kShown)));
        QueueCountText().Text(count);
        QueueEmptyText().Visibility(snapshot.queue.entries.empty() ? Visibility::Visible : Visibility::Collapsed);
        std::vector<ui::QueueRow> rows;
        rows.reserve(snapshot.queue.entries.size());
        for (const auto& entry : snapshot.queue.entries)
            rows.push_back({entry.id,
                            entry.title.empty() ? hstring(std::filesystem::path(toWide(entry.source.pathUtf8())).stem().wstring())
                                                : ui::utf8(entry.title),
                            ui::utf8(entry.artist), ui::utf8(entry.source.pathUtf8()), entry.automatic, entry.file,
                            entry.favorite});
        queueItems_->update(std::move(rows), snapshot.queueEntryId);
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

    // Playing, paused, stopped or failed, never only implied by the play button's icon
    {
        const wchar_t* glyph = L"\xE71A";
        const wchar_t* key = L"State_Stopped";
        const wchar_t* color = L"TextFillColorSecondaryBrush";
        hstring text;
        buffering_ = !s.error && s.bufferingSeconds;
        if (s.error) {
            glyph = L"\xE783";
            key = L"State_Error";
            color = L"SystemFillColorCriticalBrush";
        } else if (buffering_) {
            glyph = L"\xE916"; // stopwatch
            key = L"State_Buffering";
            color = L"AccentTextFillColorPrimaryBrush";
            text = ui::format(key, {std::to_wstring(*s.bufferingSeconds)});
        } else if (s.state == PlaybackState::Playing) {
            glyph = L"\xE768";
            key = L"State_Playing";
            color = L"AccentTextFillColorPrimaryBrush";
        } else if (s.state == PlaybackState::Paused) {
            glyph = L"\xE769";
            key = L"State_Paused";
            color = L"TextFillColorPrimaryBrush";
        }
        StateIcon().Glyph(glyph);
        stateColor_ = color;
        StateIcon().Foreground(themeBrush(stateColor_));
        if (text.empty()) text = tr(key);
        setAnnounced(StateText(), text);
        setBadgeTip(StateBadge(), text,
                    s.error      ? ui::utf8(s.errorDetail)
                    : buffering_ ? tr(L"State_BufferingDetail")
                                 : hstring{});
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
            setAnnounced(LampText(), tr(L"Lamp_BitPerfect"));
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
        // While buffering, the bar has room for how long playback waits rather than how DSD will play.
        BypassBadge().Visibility((plan.dsd == DsdPath::Dop || plan.dsd == DsdPath::Native) && !buffering_
                                     ? Visibility::Visible
                                     : Visibility::Collapsed);
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
        if (plan.dsd == DsdPath::Pcm) add(tr(L"SignalPath_DsdToPcm"), L"352.8 kHz", PathState::Processed);
        if (plan.remodulate) {
            const uint32_t outBytes = plan.dsd == DsdPath::Dop ? s.output->sampleRate * 2 : s.output->sampleRate;
            swprintf_s(text, L"DSD%u → DSD%u", s.source->sampleRate * 8 / 44100, outBytes * 8 / 44100);
            add(tr(L"SignalPath_DsdRemodulate"), text, PathState::Processed);
        }
        if (plan.resample) {
            // The decimator always produces 352.8 kHz; soxr then takes it to the output rate.
            const double from = plan.dsd == DsdPath::Pcm ? 352.8 : s.source->sampleRate / 1000.0;
            swprintf_s(text, L"%.1f→%.1f kHz", from, s.output->sampleRate / 1000.0);
            add(tr(L"SignalPath_Resample"), text, PathState::Processed);
        }
        // The track's ReplayGain, the chain's nodes in order, then the volume; raw DSD bypasses them all.
        const bool raw = plan.dsd == DsdPath::Dop || plan.dsd == DsdPath::Native;
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
        const PathState rawState = plan.remodulate ? PathState::Processed : PathState::BitPerfect;
        if (plan.dsd == DsdPath::Dop)
            add(L"DoP", tr(L"SignalPath_DspBypassed"), rawState);
        else if (plan.dsd == DsdPath::Native)
            add(tr(L"SignalPath_NativeDsd"), tr(L"SignalPath_DspBypassed"), rawState);
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
        } else {
            add(tr(s.backend == Backend::Network ? L"SignalPath_Renderer" : L"SignalPath_Device"),
                ui::utf8(s.outputMode) + L" · " + ui::utf8(describe(*s.output)), reached);
        }
        if (s.backend == Backend::Network && !settings_.network.gapless)
            add(tr(L"SignalPath_GaplessOff"), {}, PathState::AtTrackChanges);
        if (s.playerVolume)
            add(tr(L"SignalPath_PlayerVolume"), s.playerMuted ? tr(L"SignalPath_Muted") : to_hstring(*s.playerVolume), reached);
        showSignalPath(std::move(steps));
        dspActive = processed;
    } else {
        lampColor_ = L"ControlStrongFillColorDisabledBrush";
        setAnnounced(LampText(), tr(L"Lamp_Idle"));
        SourceBadge().Visibility(Visibility::Collapsed);
        BypassBadge().Visibility(Visibility::Collapsed);
        PathRouteText().Text(ui::utf8(backendName(s.backend)) + L" · " + decoder);
        showSignalPath({});
    }
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
    std::wstring status =
        std::wstring(StateText().Text()) + L"|" + std::wstring(LampText().Text()) + L"|" + std::wstring(BypassText().Text());
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
        } else if (buffering_) {
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

    if (snapshot.command.id != shownCommandId_) {
        shownCommandId_ = snapshot.command.id;
        if (snapshot.commandError)
            showError(snapshot.commandError->code, snapshot.commandError->detail, snapshot.settings.backend);
    }

    // Status area on the settings page (docs/Plan.md §6)
    StatusGrid().Visibility(s.output ? Visibility::Visible : Visibility::Collapsed);
    StatusIdleText().Visibility(s.output ? Visibility::Collapsed : Visibility::Visible);
    StatusIdleText().Text(tr(L"Status_Idle"));
    if (s.output) {
        StatusOutputText().Text(ui::utf8(describe(*s.output)));
        StatusModeText().Text(ui::utf8(s.outputMode));
        std::wstring buffer(tr(L"Status_BufferValue"));
        wchar_t ms[32];
        swprintf(ms, 32, L"%.1f", s.output->sampleRate ? 1000.0 * s.bufferFrames / s.output->sampleRate : 0.0);
        for (const auto& [marker, value] : {std::pair<const wchar_t*, std::wstring>{L"{0}", std::to_wstring(s.bufferFrames)},
                                            std::pair<const wchar_t*, std::wstring>{L"{1}", ms}})
            if (const auto at = buffer.find(marker); at != std::wstring::npos) buffer.replace(at, 3, value);
        StatusBufferText().Text(buffer);
        StatusUnderrunText().Text(to_hstring(s.underruns));
        StatusDetailText().Text(clauseLines(s.outputDetail));
    }
    showStatusNotices(s);
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

// A network player's reason is said whole in its title; the engine's English detail stays in the log.
void MainWindow::showError(ErrorCode code, const std::string& detail, Backend backend) {
    const hstring player = isNetworkPlayer(backend) ? ui::playerErrorTitle(code) : hstring{};
    ErrorBar().Severity(InfoBarSeverity::Warning);
    ErrorBar().IsClosable(true);
    ErrorBar().Title(player.empty() ? ui::errorTitle(code) : player);
    ErrorBar().Message(player.empty() ? ui::utf8(detail) : hstring{});
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

namespace {

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
        combination.LostFocus([this, action](auto&&, auto&&) {
            if (capturing_ != action) return;
            capturing_.reset();
            applyHotkeys();
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

// Takes effect at once; the window content carries the theme, and the backdrop and caption buttons follow it.
void MainWindow::applyTheme() {
    const auto theme = settings_.theme == Theme::Light  ? ElementTheme::Light
                       : settings_.theme == Theme::Dark ? ElementTheme::Dark
                                                        : ElementTheme::Default;
    const auto content = Content().as<FrameworkElement>();
    if (content.RequestedTheme() != theme) content.RequestedTheme(theme);
}

// Colors set from code take the theme's brushes again once the window shows another theme.
void MainWindow::repaintTheme() {
    ui::setTheme(Content().as<FrameworkElement>().ActualTheme());
    if (waveformBar_) waveformBar_->repaint();
    StateIcon().Foreground(themeBrush(stateColor_));
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
    const bool dark = Content().as<FrameworkElement>().ActualTheme() == ElementTheme::Dark;
    const auto bar = AppWindow().TitleBar();
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
    std::wstring all(tr(L"Search_All"));
    if (const auto at = all.find(L"{0}"); at != std::wstring::npos) all.replace(at, 3, text.c_str());
    add({Suggestion::Kind::All, 0, {}, hstring(all)}, {}, {});
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
    const bool fromLibrary = Library().Visibility() == Visibility::Visible;
    std::function<void()> leave;
    if (!fromLibrary) {
        // Back returns to the page the search started from, such as Settings.
        leave = [this, tag = shownTag_] { returnToPage(tag); };
    }
    Nav().SelectedItem(nullptr);
    SettingsPanel().Visibility(Visibility::Collapsed);
    PlaceholderPanel().Visibility(Visibility::Collapsed);
    Home().Visibility(Visibility::Collapsed);
    Library().Visibility(Visibility::Visible);
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
        std::wstring text(tr(L"Reset_Sizes"));
        const std::array<std::wstring, 4> values{bytes(library), bytes(covers), bytes(settings), bytes(logs)};
        for (size_t i = 0; i < values.size(); ++i) {
            const std::wstring marker = L"{" + std::to_wstring(i) + L"}";
            if (const auto at = text.find(marker); at != std::wstring::npos) text.replace(at, marker.size(), values[i]);
        }
        self->DataSizesText().Text(hstring(text));
    });
}

void MainWindow::OnOpenDataFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    ShellExecuteW(nullptr, L"open", camel::dataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Settings › About (docs/Plan.md §17.5): the diagnostic bundle for GitHub issues, saved where the user picks.
winrt::fire_and_forget MainWindow::OnDiagnosticBundleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!server_) co_return;
    auto dialog = winrt::create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
    constexpr COMDLG_FILTERSPEC types[] = {{L"Zip", L"*.zip"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"zip");
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::array<wchar_t, 64> name{};
    swprintf(name.data(), name.size(), L"CamelPlayer-diagnostics-%04u%02u%02u-%02u%02u%02u.zip", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    dialog->SetFileName(name.data());
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()))) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        co_return;
    const std::filesystem::path file(path);
    CoTaskMemFree(path);
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

void MainWindow::OnGuideClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    showFirstRunGuide();
}

// Only a new installation sees the guide by itself: never finished or skipped, and no library folder yet.
winrt::fire_and_forget MainWindow::showFirstRunGuideIfDue() {
    if (settings_.firstRunDone || !server_) co_return;
    auto weak = get_weak();
    auto server = server_;
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    bool empty = true;
    try {
        empty = server->libraryRoots().empty();
    } catch (const std::exception&) {
    }
    if (empty)
        queue.TryEnqueue([weak] {
            if (auto self = weak.get()) self->showFirstRunGuide();
        });
}

winrt::fire_and_forget MainWindow::showFirstRunGuide() {
    if (guideFolders_ || !server_) co_return; // already open
    auto strong = get_strong();
    const auto text = [](const wchar_t* key) {
        TextBlock block;
        block.Text(tr(key));
        block.TextWrapping(TextWrapping::Wrap);
        return block;
    };

    StackPanel welcome;
    welcome.Children().Append(text(L"Guide_Welcome"));

    // Appearance: applied and saved at once, so the guide itself already shows the choice.
    ContentDialog dialog;
    StackPanel appearance;
    appearance.Spacing(12);
    appearance.Children().Append(text(L"Guide_Theme"));
    RadioButtons themes;
    for (const auto* key : {L"Theme_System", L"Theme_Light", L"Theme_Dark"}) themes.Items().Append(box_value(tr(key)));
    themes.SelectedIndex(int32_t(settings_.theme));
    themes.SelectionChanged([this, themes, dialog](auto&&, auto&&) {
        settings_.theme = Theme(std::clamp(themes.SelectedIndex(), 0, 2));
        applySettingsNow();
        dialog.RequestedTheme(Content().as<FrameworkElement>().ActualTheme());
    });
    appearance.Children().Append(themes);

    StackPanel folders;
    folders.Spacing(12);
    folders.Children().Append(text(L"Guide_Folders"));
    Button add;
    add.Content(box_value(tr(L"Settings_AddFolder/Content")));
    add.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
    add.Click([this](auto&&, auto&&) { OnAddFolderClick(nullptr, nullptr); });
    folders.Children().Append(add);
    guideFolders_ = ListView();
    guideFolders_.SelectionMode(ListViewSelectionMode::None);
    guideFolders_.MaxHeight(200);
    folders.Children().Append(guideFolders_);
    refreshRoots();

    // Output: a backend and its device; WASAPI follows the Windows default device unless one is chosen.
    StackPanel output;
    output.Spacing(12);
    output.Children().Append(text(L"Guide_Output"));
    ComboBox backend;
    backend.Header(box_value(tr(L"Settings_Backend/Header")));
    backend.Width(320);
    // In Backend's order, so an index is its Backend.
    for (const auto& name : {hstring(L"WASAPI"), hstring(L"ASIO"), hstring(L"Kernel Streaming"),
                             tr(L"Settings_NetworkGroup/Text"), hstring(L"AirPlay")})
        backend.Items().Append(box_value(name));
    ComboBox device;
    device.Header(box_value(tr(L"Settings_Device/Header")));
    device.Width(420);
    auto devices = std::make_shared<std::vector<std::string>>(); // ids in the order shown
    const auto fillDevices = [this, device, devices](Backend chosen) {
        const auto catalog = server_->snapshot().devices;
        const auto* list =
            std::array{&catalog.wasapi, &catalog.asio, &catalog.ks, &catalog.network, &catalog.airplay}[size_t(chosen)];
        const std::string saved = *std::array{&settings_.wasapi.deviceId, &settings_.asio.driverClsid, &settings_.ks.deviceId,
                                              &settings_.network.udn, &settings_.airplay.deviceId}[size_t(chosen)];
        device.Items().Clear();
        devices->clear();
        if (chosen == Backend::Wasapi) {
            device.Items().Append(box_value(tr(L"Device_Default")));
            devices->emplace_back();
        }
        for (const auto& entry : *list) {
            if (!entry.unsupported.empty()) continue;
            device.Items().Append(box_value(ui::utf8(entry.name)));
            devices->push_back(entry.id);
        }
        // A player or receiver not found now is still the output; nothing chosen keeps it.
        const bool network = chosen == Backend::Network || chosen == Backend::AirPlay;
        const auto selected = std::find(devices->begin(), devices->end(), saved);
        device.SelectedIndex(devices->empty()             ? -1
                             : selected != devices->end() ? int32_t(selected - devices->begin())
                             : network                    ? -1
                                                          : 0);
    };
    backend.SelectionChanged(
        [fillDevices, backend](auto&&, auto&&) { fillDevices(Backend(std::max(0, backend.SelectedIndex()))); });
    backend.SelectedIndex(int32_t(settings_.backend));
    output.Children().Append(backend);
    output.Children().Append(device);

    StackPanel done;
    done.Children().Append(text(L"Guide_Done"));

    const std::array<std::pair<const wchar_t*, UIElement>, 5> steps{{{L"Guide_WelcomeTitle", welcome},
                                                                     {L"Guide_ThemeTitle", appearance},
                                                                     {L"Guide_FoldersTitle", folders},
                                                                     {L"Guide_OutputTitle", output},
                                                                     {L"Guide_DoneTitle", done}}};
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.DefaultButton(ContentDialogButton::Primary);
    size_t step = 0;
    const auto show = [&] {
        dialog.Title(box_value(tr(steps[step].first)));
        dialog.Content(steps[step].second);
        const bool last = step + 1 == steps.size();
        dialog.PrimaryButtonText(tr(last ? L"Guide_Finish" : L"Guide_Next"));
        dialog.SecondaryButtonText(step > 0 && !last ? tr(L"Guide_Back") : hstring{});
        dialog.CloseButtonText(last ? hstring{} : tr(L"Guide_Skip"));
    };
    dialog.PrimaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& e) {
        if (step + 1 == steps.size()) return;
        e.Cancel(true);
        ++step;
        show();
    });
    dialog.SecondaryButtonClick([&](auto&&, ContentDialogButtonClickEventArgs const& e) {
        e.Cancel(true);
        if (step > 0) --step;
        show();
    });
    show();
    const auto result = co_await ui::showDialog(dialog);
    guideFolders_ = nullptr;

    // Finishing applies the chosen output; skipping keeps the defaults. Either way the guide is done.
    if (result == ContentDialogResult::Primary) {
        const auto index = device.SelectedIndex();
        if (index >= 0 && size_t(index) < devices->size())
            changeOutputs([&](camel::Settings& s, const DeviceCatalog& catalog) {
                camel::server::selectOutput(s, Backend(std::max(0, backend.SelectedIndex())), (*devices)[size_t(index)], catalog);
            });
    }
    settings_.firstRunDone = true;
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    populateSettings();
}

void MainWindow::OnQueueToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    queueOpen_ = !queueOpen_;
    updateQueuePanel();
}

void MainWindow::removeSelectedQueueEntries() {
    if (QueueList().SelectedItems().Size() == 0) return;
    Command command;
    command.kind = CommandKind::QueueRemove;
    for (const auto& item : QueueList().SelectedItems()) command.entryIds.push_back(ui::QueueCollection::idOf(item));
    submitCommand(std::move(command));
}

void MainWindow::OnQueueClearClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::QueueClear;
    submitCommand(std::move(command));
}

winrt::fire_and_forget MainWindow::OnQueueSaveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!server_) co_return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t date[32] = {};
    swprintf_s(date, L"%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    const auto name = toUtf8(std::wstring(tr(L"Queue_DefaultName")) + L" " + date);
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::optional<PlaylistChange> change;
    try {
        change = server->saveQueueAsPlaylist(name);
    } catch (const std::exception& e) {
        camel::log::warn("save queue as playlist: %s", e.what());
    }
    dispatcher.TryEnqueue([weak, change] {
        auto self = weak.get();
        if (!self) return;
        if (change)
            self->showPlaylistChange(*change, true);
        else
            self->showPlaylistNotice(tr(L"Queue_SaveFailed"), {}, true);
    });
}

void MainWindow::OnQueueDoubleTapped(Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&) {
    if (!QueueList().SelectedItem()) return;
    Command command;
    command.kind = CommandKind::QueueSelect;
    command.entryId = ui::QueueCollection::idOf(QueueList().SelectedItem());
    camel::measure::markPlayRequested();
    submitCommand(std::move(command));
}

void MainWindow::OnQueueRemoveEntryClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::QueueRemove;
    command.entryIds.push_back(ui::QueueCollection::idOf(sender.as<FrameworkElement>().DataContext()));
    submitCommand(std::move(command));
}

void MainWindow::OnQueueKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    if (e.Key() != Windows::System::VirtualKey::Delete || QueueList().SelectedItems().Size() == 0) return;
    e.Handled(true);
    removeSelectedQueueEntries();
}

void MainWindow::showPlaybackFavorite(bool favorite) {
    if (playbackFavorite_ == favorite) return;
    playbackFavorite_ = favorite;
    PlaybackFavoriteIcon().Glyph(favorite ? L"\xE00B" : L"\xE006");
    if (favorite)
        PlaybackFavoriteIcon().Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
    else
        PlaybackFavoriteIcon().ClearValue(IconElement::ForegroundProperty());
}

void MainWindow::OnPlaybackFavoriteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!playbackFile_) return;
    showPlaybackFavorite(!playbackFavorite_);
    submitCommand(ui::favoriteCommand({{playbackFile_}}, playbackFavorite_));
}

void MainWindow::OnQueueContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    const auto element = sender.as<FrameworkElement>();
    queueMenuItem_ = element.DataContext();
    if (!queueMenuItem_) return;
    if (!queueMenu_) {
        queueMenu_ = MenuFlyout();
        extendPlaylistMenu(queueMenu_, [this]() -> ui::SourceFetch {
            // The selected entries in queue order, or the right-clicked one when it is not selected.
            std::vector<PlaylistSource> sources;
            const auto add = [&](Windows::Foundation::IInspectable const& item) {
                const auto path = item.as<CamelPlayer::QueueItem>().Path();
                sources.push_back({PlaylistSource::Kind::Path, 0, {}, toUtf8(std::wstring_view(path.c_str(), path.size()))});
            };
            uint32_t clicked = 0;
            bool selected = false;
            if (queueItems_->IndexOf(queueMenuItem_, clicked))
                for (const auto& range : QueueList().SelectedRanges())
                    selected = selected || (std::cmp_greater_equal(clicked, range.FirstIndex()) &&
                                            std::cmp_less_equal(clicked, range.LastIndex()));
            if (!selected) {
                add(queueMenuItem_);
            } else {
                for (const auto& range : QueueList().SelectedRanges())
                    for (auto i = range.FirstIndex(); i <= range.LastIndex(); ++i) add(queueItems_->GetAt(uint32_t(i)));
            }
            return [sources] { return sources; };
        });
        queueMenu_.Items().RemoveAt(0); // the separator before the playlist entries
        ui::appendFavoriteMenu(queueMenu_, [this](bool favorite) {
            std::vector<LibraryTrackKey> tracks;
            const auto add = [&](Windows::Foundation::IInspectable const& item) {
                const auto file = get_self<QueueItem>(item.as<CamelPlayer::QueueItem>())->row().file;
                if (file && std::ranges::find(tracks, LibraryTrackKey{file}) == tracks.end()) tracks.push_back({file});
            };
            uint32_t clicked = 0;
            bool selected = false;
            if (queueItems_->IndexOf(queueMenuItem_, clicked))
                for (const auto& range : QueueList().SelectedRanges())
                    selected = selected || (std::cmp_greater_equal(clicked, range.FirstIndex()) &&
                                            std::cmp_less_equal(clicked, range.LastIndex()));
            if (!selected) {
                add(queueMenuItem_);
            } else {
                for (const auto& range : QueueList().SelectedRanges())
                    for (auto i = range.FirstIndex(); i <= range.LastIndex(); ++i) add(queueItems_->GetAt(uint32_t(i)));
            }
            if (!tracks.empty()) submitCommand(ui::favoriteCommand(tracks, favorite));
        });
        ui::appendFileDetailsMenu(queueMenu_, [this] {
            const auto item = queueMenuItem_.as<CamelPlayer::QueueItem>();
            const auto path = item.Path();
            showFileDetails(toUtf8(std::wstring_view(path.c_str(), path.size())), item.Title());
        });
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    queueMenu_.ShowAt(element, options);
    e.Handled(true);
}

void MainWindow::showFileDetails(const std::string& pathUtf8, hstring const& title) {
    if (!server_ || pathUtf8.empty()) return;
    fileDetails_.show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), title,
                      Content().as<FrameworkElement>().ActualTheme(),
                      [server = server_, pathUtf8] { return server->fileDetails(pathUtf8); });
}

// Alt+Enter opens the details of the track row the focus is on, or is in.
bool MainWindow::openFocusedFileDetails() {
    if (!Content().XamlRoot()) return false;
    auto element = Input::FocusManager::GetFocusedElement(Content().XamlRoot()).try_as<DependencyObject>();
    for (; element; element = Media::VisualTreeHelper::GetParent(element)) {
        const auto container = element.try_as<Primitives::SelectorItem>();
        if (!container) continue;
        const auto item = container.Content() ? container.Content() : container.DataContext();
        if (const auto queued = item.try_as<CamelPlayer::QueueItem>()) {
            const auto path = queued.Path();
            showFileDetails(toUtf8(std::wstring_view(path.c_str(), path.size())), queued.Title());
            return true;
        }
        if (const auto browse = item.try_as<CamelPlayer::BrowseItem>(); browse && browse.IsTrack()) {
            const auto& row = get_self<BrowseItem>(browse)->row();
            showFileDetails(row.value, row.title);
            return true;
        }
        return false;
    }
    return false;
}

void MainWindow::OnPlayerTrackContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& e) {
    if (!server_ || server_->snapshot().playback.pathUtf8.empty()) return;
    if (!playerTrackMenu_) {
        playerTrackMenu_ = MenuFlyout();
        ui::appendFileDetailsMenu(playerTrackMenu_, [this] {
            if (server_) showFileDetails(server_->snapshot().playback.pathUtf8, TitleText().Text());
        });
        playerTrackMenu_.Items().RemoveAt(0);                                                       // the separator
        playerTrackMenu_.Items().GetAt(0).as<MenuFlyoutItem>().KeyboardAcceleratorTextOverride({}); // Alt+Enter is for rows
    }
    const auto element = sender.as<FrameworkElement>();
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    playerTrackMenu_.ShowAt(element, options);
    e.Handled(true);
}

void MainWindow::extendPlaylistMenu(MenuFlyout const& menu, const std::function<ui::SourceFetch()>& selection) {
    menu.Items().Append(MenuFlyoutSeparator());
    MenuFlyoutItem create;
    create.Text(tr(L"Library_MenuCreatePlaylist"));
    FontIcon createIcon;
    createIcon.Glyph(L"\xE8F4");
    create.Icon(createIcon);
    create.Click([this, selection](auto&&, auto&&) { runPlaylistAction(selection(), std::nullopt, {}); });
    menu.Items().Append(create);
    MenuFlyoutSubItem add;
    add.Text(tr(L"Library_MenuAddToPlaylist"));
    FontIcon addIcon;
    addIcon.Glyph(L"\xE90B");
    add.Icon(addIcon);
    menu.Items().Append(add);
    menu.Opening([this, weakAdd = make_weak(add), selection](auto&&, auto&&) {
        const auto add = weakAdd.get();
        if (!add) return;
        add.Items().Clear();
        for (const auto& playlist : playlists_) {
            MenuFlyoutItem entry;
            entry.Text(ui::utf8(playlist.name));
            entry.Click([this, selection, id = playlist.id](auto&&, auto&&) { runPlaylistAction(selection(), id, {}); });
            add.Items().Append(entry);
        }
        if (!playlists_.empty()) add.Items().Append(MenuFlyoutSeparator());
        MenuFlyoutItem create;
        create.Text(tr(L"Library_MenuNewPlaylist"));
        create.Click([this, selection](auto&&, auto&&) { askNewPlaylist(selection()); });
        add.Items().Append(create);
    });
}

void MainWindow::runPlaylistAction(ui::SourceFetch fetch, std::optional<int64_t> playlist, std::string name) {
    if (!server_ || !fetch) return;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    std::thread([weak, dispatcher, server = server_, fetch = std::move(fetch), playlist, name = std::move(name)] {
        std::vector<PlaylistSource> sources;
        std::optional<SelectionPreview> preview;
        bool changed = false;
        try {
            sources = fetch();
            preview = sources.empty() ? SelectionPreview{} : server->previewSelection(sources);
        } catch (const camel::server::LibraryRevisionChanged&) {
            changed = true;
        } catch (const std::exception& e) {
            camel::log::warn("playlist selection: %s", e.what());
        }
        dispatcher.TryEnqueue([weak, sources = std::move(sources), preview, playlist, name, changed] {
            auto self = weak.get();
            if (!self) return;
            if (changed)
                self->showSelectionChanged();
            else if (!preview)
                self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
            else if (preview->tracks == 0)
                self->showPlaylistNotice(tr(L"Playlist_NothingAdded"), tr(L"Playlist_NothingAddedText"), false);
            else
                self->applyPlaylistAction(sources, *preview, playlist, name);
        });
    }).detach();
}

winrt::fire_and_forget MainWindow::applyPlaylistAction(std::vector<PlaylistSource> sources, SelectionPreview preview,
                                                       std::optional<int64_t> playlist, std::string name) {
    auto strong = get_strong();
    if (preview.tracks > 1000) {
        ContentDialog dialog;
        ui::placeDialog(dialog, ContentLayout().XamlRoot());
        dialog.Title(box_value(tr(L"Playlist_ConfirmTitle")));
        const std::wstring count = ui::count(preview.tracks);
        const int64_t minutes = preview.durationMs / 60'000;
        const std::wstring length =
            minutes < int64_t{24} * 60
                ? ui::formatTime(double(preview.durationMs) / 1000.0)
                : std::wstring(ui::format(L"Home_DaysHours",
                                          {std::to_wstring(minutes / (int64_t{24} * 60)), std::to_wstring(minutes / 60 % 24)}));
        dialog.Content(box_value(ui::format(L"Playlist_ConfirmText", {count, length})));
        dialog.PrimaryButtonText(tr(playlist ? L"Playlist_ConfirmAdd" : L"Playlist_ConfirmCreate"));
        dialog.CloseButtonText(tr(L"Playlist_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    }
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::optional<PlaylistChange> change;
    try {
        change = playlist ? server->addToPlaylist(*playlist, std::nullopt, sources) : server->createPlaylist(name, sources);
    } catch (const std::exception& e) {
        camel::log::warn("playlist change: %s", e.what());
    }
    dispatcher.TryEnqueue([weak, change, created = !playlist] {
        auto self = weak.get();
        if (!self) return;
        if (change)
            self->showPlaylistChange(*change, created);
        else
            self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
    });
}

winrt::fire_and_forget MainWindow::askNewPlaylist(ui::SourceFetch fetch) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Playlist_NewTitle")));
    TextBox name;
    name.PlaceholderText(tr(L"Playlist_NamePlaceholder"));
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Playlist_ConfirmCreate"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    const auto text = name.Text();
    runPlaylistAction(std::move(fetch), std::nullopt, toUtf8(std::wstring_view(text.c_str(), text.size())));
}

void MainWindow::showPlaylistChange(const PlaylistChange& change, bool created) {
    std::wstring message(ui::format(L"Library_Tracks", {ui::count(change.added)}));
    if (const auto skipped = change.skippedUnavailable + change.skippedDuplicates + change.skippedExisting)
        message += L" · " + std::wstring(ui::format(L"Playlist_Skipped", {ui::count(skipped)}));
    const std::wstring name(ui::utf8(change.name));
    if (!created && change.added == 0)
        showPlaylistNotice(ui::format(L"Playlist_NothingAddedTo", {name}), hstring(message), false, std::nullopt,
                           change.playlist);
    else
        showPlaylistNotice(ui::format(created ? L"Playlist_Created" : L"Playlist_Added", {name}), hstring(message), false,
                           change.undo, change.playlist);
    get_self<PlaylistView>(Playlists())->reload(change.playlist);
    if (created)
        refreshPlaylists([this, id = change.playlist] { renameInNavigation(id); });
    else
        refreshPlaylists();
}

void MainWindow::showSelectionChanged() {
    showPlaylistNotice(tr(L"Library_SelectionChangedTitle"), tr(L"Library_SelectionChangedText"), false);
    PlaylistNotice().Severity(InfoBarSeverity::Warning);
}

void MainWindow::showPlaylistNotice(hstring const& title, hstring const& message, bool error, std::optional<PlaylistUndo> undo,
                                    int64_t playlist) {
    auto bar = PlaylistNotice();
    bar.IsOpen(false);
    bar.Title(title);
    bar.Message(message);
    bar.Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Success);
    StackPanel actions;
    actions.Orientation(Orientation::Horizontal);
    actions.Spacing(8);
    actions.Margin({0, 0, 0, 12});
    if (playlist) {
        HyperlinkButton open;
        open.Content(box_value(tr(L"Playlist_Open")));
        open.Click([this, playlist](auto&&, auto&&) {
            PlaylistNotice().IsOpen(false);
            selectPage(L"playlist:" + to_hstring(playlist));
        });
        actions.Children().Append(open);
    }
    if (undo && (undo->created || !undo->items.empty())) {
        Button button;
        button.Content(box_value(tr(L"Playlist_Undo")));
        button.Click([this](auto&&, auto&&) { undoPlaylistNotice(); });
        actions.Children().Append(button);
    }
    if (actions.Children().Size())
        bar.Content(actions);
    else
        bar.Content(nullptr);
    bar.IsOpen(true);
    noticeUndo_ = undo; // after IsOpen: reopening a closed notice raises Closed for the previous one
    noticeTimer_.Stop();
    noticeTimer_.Start();
}

void MainWindow::undoPlaylistNotice() {
    if (!noticeUndo_ || !server_) return;
    const auto undo = *noticeUndo_;
    noticeUndo_.reset();
    PlaylistNotice().IsOpen(false);
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    std::thread([weak, dispatcher, server = server_, undo] {
        bool undone = false;
        try {
            undone = server->undoPlaylistChange(undo);
        } catch (const std::exception& e) {
            camel::log::warn("playlist undo: %s", e.what());
        }
        dispatcher.TryEnqueue([weak, undo, undone] {
            auto self = weak.get();
            if (!self) return;
            self->showPlaylistNotice(tr(undone ? L"Playlist_Undone" : L"Playlist_UndoFailed"), {}, !undone);
            self->refreshPlaylists();
            auto view = get_self<PlaylistView>(self->Playlists());
            if (undone && undo.created && view->shown() == undo.playlist)
                self->selectPage(L"playlists");
            else
                view->reload(undo.playlist);
        });
    }).detach();
}

winrt::fire_and_forget MainWindow::refreshPlaylists(std::function<void()> then) {
    auto strong = get_strong();
    if (!server_) co_return;
    auto server = server_;
    auto weak = get_weak();
    auto dispatcher = DispatcherQueue();
    co_await resume_background();
    std::vector<PlaylistSummary> list;
    try {
        list = server->playlists();
    } catch (const std::exception& e) {
        camel::log::warn("playlists: %s", e.what());
        co_return;
    }
    dispatcher.TryEnqueue([weak, list = std::move(list), then] {
        auto self = weak.get();
        if (!self) return;
        self->playlists_ = list;
        self->showPlaylistsInNavigation();
        if (then) then();
    });
}

void MainWindow::showPlaylistsInNavigation() {
    const auto children = PlaylistsNav().MenuItems();
    const auto tagOf = [](const PlaylistSummary& p) { return L"playlist:" + to_hstring(p.id); };
    bool same = children.Size() == playlists_.size();
    for (uint32_t i = 0; same && i < children.Size(); ++i)
        same = unbox_value_or<hstring>(children.GetAt(i).as<NavigationViewItem>().Tag(), L"") == tagOf(playlists_[i]);
    if (same) {
        for (uint32_t i = 0; i < children.Size(); ++i)
            children.GetAt(i).as<NavigationViewItem>().Content(box_value(ui::utf8(playlists_[i].name)));
        return;
    }
    navUpdating_ = true;
    children.Clear();
    for (const auto& playlist : playlists_) {
        NavigationViewItem item;
        item.Content(box_value(ui::utf8(playlist.name)));
        item.Tag(box_value(tagOf(playlist)));
        FontIcon icon;
        icon.Glyph(L"\xE90B");
        item.Icon(icon);
        children.Append(item);
        if (shownTag_ == tagOf(playlist)) Nav().SelectedItem(item);
    }
    navUpdating_ = false;
}

void MainWindow::renameInNavigation(int64_t playlist) {
    if (!Nav().IsPaneOpen() || Nav().DisplayMode() != NavigationViewDisplayMode::Expanded) return;
    NavigationViewItem item{nullptr};
    for (const auto& child : PlaylistsNav().MenuItems())
        if (unbox_value_or<hstring>(child.as<NavigationViewItem>().Tag(), L"") == L"playlist:" + to_hstring(playlist))
            item = child.as<NavigationViewItem>();
    if (!item) return;
    PlaylistsNav().IsExpanded(true);
    // The box sits in a flyout beside the item: inside the item, Enter would also open the playlist.
    const auto name = unbox_value_or<hstring>(item.Content(), L"");
    TextBox box;
    box.Text(name);
    box.MinWidth(240);
    Flyout flyout;
    flyout.Content(box);
    flyout.Placement(Primitives::FlyoutPlacementMode::RightEdgeAlignedTop);
    auto cancelled = std::make_shared<bool>(false);
    box.KeyDown([cancelled, weakFlyout = make_weak(flyout)](auto&&, Input::KeyRoutedEventArgs const& e) {
        if (e.Key() != Windows::System::VirtualKey::Enter && e.Key() != Windows::System::VirtualKey::Escape) return;
        e.Handled(true);
        *cancelled = e.Key() == Windows::System::VirtualKey::Escape;
        if (const auto flyout = weakFlyout.get()) flyout.Hide();
    });
    flyout.Opened([weakBox = make_weak(box)](auto&&, auto&&) {
        if (const auto box = weakBox.get()) {
            box.Focus(FocusState::Programmatic);
            box.SelectAll();
        }
    });
    flyout.Closed([this, playlist, name, cancelled, weakBox = make_weak(box)](auto&&, auto&&) {
        const auto box = weakBox.get();
        const hstring text = box ? box.Text() : name;
        if (*cancelled || text.empty() || text == name || !server_) return;
        PlaylistEdit edit;
        edit.kind = PlaylistEdit::Kind::Rename;
        edit.playlist = playlist;
        edit.name = toUtf8(std::wstring_view(text.c_str(), text.size()));
        auto weak = get_weak();
        auto dispatcher = DispatcherQueue();
        std::thread([weak, dispatcher, server = server_, edit] {
            try {
                server->editPlaylist(edit);
            } catch (const std::exception& e) {
                camel::log::warn("playlist rename: %s", e.what());
            }
            dispatcher.TryEnqueue([weak, playlist = edit.playlist] {
                if (auto self = weak.get()) {
                    self->refreshPlaylists();
                    get_self<PlaylistView>(self->Playlists())->reload(playlist);
                }
            });
        }).detach();
    });
    // An item under a collapsed Playlists entry is created by expanding it; a flyout needs it loaded.
    const auto show = [flyout, weakItem = make_weak(item)] {
        const auto item = weakItem.get();
        if (!item || !item.IsLoaded()) return;
        try {
            flyout.ShowAt(item);
        } catch (const hresult_error& e) {
            camel::log::warn("playlist rename box: 0x%08x", uint32_t(e.code()));
        }
    };
    if (item.IsLoaded())
        show();
    else {
        auto token = std::make_shared<event_token>();
        *token = item.Loaded([show, token, weakItem = make_weak(item)](auto&&, auto&&) {
            if (const auto item = weakItem.get()) item.Loaded(*token);
            show();
        });
    }
}

void MainWindow::OnQueueDragItemsCompleted(ListViewBase const&, DragItemsCompletedEventArgs const&) {
    if (queueItems_->Size() < 2) return;
    Command command;
    command.kind = CommandKind::QueueReorder;
    command.entryIds = queueItems_->ids();
    submitCommand(std::move(command));
}

void MainWindow::OnCoverClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    openLyrics(!lyricsOpen_);
}

constexpr auto kLyricsFollowAgain = std::chrono::seconds(3);
constexpr double kLyricsDimmed = 0.45;

void MainWindow::openLyrics(bool open) {
    lyricsOpen_ = open;
    LyricsView().Visibility(open ? Visibility::Visible : Visibility::Collapsed);
    const hstring tip = tr(open ? L"Lyrics_Hide" : L"Lyrics_Show");
    ToolTipService::SetToolTip(CoverButton(), box_value(tip));
    Automation::AutomationProperties::SetName(CoverButton(), tip);
    showTitleBack();
    if (!open) return;
    lyricsCentered_ = false;
    lyricsScrolledAt_ = {};
    const auto path = server_ ? server_->snapshot().playback.pathUtf8 : std::string();
    if (path != lyricsPath_) loadLyrics(path);
    showLyrics();
}

// Asked when a track starts, open or not, so that opening finds them; the Server reads them off this thread.
winrt::fire_and_forget MainWindow::loadLyrics(std::string path) {
    auto strong = get_strong();
    lyricsPath_ = path;
    lyrics_ = {};
    if (lyricsOpen_) showLyrics();
    if (path.empty() || !server_) co_return;
    auto server = server_;
    winrt::apartment_context foreground;
    co_await winrt::resume_background();
    auto read = server->lyrics(path);
    co_await foreground;
    if (lyricsPath_ != path) co_return;
    lyrics_ = std::move(read);
    if (lyricsOpen_) showLyrics();
}

void MainWindow::showLyrics() {
    lyricsLit_.reset();
    lyricsCentered_ = false;
    auto lines = LyricsLines().Children();
    lines.Clear();
    const bool none = lyrics_.lines.empty();
    LyricsNone().Visibility(none ? Visibility::Visible : Visibility::Collapsed);
    LyricsScroller().Visibility(none ? Visibility::Collapsed : Visibility::Visible);
    if (none) {
        LyricsNoneTitle().Text(TitleText().Text());
        hstring artist;
        if (server_) {
            const auto snapshot = server_->snapshot();
            const auto current = std::ranges::find(snapshot.queue.entries, snapshot.queueEntryId, &QueueEntry::id);
            if (current != snapshot.queue.entries.end()) artist = ui::utf8(current->artist);
        }
        LyricsNoneArtist().Text(artist);
        return;
    }
    for (const auto& line : lyrics_.lines) {
        TextBlock text;
        text.Text(ui::utf8(line.text));
        text.FontSize(lyrics_.synced ? 30 : 24);
        text.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        text.Foreground(Media::SolidColorBrush(Windows::UI::Colors::White()));
        text.TextAlignment(TextAlignment::Center);
        text.HorizontalAlignment(HorizontalAlignment::Center);
        text.TextWrapping(TextWrapping::Wrap);
        text.Opacity(lyrics_.synced ? kLyricsDimmed : 0.9);
        if (line.ms) {
            text.Tapped([this, ms = *line.ms](auto&&, auto&&) {
                const auto s = server_ ? server_->snapshot().playback : PlaybackSnapshot{};
                if (!s.source) return;
                Command seek;
                seek.kind = CommandKind::Seek;
                seek.frame = secondsToFrames(ms / 1000.0, s.source->sampleRate);
                submitCommand(std::move(seek));
                lyricsScrolledAt_ = {};
                lyricsCentered_ = false;
            });
        }
        lines.Append(text);
    }
    LyricsScroller().ChangeView(nullptr, 0.0, nullptr, true);
    // Paused, no snapshot follows to light the line, so the position is read once the lines are laid out.
    DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()] {
        const auto self = weak.get();
        if (!self || !self->server_) return;
        const auto s = self->server_->snapshot().playback;
        if (s.source && s.source->sampleRate) self->followLyrics(framesToSeconds(s.position, s.source->sampleRate));
    });
}

// The lines of the stamp being sung lit and brought to the middle, unless the user scrolled in the last seconds.
void MainWindow::followLyrics(double seconds) {
    if (!lyricsOpen_ || !lyrics_.synced || lyrics_.lines.empty()) return;
    const auto at = camel::server::lyricLineAt(lyrics_, uint32_t(std::max(0.0, seconds) * 1000));
    auto lines = LyricsLines().Children();
    if (at != lyricsLit_) {
        lyricsLit_ = at;
        lyricsCentered_ = false;
        for (uint32_t i = 0; i < lines.Size(); ++i) {
            const bool lit = at && lyrics_.lines[i].ms == lyrics_.lines[*at].ms;
            lines.GetAt(i).as<TextBlock>().Opacity(lit ? 1.0 : kLyricsDimmed);
        }
    }
    if (!at || lyricsCentered_ || std::chrono::steady_clock::now() - lyricsScrolledAt_ < kLyricsFollowAgain) return;
    size_t last = *at;
    while (last + 1 < lyrics_.lines.size() && lyrics_.lines[last + 1].ms == lyrics_.lines[*at].ms) ++last;
    const auto first = lines.GetAt(uint32_t(*at)).as<FrameworkElement>();
    const auto end = lines.GetAt(uint32_t(last)).as<FrameworkElement>();
    const auto top = first.TransformToVisual(LyricsLines()).TransformPoint({0, 0}).Y;
    const auto bottom = end.TransformToVisual(LyricsLines()).TransformPoint({0, 0}).Y + float(end.ActualHeight());
    const double middle = (top + bottom) / 2.0 - LyricsScroller().ViewportHeight() / 2.0;
    LyricsScroller().ChangeView(nullptr, std::max(0.0, middle), nullptr, false);
    lyricsCentered_ = true;
}

void MainWindow::OnLyricsScrollerSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const& e) {
    // Room above the first line and below the last, so that either can stand in the middle.
    const double half = e.NewSize().Height / 2;
    LyricsLines().Padding(ThicknessHelper::FromLengths(32, half, 32, half));
    lyricsCentered_ = false;
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
        const auto server = self->server_;
        server->waitIdle();
        self->replaced_ = true;
        self->exiting_ = true;
        self->tray_.reset();
        self->hotkeys_.reset();
        self->media_.reset();
        server->setListener({});
        self->replaceWindow_(server);
    });
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

winrt::fire_and_forget MainWindow::OnAsioPanelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::ShowControlPanel;
    submitCommand(std::move(command));
    co_return;
}

void MainWindow::OnRefreshDevicesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    Command command;
    command.kind = CommandKind::RefreshDevices;
    submitCommand(std::move(command));
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

void MainWindow::OnEqToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.eq.enabled = EqSwitch().IsOn();
    eqEditor_->show(settings_.eq, eqRate());
    eqChanged(true);
}

uint32_t MainWindow::eqRate() const {
    if (server_)
        if (const auto output = server_->snapshot().playback.output) return output->sampleRate;
    return 48000;
}

void MainWindow::showEq() {
    if (!eqEditor_) return;
    const bool wasPopulating = std::exchange(populating_, true);
    EqSwitch().IsOn(settings_.eq.enabled);
    EqPresetCombo().Items().Clear();
    int selected = -1;
    for (size_t i = 0; i < settings_.eq.presets.size(); ++i) {
        EqPresetCombo().Items().Append(box_value(ui::utf8(settings_.eq.presets[i].name)));
        if (settings_.eq.presets[i].name == settings_.eq.preset) selected = int(i);
    }
    EqPresetCombo().SelectedIndex(selected);
    EqDeletePresetButton().IsEnabled(selected >= 0);
    eqEditor_->show(settings_.eq, eqRate());
    populating_ = wasPopulating;
    showDspSummary();
}

void MainWindow::eqChanged(bool final) {
    settings_.eq.bands = eqEditor_->settings().bands;
    showDspSummary();
    if (final) {
        eqTimer_.Stop();
        settingsChanged();
    } else if (!eqTimer_.IsRunning()) {
        eqTimer_.Start();
    }
}

void MainWindow::OnEqPresetChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = EqPresetCombo().SelectedIndex();
    if (populating_ || index < 0 || size_t(index) >= settings_.eq.presets.size()) return;
    settings_.eq.bands = settings_.eq.presets[size_t(index)].bands;
    settings_.eq.preset = settings_.eq.presets[size_t(index)].name;
    showEq();
    settingsChanged();
}

winrt::fire_and_forget MainWindow::OnEqSavePresetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    ContentDialog dialog;
    ui::placeDialog(dialog, ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Eq_SaveTitle")));
    TextBox name;
    name.Header(box_value(tr(L"Eq_PresetName")));
    name.Text(ui::utf8(settings_.eq.preset));
    name.SelectAll();
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Eq_Save"));
    dialog.CloseButtonText(tr(L"Reset_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) co_return;
    std::wstring text(name.Text());
    text.erase(0, text.find_first_not_of(L" \t"));
    text.erase(text.find_last_not_of(L" \t") + 1);
    if (text.empty()) co_return;
    const auto utf8 = camel::toUtf8(text);
    auto& presets = settings_.eq.presets;
    const auto found = std::ranges::find(presets, utf8, &camel::EqPreset::name);
    if (found != presets.end())
        found->bands = eqEditor_->settings().bands;
    else
        presets.push_back({utf8, eqEditor_->settings().bands});
    settings_.eq.preset = utf8;
    showEq();
    settingsChanged();
}

void MainWindow::OnEqDeletePresetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    const int index = EqPresetCombo().SelectedIndex();
    if (index < 0 || size_t(index) >= settings_.eq.presets.size()) return;
    settings_.eq.presets.erase(settings_.eq.presets.begin() + index);
    settings_.eq.preset.clear();
    showEq();
    settingsChanged();
}

// Back to the 5 flat bands a new EQ starts with; saved presets stay.
void MainWindow::OnEqResetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.eq.bands = camel::defaultEqBands();
    settings_.eq.preset.clear();
    showEq();
    settingsChanged();
}

// Equalizer APO and AutoEq ParametricEQ.txt: the filters become a preset named after the file, and the EQ turns on.
void MainWindow::OnEqImportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    constexpr COMDLG_FILTERSPEC types[] = {{L"Equalizer APO / AutoEq", L"*.txt"}};
    dialog->SetFileTypes(1, types);
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()))) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    const std::filesystem::path file(path);
    CoTaskMemFree(path);
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    camel::EqImportNotes notes;
    const auto bands = camel::parseEqualizerApo(text, &notes);
    if (bands.empty()) {
        ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Eq_ImportTitle"), tr(L"Eq_ImportFailed")));
        return;
    }
    const auto name = camel::toUtf8(file.stem().wstring());
    auto& presets = settings_.eq.presets;
    const auto found = std::ranges::find(presets, name, &camel::EqPreset::name);
    if (found != presets.end())
        found->bands = bands;
    else
        presets.push_back({name, bands});
    settings_.eq.bands = bands;
    settings_.eq.preset = name;
    settings_.eq.enabled = true;
    showEq();
    settingsChanged();
    const auto lines = [](const std::vector<int>& numbers) {
        std::wstring text;
        for (const int number : numbers) text += (text.empty() ? L"" : L", ") + std::to_wstring(number);
        return text;
    };
    std::wstring changes;
    if (!notes.held.empty()) changes += ui::format(L"Eq_ImportHeld", {lines(notes.held)}).c_str();
    if (!notes.dropped.empty())
        changes += (changes.empty() ? L"" : L"\n\n") + std::wstring(ui::format(L"Eq_ImportDropped", {lines(notes.dropped)}));
    if (!changes.empty()) ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Eq_ImportTitle"), hstring(changes)));
}

void MainWindow::OnEqExportClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto dialog = winrt::create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
    constexpr COMDLG_FILTERSPEC types[] = {{L"Equalizer APO", L"*.txt"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"txt");
    const auto name = settings_.eq.preset.empty() ? std::wstring(L"CamelPlayer EQ") : camel::toWide(settings_.eq.preset);
    dialog->SetFileName((name + L".txt").c_str());
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()))) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    const std::filesystem::path file(path);
    CoTaskMemFree(path);
    auto shown = settings_.eq;
    shown.enabled = true;
    const auto text = camel::formatEqualizerApo(settings_.eq.bands, analyzeDsp(eqChain(shown), eqRate()).preampDb);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
    if (!out) camel::log::warn("EQ export: cannot write %s", camel::toUtf8(file.wstring()).c_str());
}

// Software volume (docs/Plan.md §5.9): the mode on the DSP card, and the playback bar's volume button, which opens the
// slider and mute while it is Software and says who sets the level while it is Fixed.
void MainWindow::showVolume() {
    const bool wasPopulating = std::exchange(populating_, true);
    const bool network = camel::isNetworkPlayer(settings_.backend);
    const bool software = settings_.volume.mode == camel::VolumeMode::Software;
    const bool wantsPlayer = settings_.volume.mode == camel::VolumeMode::Player;
    const bool player = wantsPlayer && playerVolume_.has_value();
    VolumeModeOwnItem().Content(box_value(tr(network ? L"Volume_ModePlayer" : L"Volume_ModeSoftware")));
    VolumeModeCombo().SelectedIndex(software || wantsPlayer ? 1 : 0);
    VolumeNote().Text(tr(settings_.backend == Backend::AirPlay ? L"Volume_NoteAirPlay"
                         : network                             ? L"Volume_NotePlayer"
                                                               : L"Volume_NoteLocal"));
    FixedVolumePanel().Visibility(software || player ? Visibility::Collapsed : Visibility::Visible);
    SoftwareVolumePanel().Visibility(software ? Visibility::Visible : Visibility::Collapsed);
    PlayerVolumePanel().Visibility(player ? Visibility::Visible : Visibility::Collapsed);
    VolumeLock().Visibility(software || player ? Visibility::Collapsed : Visibility::Visible);
    FixedVolumeTitle().Text(tr(wantsPlayer ? L"Volume_PlayerUnknown" : L"Volume_Fixed"));
    VolumeSlider().Value(settings_.volume.db);
    if (player && std::chrono::steady_clock::now() - playerVolumeMovedAt_ > kPlayerVolumeSettles)
        PlayerVolumeSlider().Value(*playerVolume_);
    PlayerMuteIcon().Glyph(playerMuted_ ? L"\xE74F" : L"\xE767");
    const auto playerMute = tr(playerMuted_ ? L"Volume_Unmute" : L"Volume_Mute");
    ToolTipService::SetToolTip(PlayerMuteButton(), box_value(playerMute));
    Automation::AutomationProperties::SetName(PlayerMuteButton(), playerMute);
    MuteIcon().Glyph(settings_.volume.muted ? L"\xE74F" : L"\xE767");
    const auto mute = tr(settings_.volume.muted ? L"Volume_Unmute" : L"Volume_Mute");
    ToolTipService::SetToolTip(MuteButton(), box_value(mute));
    Automation::AutomationProperties::SetName(MuteButton(), mute);
    populating_ = wasPopulating;
    showVolumeLevel();
    showDspSummary();
}

// The volume button's icon, name and the level above the slider follow the level as it moves.
void MainWindow::showVolumeLevel() {
    const auto& volume = settings_.volume;
    wchar_t level[32];
    swprintf_s(level, L"%.1f dB", volume.db);
    VolumeText().Text(level);
    hstring name;
    if (volume.mode == camel::VolumeMode::Player && playerVolume_) {
        const auto shown = uint32_t(PlayerVolumeSlider().Value());
        const auto text = to_hstring(shown);
        PlayerVolumeText().Text(text);
        VolumeIcon().Glyph(playerMuted_ ? L"\xE74F" : shown > 66 ? L"\xE995" : shown > 33 ? L"\xE994" : L"\xE993");
        name = ui::format(L"Volume_Level", {playerMuted_ ? std::wstring(tr(L"SignalPath_Muted")) : std::wstring(text)});
    } else if (volume.mode != camel::VolumeMode::Software) {
        VolumeIcon().Glyph(L"\xE767");
        name = tr(L"Volume_Fixed");
    } else {
        VolumeIcon().Glyph(volume.muted ? L"\xE74F" : volume.db > -10 ? L"\xE995" : volume.db > -30 ? L"\xE994" : L"\xE993");
        name = ui::format(L"Volume_Level", {volume.muted ? std::wstring(tr(L"SignalPath_Muted")) : std::wstring(level)});
    }
    setBadgeTip(VolumeButton(), name, name);
}

void MainWindow::OnVolumeSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    VolumeFlyout().Hide();
    showSettingsCard(VolumeCard());
}

void MainWindow::OnStatusAreaSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&) {
    fitStatus();
}

// Where the badges do not fit the state badge gives way, then the lamp's words; the dot stays and opens the signal path.
// While buffering, the state badge says how long playback waits, so the lamp's words give way first.
void MainWindow::fitStatus() {
    const auto fits = [this] {
        StatusBadges().Measure({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
        return StatusBadges().DesiredSize().Width <= StatusArea().ActualWidth();
    };
    StateBadge().Visibility(Visibility::Visible);
    LampText().Visibility(Visibility::Visible);
    if (StatusArea().ActualWidth() == 0 || fits()) return;
    UIElement first = StateBadge(), second = LampText();
    if (buffering_) std::swap(first, second);
    first.Visibility(Visibility::Collapsed);
    if (!fits()) second.Visibility(Visibility::Collapsed);
}

void MainWindow::showReplayGain() {
    const bool wasPopulating = std::exchange(populating_, true);
    const auto& rg = settings_.replayGain;
    ReplayGainModeCombo().SelectedIndex(int(rg.mode));
    ReplayGainPreampBox().Value(rg.preampDb);
    ReplayGainNoValueBox().Value(rg.noValuePreampDb);
    ReplayGainClipSwitch().IsOn(rg.preventClipping);
    populating_ = wasPopulating;
    enableDependentFields();
    showLoudnessProgress();
    showDspSummary();
}

// A feature's own fields are disabled while it is off, so what depends on what shows.
void MainWindow::enableDependentFields() {
    const bool replayGain = ReplayGainModeCombo().SelectedIndex() > 0;
    for (const Control control :
         {Control(ReplayGainPreampBox()), Control(ReplayGainNoValueBox()), Control(ReplayGainClipSwitch())})
        control.IsEnabled(replayGain);
    CrossfadeSecondsBox().IsEnabled(CrossfadeSwitch().IsOn());
}

void MainWindow::showLoudnessProgress() {
    if (!server_ || SettingsPanel().Visibility() != Visibility::Visible) return;
    const auto progress = server_->loudnessProgress();
    std::wstring text;
    if (progress.total > 0) {
        text = ui::format(L"Loudness_Progress", {ui::count(progress.analyzed), ui::count(progress.total)});
        // A sentence after a full-width full stop needs no space.
        const std::wstring separator = text.ends_with(L'。') ? L"" : L" ";
        if (settings_.replayGain.mode == camel::ReplayGainMode::Off && progress.analyzed < progress.total)
            text += separator + std::wstring(tr(L"Loudness_Off"));
        else if (progress.running)
            text += separator + std::wstring(tr(L"Loudness_Analyzing"));
    }
    if (LoudnessProgressText().Text() != text) LoudnessProgressText().Text(text);
}

camel::VolumeMode MainWindow::chosenVolumeMode() {
    if (VolumeModeCombo().SelectedIndex() != 1) return camel::VolumeMode::Fixed;
    return camel::isNetworkPlayer(settings_.backend) ? camel::VolumeMode::Player : camel::VolumeMode::Software;
}

void MainWindow::OnVolumeModeChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    settings_.volume.mode = chosenVolumeMode();
    showVolume();
    settingsChanged();
}

void MainWindow::OnVolumeChanged(Windows::Foundation::IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (populating_) return;
    settings_.volume.db = std::round(e.NewValue() * 2) / 2;
    showVolumeLevel();
    showDspSummary();
    // A slider being dragged applies at most every 100 ms.
    if (!eqTimer_.IsRunning()) eqTimer_.Start();
}

void MainWindow::OnVolumeWheel(Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
    const bool player = settings_.volume.mode == camel::VolumeMode::Player && playerVolume_;
    if (settings_.volume.mode != camel::VolumeMode::Software && !player) return;
    // A notch moves 1 dB; one event may carry several notches, and a touchpad sends parts of one.
    volumeWheel_ += e.GetCurrentPoint(nullptr).Properties().MouseWheelDelta();
    const int notches = volumeWheel_ / WHEEL_DELTA;
    volumeWheel_ %= WHEEL_DELTA;
    if (notches != 0 && player)
        PlayerVolumeSlider().Value(std::clamp(PlayerVolumeSlider().Value() + notches, 0.0, 100.0));
    else if (notches != 0)
        VolumeSlider().Value(std::clamp(VolumeSlider().Value() + notches, camel::VolumeSettings::kMinDb, 0.0));
    e.Handled(true);
}

// Only a move of the user's reaches the player; what it reports by itself is shown with `populating_` set.
void MainWindow::OnPlayerVolumeChanged(Windows::Foundation::IInspectable const&,
                                       Primitives::RangeBaseValueChangedEventArgs const&) {
    if (populating_ || !server_) return;
    showVolumeLevel();
    playerVolumeMovedAt_ = std::chrono::steady_clock::now();
    Command command;
    command.kind = CommandKind::SetPlayerVolume;
    command.playerVolume = uint32_t(PlayerVolumeSlider().Value());
    submitCommand(std::move(command));
}

// Under the group's slider, one for each receiver of an AirPlay group; a slider the user moves stays where it was left
// until the level settles.
void MainWindow::showReceiverVolumes() {
    const bool group = receivers_.size() > 1;
    ReceiverVolumes().Visibility(group ? Visibility::Visible : Visibility::Collapsed);
    auto rows = ReceiverVolumes().Children();
    const bool same = group && rows.Size() == receivers_.size() &&
                      std::ranges::all_of(std::views::iota(size_t{0}, receivers_.size()), [&](size_t i) {
                          return unbox_value_or<hstring>(rows.GetAt(uint32_t(i)).as<FrameworkElement>().Tag(), L"") ==
                                 ui::utf8(receivers_[i].id);
                      });
    if (!same) rows.Clear();
    if (!group) return;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < receivers_.size(); ++i) {
        const auto& receiver = receivers_[i];
        if (same) {
            const auto moved = receiverMovedAt_.find(receiver.id);
            if (moved != receiverMovedAt_.end() && now - moved->second < kPlayerVolumeSettles) continue;
            const auto slider = rows.GetAt(uint32_t(i)).as<StackPanel>().Children().GetAt(1).as<Slider>();
            const bool wasPopulating = std::exchange(populating_, true);
            slider.Value(receiver.level);
            populating_ = wasPopulating;
            continue;
        }
        StackPanel row;
        row.Tag(box_value(ui::utf8(receiver.id)));
        TextBlock name;
        name.Text(ui::utf8(receiver.name));
        name.Style(unbox_value<Microsoft::UI::Xaml::Style>(
            Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle"))));
        Slider slider;
        slider.Width(160);
        slider.Minimum(0);
        slider.Maximum(100);
        slider.StepFrequency(1);
        slider.Value(receiver.level);
        Automation::AutomationProperties::SetName(slider, ui::format(L"Volume_ReceiverSlider", {std::wstring(name.Text())}));
        slider.ValueChanged([this, id = receiver.id](auto&&, Primitives::RangeBaseValueChangedEventArgs const& e) {
            if (populating_ || !server_) return;
            receiverMovedAt_[id] = std::chrono::steady_clock::now();
            Command command;
            command.kind = CommandKind::SetReceiverVolume;
            command.receiver = id;
            command.playerVolume = uint32_t(e.NewValue());
            submitCommand(std::move(command));
        });
        row.Children().Append(name);
        row.Children().Append(slider);
        rows.Append(row);
    }
}

void MainWindow::OnPlayerMuteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    Command command;
    command.kind = CommandKind::SetPlayerMute;
    command.playerMute = !playerMuted_;
    submitCommand(std::move(command));
}

void MainWindow::OnMuteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.volume.muted = !settings_.volume.muted;
    showVolume();
    settingsChanged();
}

void MainWindow::OnReplayGainChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    enableDependentFields();
    settingsChanged();
    showLoudnessProgress();
    showDspSummary();
}

void MainWindow::OnReplayGainValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
}

void MainWindow::OnReplayGainToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
}

void MainWindow::showCrossfade() {
    const bool wasPopulating = std::exchange(populating_, true);
    CrossfadeSwitch().IsOn(settings_.crossfade.enabled);
    CrossfadeSecondsBox().Value(settings_.crossfade.seconds);
    populating_ = wasPopulating;
    enableDependentFields();
}

// Rebuilds the signal path rows only when a step changed; snapshots arrive many times a second.
void MainWindow::showSignalPath(std::vector<PathStep> steps) {
    if (steps == shownPath_) return;
    shownPath_ = std::move(steps);
    auto grid = SignalPathGrid();
    grid.Children().Clear();
    grid.RowDefinitions().Clear();
    // With nothing playing, the verdict and the route stand alone.
    const auto shown = shownPath_.empty() ? Visibility::Collapsed : Visibility::Visible;
    PathDivider().Visibility(shown);
    grid.Visibility(shown);
    if (shownPath_.empty()) return;
    const auto resources = Application::Current().Resources();
    const auto body = resources.Lookup(box_value(L"BodyTextBlockStyle")).as<Style>();
    const auto caption = resources.Lookup(box_value(L"CaptionTextBlockStyle")).as<Style>();
    const auto secondary = themeBrush(L"TextFillColorSecondaryBrush");
    const auto rail = themeBrush(L"ControlStrongStrokeColorDefaultBrush");
    // A body line is 20 px high; the 10 px dot sits in the middle of the first one.
    constexpr double dotTop = 5;
    constexpr double dotSize = 10;
    int32_t row = 0;
    const auto place = [&](FrameworkElement const& element, int32_t column, int32_t span) {
        Grid::SetRow(element, row);
        Grid::SetColumn(element, column);
        Grid::SetColumnSpan(element, span);
        grid.Children().Append(element);
    };
    const auto addRow = [&] {
        RowDefinition definition;
        definition.Height(GridLength{1, GridUnitType::Auto});
        grid.RowDefinitions().Append(definition);
    };
    for (const auto [stage, title] : {std::pair{PathStage::Source, L"SignalPath_StageSource"},
                                      {PathStage::Processing, L"SignalPath_StageProcessing"},
                                      {PathStage::Output, L"SignalPath_StageOutput"}}) {
        addRow();
        TextBlock heading;
        heading.Style(caption);
        heading.Text(tr(title));
        heading.Foreground(secondary);
        heading.Margin({0, row == 0 ? 0.0 : 12.0, 0, 6});
        Automation::AutomationProperties::SetHeadingLevel(heading, Automation::Peers::AutomationHeadingLevel::Level2);
        place(heading, 0, 3);
        ++row;
        std::vector<const PathStep*> members;
        for (const auto& step : shownPath_)
            if (step.stage == stage) members.push_back(&step);
        // A stage without steps says so, beside a dashed rail.
        if (members.empty()) {
            addRow();
            Shapes::Line dashes;
            dashes.X1(dotSize / 2);
            dashes.X2(dotSize / 2);
            dashes.Y2(20);
            dashes.Stroke(rail);
            dashes.StrokeThickness(2);
            Media::DoubleCollection pattern;
            pattern.Append(2);
            pattern.Append(2);
            dashes.StrokeDashArray(pattern);
            TextBlock text;
            text.Style(body);
            text.Text(tr(L"SignalPath_Unchanged"));
            text.Foreground(secondary);
            text.TextWrapping(TextWrapping::Wrap);
            place(dashes, 0, 1);
            place(text, 1, 2);
            ++row;
            continue;
        }
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& step = *members[i];
            const bool last = i + 1 == members.size();
            addRow();
            const wchar_t* brush = step.state == PathState::BitPerfect ? L"SystemFillColorSuccessBrush"
                                   : step.state == PathState::Lossless ? L"SystemFillColorCautionBrush"
                                   : step.state == PathState::Lossy    ? L"SystemFillColorNeutralBrush"
                                                                       : L"SystemFillColorCriticalBrush";
            const wchar_t* state = step.state == PathState::BitPerfect  ? L"Lamp_BitPerfect"
                                   : step.state == PathState::Lossless  ? L"Lamp_Lossless"
                                   : step.state == PathState::Lossy     ? L"Lamp_Lossy"
                                   : step.state == PathState::Processed ? L"Lamp_Processed"
                                                                        : L"SignalPath_StateAtTrackChanges";
            // The rail joins the dots of a stage and stops short of each dot, so a ring stays hollow.
            Grid marker;
            marker.Width(dotSize);
            if (i > 0) {
                Shapes::Rectangle above;
                above.Width(2);
                above.Height(dotTop);
                above.VerticalAlignment(VerticalAlignment::Top);
                above.Fill(rail);
                marker.Children().Append(above);
            }
            if (!last) {
                Shapes::Rectangle below;
                below.Width(2);
                below.Margin({0, dotTop + dotSize, 0, 0});
                below.Fill(rail);
                marker.Children().Append(below);
            }
            // A ring marks a step that acts only when tracks change.
            Shapes::Ellipse dot;
            dot.Width(dotSize);
            dot.Height(dotSize);
            dot.Margin({0, dotTop, 0, 0});
            dot.VerticalAlignment(VerticalAlignment::Top);
            if (step.state == PathState::AtTrackChanges) {
                dot.Stroke(themeBrush(brush));
                dot.StrokeThickness(2);
            } else {
                dot.Fill(themeBrush(brush));
            }
            ToolTipService::SetToolTip(dot, box_value(tr(state)));
            marker.Children().Append(dot);
            const Thickness gap{0, 0, 0, last ? 0.0 : 10.0};
            TextBlock name;
            name.Style(body);
            name.Text(step.name);
            name.Margin(gap);
            Automation::AutomationProperties::SetName(name, step.name + L", " + tr(state));
            TextBlock detail;
            detail.Style(body);
            detail.Text(step.detail);
            detail.Margin(gap);
            detail.TextWrapping(TextWrapping::Wrap);
            detail.IsTextSelectionEnabled(true);
            detail.Foreground(secondary);
            place(marker, 0, 1);
            place(name, 1, 1);
            place(detail, 2, 1);
            ++row;
        }
    }
}

void MainWindow::OnCrossfadeToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    enableDependentFields();
    settingsChanged();
}

void MainWindow::OnCrossfadeSecondsChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    if (populating_) return;
    settingsChanged();
    // A typed length shows as the half second it is kept at.
    showCrossfade();
}

// The DSP card (docs/Plan.md §5.9): every node from settings_, the order rows and the chain's headroom.
void MainWindow::showDsp() {
    const bool wasPopulating = std::exchange(populating_, true);
    CrossfeedSwitch().IsOn(settings_.crossfeed.enabled);
    CrossfeedPresetCombo().SelectedIndex(int(settings_.crossfeed.preset));
    const auto [cutoff, feed] = camel::crossfeedValues(settings_.crossfeed);
    CrossfeedCutoffBox().Value(cutoff);
    CrossfeedFeedBox().Value(feed);
    const bool custom = settings_.crossfeed.preset == camel::CrossfeedPreset::Custom;
    CrossfeedCutoffBox().IsEnabled(custom);
    CrossfeedFeedBox().IsEnabled(custom);
    const bool hasFile = !settings_.convolution.file.empty();
    ConvolutionSwitch().IsOn(settings_.convolution.enabled && hasFile);
    ConvolutionSwitch().IsEnabled(hasFile);
    ConvolutionRemoveButton().IsEnabled(hasFile);
    ConvolutionFileText().Text(hasFile ? ui::utf8(settings_.convolution.file) : tr(L"Convolution_NoFile"));
    populating_ = wasPopulating;
    showDspOrder();
    showEq();
    if (settings_.convolution.file != impulseFile_ || eqRate() != impulseRate_) loadConvolution();
}

void MainWindow::showDspOrder() {
    auto rows = DspOrderList().Children();
    rows.Clear();
    const auto& order = settings_.dspOrder;
    for (size_t i = 0; i < order.size(); ++i) {
        Grid row;
        row.ColumnSpacing(8);
        for (const auto width : {GridLengthHelper::FromPixels(24), GridLengthHelper::FromPixels(200),
                                 GridLength{1, GridUnitType::Auto}, GridLength{1, GridUnitType::Auto}}) {
            ColumnDefinition column;
            column.Width(width);
            row.ColumnDefinitions().Append(column);
        }
        TextBlock number;
        number.Text(to_hstring(i + 1) + L".");
        number.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(number);
        TextBlock name;
        name.Text(tr(dspNodeKey(order[i])));
        name.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(name, 1);
        row.Children().Append(name);
        const auto move = [&](const wchar_t* glyph, const wchar_t* key, size_t to, int column) {
            Button button;
            button.Padding(ThicknessHelper::FromUniformLength(6));
            FontIcon icon;
            icon.Glyph(glyph);
            icon.FontSize(12);
            button.Content(icon);
            button.IsEnabled(to < order.size());
            ToolTipService::SetToolTip(button, box_value(tr(key)));
            Automation::AutomationProperties::SetName(button, tr(key) + L" " + name.Text());
            button.Click([this, from = i, to](auto&&, auto&&) {
                std::swap(settings_.dspOrder[from], settings_.dspOrder[to]);
                // The row's own click is still being handled; rebuild once it returns.
                DispatcherQueue().TryEnqueue([this] { showDspOrder(); });
                showDspSummary();
                settingsChanged();
            });
            Grid::SetColumn(button, column);
            row.Children().Append(button);
        };
        move(L"\xE70E", L"Dsp_MoveUp", i - 1, 2); // wraps past the end for the first row, which disables it
        move(L"\xE70D", L"Dsp_MoveDown", i + 1, 3);
        rows.Append(row);
    }
    DspRestoreOrderButton().IsEnabled(order != camel::audio::kDefaultDspOrder);
}

void MainWindow::showDspSummary() {
    const uint32_t rate = eqRate();
    const auto chain = camel::audio::dspChain(settings_.eq, settings_.crossfeed, settings_.convolution, settings_.dspOrder,
                                              impulseRate_ == rate ? impulse_ : nullptr);
    const bool anyOn = settings_.eq.enabled || settings_.crossfeed.enabled ||
                       (settings_.convolution.enabled && !settings_.convolution.file.empty());
    const auto analysis = camel::audio::analyzeDsp(chain, rate);
    // The software volume and ReplayGain change the level whatever the chain does.
    const bool levelled = camel::volumeDb(settings_.volume) != 0.0 || settings_.replayGain.mode != camel::ReplayGainMode::Off;
    if (!anyOn) {
        DspSummaryText().Text(tr(levelled ? L"Dsp_OffLevelled" : L"Dsp_Off"));
    } else if (!analysis.active) {
        DspSummaryText().Text(tr(levelled ? L"Dsp_FlatLevelled" : L"Dsp_Flat"));
    } else {
        wchar_t db[16];
        swprintf_s(db, L"%.1f", -analysis.preampDb);
        DspSummaryText().Text(ui::format(L"Dsp_Headroom", {db}));
    }
}

// Reads the impulse response for the playing rate off the UI thread, then shows what it holds.
winrt::fire_and_forget MainWindow::loadConvolution() {
    const auto file = settings_.convolution.file;
    const uint32_t rate = eqRate();
    const uint64_t load = ++impulseLoads_;
    impulse_ = nullptr;
    impulseFile_ = file;
    impulseRate_ = rate;
    ConvolutionInfoText().Text(file.empty() ? hstring{} : tr(L"Convolution_Loading"));
    showDspSummary();
    if (file.empty()) co_return;
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await winrt::resume_background();
    camel::audio::LoadedImpulse loaded;
    std::optional<camel::audio::ImpulseProblem> problem;
    try {
        loaded = camel::audio::loadImpulse(camel::toWide(file), rate);
    } catch (const camel::audio::ImpulseError& e) {
        problem = e.problem();
        camel::log::warn("impulse response: %s", e.what());
    }
    queue.TryEnqueue([weak, load, rate, loaded, problem] {
        auto self = weak.get();
        if (!self || load != self->impulseLoads_) return;
        self->impulse_ = loaded.impulse;
        self->ConvolutionInfoText().Text(problem ? impulseProblemText(*problem) : impulseInfoText(loaded.info, rate));
        self->showDspSummary();
    });
}

void MainWindow::OnDspRestoreOrderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.dspOrder = camel::audio::kDefaultDspOrder;
    showDspOrder();
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnCrossfeedToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.crossfeed.enabled = CrossfeedSwitch().IsOn();
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnCrossfeedPresetChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const int index = CrossfeedPresetCombo().SelectedIndex();
    if (populating_ || index < 0) return;
    const auto preset = camel::CrossfeedPreset(index);
    if (preset == camel::CrossfeedPreset::Custom && settings_.crossfeed.preset != preset) {
        // Custom starts from the preset it replaces.
        std::tie(settings_.crossfeed.cutoffHz, settings_.crossfeed.feedDb) = camel::crossfeedValues(settings_.crossfeed);
    }
    settings_.crossfeed.preset = preset;
    showDsp();
    settingsChanged();
}

void MainWindow::OnCrossfeedValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) {
    const double cutoff = CrossfeedCutoffBox().Value(), feed = CrossfeedFeedBox().Value();
    if (populating_ || std::isnan(cutoff) || std::isnan(feed)) return;
    settings_.crossfeed.cutoffHz = uint32_t(std::clamp(std::lround(cutoff), long(camel::CrossfeedSettings::kMinCutoffHz),
                                                       long(camel::CrossfeedSettings::kMaxCutoffHz)));
    settings_.crossfeed.feedDb =
        std::clamp(std::round(feed * 10) / 10, camel::CrossfeedSettings::kMinFeedDb, camel::CrossfeedSettings::kMaxFeedDb);
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnConvolutionToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (populating_) return;
    settings_.convolution.enabled = ConvolutionSwitch().IsOn();
    showDspSummary();
    settingsChanged();
}

void MainWindow::OnConvolutionChooseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    const hstring types = tr(L"Convolution_FileTypes");
    const COMDLG_FILTERSPEC filter[] = {{types.c_str(), L"*.wav;*.zip"}};
    dialog->SetFileTypes(1, filter);
    winrt::com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->Show(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()))) || FAILED(dialog->GetResult(item.put())) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    settings_.convolution.file = camel::toUtf8(path);
    CoTaskMemFree(path);
    settings_.convolution.enabled = true;
    showDsp();
    settingsChanged();
}

void MainWindow::OnConvolutionRemoveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settings_.convolution = {};
    showDsp();
    settingsChanged();
}

// The playback-bar DSP button: each node on or off, the EQ presets, and the DSP settings.
void MainWindow::OnDspMenuOpening(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) {
    auto items = DspMenu().Items();
    items.Clear();
    const auto toggle = [&](camel::audio::DspNode node, bool on, bool available, auto flip) {
        ToggleMenuFlyoutItem item;
        item.Text(tr(dspNodeKey(node)));
        item.IsChecked(on);
        item.IsEnabled(available);
        item.Click([this, flip](auto&&, auto&&) {
            flip();
            showDsp();
            settingsChanged();
        });
        items.Append(item);
    };
    for (const auto node : settings_.dspOrder) {
        switch (node) {
        case camel::audio::DspNode::Eq:
            toggle(node, settings_.eq.enabled, true, [this] { settings_.eq.enabled = !settings_.eq.enabled; });
            break;
        case camel::audio::DspNode::Crossfeed:
            toggle(node, settings_.crossfeed.enabled, true,
                   [this] { settings_.crossfeed.enabled = !settings_.crossfeed.enabled; });
            break;
        case camel::audio::DspNode::Convolution:
            toggle(node, settings_.convolution.enabled && !settings_.convolution.file.empty(),
                   !settings_.convolution.file.empty(),
                   [this] { settings_.convolution.enabled = !settings_.convolution.enabled; });
            break;
        }
    }
    MenuFlyoutSubItem replayGain;
    replayGain.Text(tr(L"Settings_ReplayGain/Text"));
    constexpr const wchar_t* kModes[] = {L"Settings_ReplayGainOff/Content", L"Settings_ReplayGainTrack/Content",
                                         L"Settings_ReplayGainAlbum/Content", L"Settings_ReplayGainAuto/Content"};
    for (int mode = 0; mode < int(std::size(kModes)); ++mode) {
        RadioMenuFlyoutItem item;
        item.Text(tr(kModes[mode]));
        item.GroupName(L"ReplayGainModes");
        item.IsChecked(int(settings_.replayGain.mode) == mode);
        item.Click([this, mode](auto&&, auto&&) {
            settings_.replayGain.mode = camel::ReplayGainMode(mode);
            showReplayGain();
            settingsChanged();
        });
        replayGain.Items().Append(item);
    }
    ToggleMenuFlyoutItem crossfade;
    crossfade.Text(tr(L"Settings_Crossfade/Text"));
    crossfade.IsChecked(settings_.crossfade.enabled);
    crossfade.Click([this](auto&&, auto&&) {
        settings_.crossfade.enabled = !settings_.crossfade.enabled;
        showCrossfade();
        settingsChanged();
    });
    items.Append(MenuFlyoutSeparator());
    items.Append(crossfade);
    items.Append(replayGain);
    if (!settings_.eq.presets.empty()) {
        MenuFlyoutSubItem presets;
        presets.Text(tr(L"Dsp_MenuEqPresets"));
        for (const auto& preset : settings_.eq.presets) {
            RadioMenuFlyoutItem item;
            item.Text(ui::utf8(preset.name));
            item.GroupName(L"EqPresets");
            item.IsChecked(preset.name == settings_.eq.preset);
            item.Click([this, name = preset.name](auto&&, auto&&) {
                const auto found = std::ranges::find(settings_.eq.presets, name, &camel::EqPreset::name);
                if (found == settings_.eq.presets.end()) return;
                settings_.eq.bands = found->bands;
                settings_.eq.preset = name;
                showDsp();
                settingsChanged();
            });
            presets.Items().Append(item);
        }
        items.Append(MenuFlyoutSeparator());
        items.Append(presets);
    }
    items.Append(MenuFlyoutSeparator());
    MenuFlyoutItem open;
    open.Text(tr(L"Dsp_MenuSettings"));
    open.Click([this](auto&&, auto&&) { showSettingsCard(DspCard()); });
    items.Append(open);
}

void MainWindow::OnCheckUpdatesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    checkForUpdates(true);
}

// The startup and daily checks skip a skipped version and stay silent on failure; the About button does neither.
winrt::fire_and_forget MainWindow::checkForUpdates(bool manual) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    if (manual) {
        CheckUpdatesButton().IsEnabled(false);
        UpdateStatusText().Text(tr(L"Update_Checking"));
    }
    co_await resume_background();
    std::optional<update::Release> release;
    std::string failure;
    try {
        update::cleanTemp(camel::dataDirectory());
        release = update::check(CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        failure = e.what();
        camel::log::warn("update check: %s", e.what());
    }
    queue.TryEnqueue([weak, manual, release, failure] {
        auto self = weak.get();
        if (!self) return;
        if (manual) {
            self->CheckUpdatesButton().IsEnabled(true);
            self->UpdateStatusText().Text(!failure.empty() ? ui::format(L"Update_CheckFailed", {std::wstring(ui::utf8(failure))})
                                          : release        ? ui::format(L"Update_Found", {toWide(release->version)})
                                                           : tr(L"Update_UpToDate"));
        }
        if (!release || (!manual && release->version == self->settings_.skippedVersion)) return;
        if (self->release_ && self->release_->version == release->version) { // keeps a running or finished download
            self->UpdateBar().IsOpen(true);
            return;
        }
        camel::log::info("update: CamelPlayer %s is available", release->version.c_str());
        self->release_ = release;
        self->updateFiles_ = {};
        self->showUpdateOffer();
    });
}

void MainWindow::showUpdateOffer() {
    if (!release_ || (RepairBar().IsOpen() && RepairBar().Severity() != InfoBarSeverity::Success)) return;
    const auto& release = *release_;
    const std::wstring version = toWide(release.version);
    auto bar = UpdateBar();
    bar.Severity(InfoBarSeverity::Informational);
    bar.Title(ui::format(updateFiles_.files.empty() ? L"Update_AvailableTitle" : L"Update_ReadyTitle", {version}));
    bar.Message(ui::format(L"Update_AvailableText", {L"" CAMEL_VERSION_STRING}));
    UpdateInstallButton().Content(box_value(tr(updateFiles_.files.empty() ? L"Update_Install" : L"Update_Restart")));
    UpdateInstallButton().Visibility(Visibility::Visible);
    UpdateNotesButton().Visibility(release.notes.empty() ? Visibility::Collapsed : Visibility::Visible);
    UpdateSkipButton().Visibility(Visibility::Visible);
    UpdateProgress().Visibility(Visibility::Collapsed);
    UpdateCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(true);
    bar.IsOpen(true);
}

winrt::fire_and_forget MainWindow::OnUpdateInstallClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!release_) co_return;
    if (!updateFiles_.files.empty()) {
        restartToUpdate(*release_, updateFiles_);
        co_return;
    }
    const auto release = *release_;
    const std::wstring version = toWide(release.version);
    updateToken_ = {};
    const auto token = updateToken_;
    UpdateBar().Title(ui::format(L"Update_Downloading", {version, L""}));
    UpdateBar().IsClosable(false);
    UpdateInstallButton().Visibility(Visibility::Collapsed);
    UpdateNotesButton().Visibility(Visibility::Collapsed);
    UpdateSkipButton().Visibility(Visibility::Collapsed);
    UpdateProgress().IsIndeterminate(true);
    UpdateProgress().Visibility(Visibility::Visible);
    UpdateCancelButton().Visibility(Visibility::Visible);
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    update::Prepared files;
    std::string failure;
    auto shownPercent = std::make_shared<int>(-1);
    try {
        files = update::prepare(
            release,
            [weak, queue, version, shownPercent](uint64_t done, uint64_t total) {
                if (total == 0) return;
                const int percent = int(done * 100 / total);
                if (percent == *shownPercent) return;
                *shownPercent = percent;
                queue.TryEnqueue([weak, version, percent] {
                    auto self = weak.get();
                    if (!self) return;
                    self->UpdateProgress().IsIndeterminate(false);
                    self->UpdateProgress().Value(percent);
                    self->UpdateBar().Title(ui::format(L"Update_Downloading", {version, std::to_wstring(percent) + L"%"}));
                });
            },
            token);
    } catch (const std::exception& e) {
        failure = e.what();
    }
    const bool cancelled = token.cancelled();
    queue.TryEnqueue([weak, release, files, failure, cancelled] {
        auto self = weak.get();
        if (!self) return;
        if (!failure.empty()) {
            camel::log::warn("update download: %s", failure.c_str());
            self->showUpdateOffer();
            if (cancelled) return;
            self->UpdateBar().Severity(InfoBarSeverity::Error);
            self->UpdateBar().Title(tr(L"Update_DownloadFailedTitle"));
            self->UpdateBar().Message(ui::utf8(failure));
            self->UpdateInstallButton().Content(box_value(tr(L"Update_Retry")));
            return;
        }
        self->updateFiles_ = files;
        self->showUpdateOffer();
        self->restartToUpdate(release, files);
    });
}

void MainWindow::OnUpdateCancelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    updateToken_.cancel();
}

winrt::fire_and_forget MainWindow::OnUpdateNotesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!release_) co_return;
    const auto release = *release_;
    co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), ui::format(L"Update_NotesTitle", {toWide(release.version)}),
                                       ui::utf8(release.notes)));
}

void MainWindow::OnUpdateSkipClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!release_) return;
    const auto& release = *release_;
    settings_.skippedVersion = release.version;
    applySettingsNow();
    UpdateBar().IsOpen(false);
}

// Hands the verified files to CamelUpdater.exe and exits the normal way, so the queue and position are saved. The
// running version's own files repair the installation.
winrt::fire_and_forget MainWindow::restartToUpdate(update::Release release, update::Prepared prepared) {
    auto strong = get_strong();
    const bool repair = release.version == CAMEL_VERSION_STRING;
    auto bar = repair ? RepairBar() : UpdateBar();
    if (server_ && server_->snapshot().playback.state == PlaybackState::Playing && !inTray_) {
        ContentDialog dialog;
        ui::placeDialog(dialog, ContentLayout().XamlRoot());
        dialog.Title(box_value(tr(repair ? L"Repair_ConfirmTitle" : L"Update_ConfirmTitle")));
        dialog.Content(
            box_value(repair ? tr(L"Repair_ConfirmText") : ui::format(L"Update_ConfirmText", {toWide(release.version)})));
        dialog.PrimaryButtonText(tr(repair ? L"Repair_ConfirmPrimary" : L"Update_ConfirmPrimary"));
        dialog.CloseButtonText(tr(L"Update_ConfirmCancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await ui::showDialog(dialog) != ContentDialogResult::Primary) {
            if (repair) showRepairOffer();
            co_return;
        }
    }
    try {
        update::startUpdater(programDirectory(), prepared, camel::dataDirectory(), release.version, CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        camel::log::warn("update: %s", e.what());
        if (repair) showRepairOffer();
        bar.Severity(InfoBarSeverity::Error);
        bar.Title(ui::format(L"Update_StartFailed", {std::wstring(ui::utf8(e.what()))}));
        co_return;
    }
    camel::log::info("update: CamelUpdater takes over to %s %s", repair ? "repair" : "install", release.version.c_str());
    exiting_ = true;
    Close();
}

void MainWindow::showDataRecovery() {
    if (!server_) return;
    const auto recovery = server_->dataRecovery();
    std::wstring text;
    // Sentences after a full-width full stop need no space.
    const auto add = [&text](hstring const& sentence) {
        if (!text.empty() && text.back() != L'。') text += L' ';
        text += sentence;
    };
    if (recovery.settings == camel::SettingsRecovery::RestoredLastGood) add(tr(L"Recovery_SettingsRestored"));
    if (recovery.settings == camel::SettingsRecovery::Reset) add(tr(L"Recovery_SettingsReset"));
    if (recovery.library == DataRecovery::Library::RestoredBackup) add(tr(L"Recovery_LibraryRestored"));
    if (recovery.library == DataRecovery::Library::Rebuilt) add(tr(L"Recovery_LibraryRebuilt"));
    if (text.empty()) return;
    add(ui::format(L"Recovery_Kept", {camel::dataDirectory().wstring()}));
    RecoveryBar().Title(tr(L"Recovery_Title"));
    RecoveryBar().Message(text);
    RecoveryBar().IsOpen(true);
}

void MainWindow::OnCheckInstallClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    checkInstallation(true);
}

// A version whose files have not passed a full check yet gets one, later starts compare sizes; the About button always
// checks fully. A development build has no install-manifest.txt and nothing to check.
winrt::fire_and_forget MainWindow::checkInstallation(bool manual) {
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    if (manual) {
        CheckInstallButton().IsEnabled(false);
        InstallStatusText().Text(tr(L"Install_Checking"));
    }
    co_await resume_background();
    const auto data = camel::dataDirectory();
    const bool full = manual || !update::verified(data, CAMEL_VERSION_STRING);
    std::optional<update::InstallCheck> check;
    std::string failure;
    try {
        check = update::checkInstallation(programDirectory(), full);
        if (check && check->intact() && full) update::markVerified(data, CAMEL_VERSION_STRING);
    } catch (const std::exception& e) {
        failure = e.what();
        camel::log::warn("installation check: %s", e.what());
    }
    if (check && !check->intact())
        camel::log::warn("installation check: %zu of %zu files missing, %zu damaged", check->missing.size(), check->files,
                         check->damaged.size());
    queue.TryEnqueue([weak, manual, check, failure] {
        auto self = weak.get();
        if (!self) return;
        if (manual) {
            self->CheckInstallButton().IsEnabled(true);
            self->InstallStatusText().Text(
                !failure.empty() ? ui::format(L"Install_CheckFailed", {std::wstring(ui::utf8(failure))})
                : !check         ? tr(L"Install_NoManifest")
                : check->intact()
                    ? ui::format(L"Install_Intact", {std::to_wstring(check->files)})
                    : ui::format(L"Install_Problems", {std::to_wstring(check->missing.size() + check->damaged.size())}));
        }
        if (!check || check->intact()) return;
        self->installCheck_ = check;
        // A repair result still shown keeps its message; its button tries again.
        if (!manual && self->RepairBar().IsOpen()) return;
        self->showRepairOffer();
    });
}

void MainWindow::showRepairOffer() {
    if (!installCheck_) return;
    const auto& check = installCheck_.value();
    auto bar = RepairBar();
    bar.Severity(InfoBarSeverity::Warning);
    bar.Title(tr(L"Repair_Title"));
    bar.Message(
        ui::format(L"Repair_Text", {std::to_wstring(check.missing.size() + check.damaged.size()), L"" CAMEL_VERSION_STRING}));
    RepairButton().Content(box_value(tr(L"Repair_Install")));
    RepairButton().Visibility(Visibility::Visible);
    RepairDetailsButton().Visibility(Visibility::Visible);
    RepairSkipButton().Visibility(Visibility::Visible);
    RepairProgress().Visibility(Visibility::Collapsed);
    RepairCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(false);
    bar.IsOpen(true);
    UpdateBar().IsOpen(false);
}

// What the updater reported after a repair; Repair stays for one that did not complete.
void MainWindow::showRepairResult(wchar_t const* title, wchar_t const* text, InfoBarSeverity severity) {
    auto bar = RepairBar();
    bar.Severity(severity);
    bar.Title(tr(title));
    bar.Message(tr(text));
    const bool done = severity == InfoBarSeverity::Success;
    RepairButton().Content(box_value(tr(L"Update_Retry")));
    RepairButton().Visibility(done ? Visibility::Collapsed : Visibility::Visible);
    RepairDetailsButton().Visibility(Visibility::Collapsed);
    RepairSkipButton().Visibility(done ? Visibility::Collapsed : Visibility::Visible);
    RepairProgress().Visibility(Visibility::Collapsed);
    RepairCancelButton().Visibility(Visibility::Collapsed);
    bar.IsClosable(done);
    bar.IsOpen(true);
}

// Downloads the running version again and installs it over the program folder like an update, which replaces only the
// program's own files and leaves anything else in the folder where it is. When GitHub no longer has it, a newer release
// is offered instead, which replaces the files as well.
winrt::fire_and_forget MainWindow::OnRepairClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    repairToken_ = {};
    const auto token = repairToken_;
    RepairBar().Severity(InfoBarSeverity::Warning);
    RepairBar().Title(tr(L"Repair_Finding"));
    RepairBar().Message(L"");
    RepairButton().Visibility(Visibility::Collapsed);
    RepairDetailsButton().Visibility(Visibility::Collapsed);
    RepairSkipButton().Visibility(Visibility::Collapsed);
    RepairProgress().IsIndeterminate(true);
    RepairProgress().Visibility(Visibility::Visible);
    RepairCancelButton().Visibility(Visibility::Visible);
    auto weak = get_weak();
    auto queue = DispatcherQueue();
    co_await resume_background();
    std::optional<update::Release> release, newer;
    update::Prepared files;
    std::string failure;
    auto shownPercent = std::make_shared<int>(-1);
    try {
        release = update::releaseOf(CAMEL_VERSION_STRING);
        if (!release) {
            newer = update::check(CAMEL_VERSION_STRING);
        } else {
            files = update::prepare(
                *release,
                [weak, queue, shownPercent](uint64_t done, uint64_t total) {
                    if (total == 0) return;
                    const int percent = int(done * 100 / total);
                    if (percent == *shownPercent) return;
                    *shownPercent = percent;
                    queue.TryEnqueue([weak, percent] {
                        auto self = weak.get();
                        if (!self) return;
                        self->RepairProgress().IsIndeterminate(false);
                        self->RepairProgress().Value(percent);
                        self->RepairBar().Title(ui::format(L"Repair_Downloading", {std::to_wstring(percent) + L"%"}));
                    });
                },
                token);
        }
    } catch (const std::exception& e) {
        failure = e.what();
    }
    const bool cancelled = token.cancelled();
    queue.TryEnqueue([weak, release, newer, files, failure, cancelled] {
        auto self = weak.get();
        if (!self) return;
        if (failure.empty() && release) {
            self->restartToUpdate(*release, files);
            return;
        }
        if (failure.empty() && newer) {
            camel::log::info("repair: %s is no longer on GitHub; offering %s", CAMEL_VERSION_STRING, newer->version.c_str());
            self->RepairBar().IsOpen(false);
            self->release_ = newer;
            self->updateFiles_ = {};
            self->showUpdateOffer();
            self->UpdateBar().Message(tr(L"Repair_UseUpdate"));
            return;
        }
        if (!failure.empty()) camel::log::warn("repair: %s", failure.c_str());
        self->showRepairOffer();
        if (cancelled) return;
        self->RepairBar().Severity(InfoBarSeverity::Error);
        self->RepairBar().Title(tr(failure.empty() ? L"Repair_GoneTitle" : L"Repair_DownloadFailedTitle"));
        self->RepairBar().Message(failure.empty() ? tr(L"Repair_GoneText") : hstring(ui::utf8(failure)));
        self->RepairButton().Content(box_value(tr(L"Update_Retry")));
    });
}

winrt::fire_and_forget MainWindow::OnRepairDetailsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    if (!installCheck_) co_return;
    std::wstring text;
    for (const auto& path : installCheck_->missing)
        text += std::wstring(tr(L"Repair_Missing")) + L"  " + path.generic_wstring() + L"\n";
    for (const auto& path : installCheck_->damaged)
        text += std::wstring(tr(L"Repair_Damaged")) + L"  " + path.generic_wstring() + L"\n";
    co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), tr(L"Repair_DetailsTitle"), hstring(text)));
}

void MainWindow::OnRepairSkipClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    RepairBar().IsOpen(false);
    showUpdateOffer();
}

void MainWindow::OnRepairCancelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    repairToken_.cancel();
}

winrt::fire_and_forget MainWindow::showUpdateResult() {
    auto strong = get_strong();
    const auto data = camel::dataDirectory();
    const auto result = update::readResult(data);
    if (!result) co_return;
    const std::wstring version = toWide(result->version);
    const bool repair = result->version == CAMEL_VERSION_STRING;
    if (result->kind == update::Result::Kind::Installed) {
        if (!repair || !update::signalStarted(data)) {
            update::removeResult(data);
            co_return;
        }
        if (result->previous == result->version) {
            showRepairResult(L"Repair_DoneTitle", L"Repair_DoneText", InfoBarSeverity::Success);
            co_return;
        }
        std::ifstream in(programDirectory() / L"CHANGELOG.md", std::ios::binary);
        const std::string changelog{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        co_await ui::showDialog(textDialog(ContentLayout().XamlRoot(), ui::format(L"Update_InstalledTitle", {version}),
                                           ui::utf8(update::changelogSince(changelog, result->previous, result->version))));
        co_return;
    }
    update::removeResult(data);
    const bool undone = result->kind == update::Result::Kind::Undone;
    const bool failed = undone || result->kind == update::Result::Kind::Failed;
    if (repair) {
        showRepairResult(failed ? L"Repair_FailedTitle" : L"Repair_CancelledTitle",
                         failed ? L"Repair_FailedText" : L"Update_CancelledText", InfoBarSeverity::Warning);
        co_return;
    }
    if (failed && !undone) { // only a version that did not start is skipped
        settings_.skippedVersion = result->version;
        applySettingsNow();
    }
    auto bar = UpdateBar();
    bar.Severity(InfoBarSeverity::Warning);
    bar.Title(ui::format(failed ? L"Update_FailedTitle" : L"Update_CancelledTitle", {version}));
    bar.Message(failed ? ui::format(undone ? L"Update_UndoneText" : L"Update_FailedText", {version})
                       : tr(L"Update_CancelledText"));
    UpdateInstallButton().Visibility(Visibility::Collapsed);
    UpdateNotesButton().Visibility(Visibility::Collapsed);
    UpdateSkipButton().Visibility(Visibility::Collapsed);
    bar.IsOpen(true);
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
