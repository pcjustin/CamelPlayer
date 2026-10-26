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
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>

#include "UiText.h"
#include "ArtworkLoader.h"
#include "Common/settings/Settings.h"
#include "Common/util/Log.h"
#include "Common/util/Measure.h"
#include "Common/util/Text.h"
#include "Server/Playback/DeviceMatch.h"
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

namespace {

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

// Theme brushes follow light, dark and high-contrast themes.
Media::Brush themeBrush(const wchar_t* key) {
    return Application::Current().Resources().TryLookup(box_value(key)).try_as<Media::Brush>();
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

std::vector<hstring> rateItems() {
    std::vector<hstring> items{tr(L"Rate_FollowFile")};
    for (uint32_t rate : kFixedRates) {
        wchar_t text[32];
        swprintf(text, 32, L"%.1f kHz", rate / 1000.0);
        items.push_back(text);
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
        presenter.PreferredMinimumWidth(static_cast<int32_t>(800 * scale));
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
        if (std::filesystem::exists(iconPath, error)) AppWindow().SetIcon(iconPath.c_str());
    }

    if (!server_) server_ = createLocalServer();
    settings_ = server_->snapshot().settings;
    auto dq = DispatcherQueue();
    server_->setListener([dq, weak = get_weak()](const Event& event) {
        dq.TryEnqueue([weak, snapshot = event.snapshot] {
            if (auto self = weak.get()) self->applySnapshot(snapshot);
        });
    });
    using namespace std::chrono_literals;
    uiTimer_ = dq.CreateTimer();
    uiTimer_.Interval(250ms);
    uiTimer_.Tick([this](auto&&, auto&&) { refreshFromServer(); });
    uiTimer_.Start();
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
    setButtonTip(PlayPauseButton(), L"Tip_PlayPause");
    setButtonTip(StopButton(), L"Tip_Stop");
    setButtonTip(PreviousButton(), L"Tip_Previous");
    setButtonTip(NextButton(), L"Tip_Next");
    setButtonTip(ShuffleButton(), L"Tip_Shuffle");
    setButtonTip(RepeatButton(), L"Tip_Repeat");
    setButtonTip(QueueToggleButton(), L"Tip_Queue");

    populateSettings();
    const ui::PlaylistMenu playlistMenu = [this](MenuFlyout const& menu, std::function<ui::SourceFetch()> selection) {
        extendPlaylistMenu(menu, std::move(selection));
    };
    get_self<LibraryView>(Library())->attach(server_, [this](Command command) { submitCommand(std::move(command)); }, playlistMenu);
    get_self<PlaylistView>(Playlists())
        ->attach(server_, {[this](Command command) { submitCommand(std::move(command)); }, [this] { refreshPlaylists(); },
                           [this](int64_t playlist) {
                               // A playlist just imported is listed in the navigation once it is refreshed.
                               const auto tag = playlist ? L"playlist:" + to_hstring(playlist) : hstring(L"playlists");
                               refreshPlaylists([this, tag] { selectPage(tag); });
                           },
                           [this](hstring const& title, hstring const& message, bool error) {
                               showPlaylistNotice(title, message, error);
                           },
                           [this] { return Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()); }, playlistMenu});
    noticeTimer_ = DispatcherQueue().CreateTimer();
    noticeTimer_.Interval(std::chrono::seconds(10));
    noticeTimer_.Tick([this](auto&&, auto&&) {
        noticeTimer_.Stop();
        PlaylistNotice().IsOpen(false);
    });
    PlaylistNotice().Closed([this](auto&&, auto&&) { noticeUndo_.reset(); });
    refreshPlaylists();
    get_self<HomeView>(Home())->attach(server_, {[this](int64_t album, hstring const& title) {
                                                     selectPage(L"albums");
                                                     get_self<LibraryView>(Library())->showAlbum(album, title);
                                                 },
                                                 [this](hstring const& artist) {
                                                     selectPage(L"albums");
                                                     get_self<LibraryView>(Library())->showArtist(artist);
                                                 },
                                                 [this](hstring const& list) { viewAll(list); }});
    std::ifstream queueState(camel::dataDirectory() / L"queue-panel-open");
    queueState >> queueOpen_;
    QueueList().ItemsSource(*queueItems_);
    updateQueuePanel();
    Nav().SizeChanged([this](auto&&, auto&&) { fitNavigationPane(); });
    if (camel::measure::measuring()) Media::CompositionTarget::Rendering([this](auto&&, auto&&) { recordFrame(); });
    ContentLayout().Loaded([this](auto&&, auto&&) {
        if (camel::measure::measuring())
            camel::log::info("measure: main window loaded %.0f ms after process start", camel::measure::sinceProcessStart());
        showFirstRunGuideIfDue(); // a dialog needs a XamlRoot
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
    if (winrt::com_ptr<IPropertyStore> properties;
        SUCCEEDED(SHGetPropertyStoreForWindow(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), IID_PPV_ARGS(properties.put())))) {
        PROPVARIANT id;
        if (SUCCEEDED(InitPropVariantFromString(L"CamelPlayer.CamelPlayer", &id))) {
            properties->SetValue(PKEY_AppUserModel_ID, id);
            PropVariantClear(&id);
        }
    }
    if (server_) {
        media_ = std::make_unique<ui::MediaControls>(
            Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()), DispatcherQueue(),
            ui::MediaControls::Handlers{[this](CommandKind kind) { submitKind(kind); },
                                        [this](double seconds) {
                                            const auto s = server_->snapshot().playback;
                                            if (!s.source) return;
                                            Command command;
                                            command.kind = CommandKind::Seek;
                                            command.frame = secondsToFrames(seconds, s.source->sampleRate);
                                            submitCommand(std::move(command));
                                        }},
            server_);
        hotkeys_ = std::make_unique<ui::GlobalHotkeys>([this](ui::GlobalHotkeys::Action action) {
            using Action = ui::GlobalHotkeys::Action;
            submitKind(action == Action::PlayPause ? CommandKind::TogglePause
                       : action == Action::Previous ? CommandKind::Previous
                                                    : CommandKind::Next);
        });
        applyHotkeys();
    }
    AppWindow().Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& e) {
        if (exiting_ || settings_.closeAction == CloseAction::Exit) return;
        e.Cancel(true);
        hideToTray();
    });

    Closed([this](auto&&, auto&&) {
        tray_.reset();
        artworkToken_.cancel();
        uiTimer_.Stop();
        seekTimer_.Stop();
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
    uiTimer_.Stop();
    get_self<LibraryView>(Library())->setActive(false);
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

void MainWindow::showFromTray() {
    const bool wasInTray = inTray_;
    inTray_ = false;
    AppWindow().Show();
    if (const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized)
        presenter.Restore();
    Activate();
    SetForegroundWindow(Microsoft::UI::GetWindowFromWindowId(AppWindow().Id()));
    if (!wasInTray) return;
    uiTimer_.Start();
    get_self<LibraryView>(Library())->setActive(true);
    refreshFromServer();
}

void MainWindow::populateSettings() {
    populating_ = true;
    fill(BackendCombo(), {L"WASAPI", L"ASIO", L"Kernel Streaming"}, int(settings_.backend));
    for (const auto& [combo, format] : decoderCombos()) {
        const auto registered = decodersFor(format);
        std::vector<hstring> names;
        for (DecoderKind k : registered) names.push_back(ui::utf8(decoderName(k)));
        const auto chosen = std::find(registered.begin(), registered.end(), decoderFor(settings_.decoderByFormat, format));
        fill(combo, names, int(chosen - registered.begin()));
    }
    for (const auto& [combo, format] : pcmBitsCombos())
        fill(combo, {L"24-bit", L"16-bit"}, pcmBitsFor(settings_.pcmBitsByFormat, format) == 16 ? 1 : 0);
    fill(ConvertCombo(), {tr(L"Convert_Off"), tr(L"Convert_On")}, settings_.convertWhenUnsupported ? 1 : 0);
    fill(ResampleQualityCombo(), {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")},
         int(settings_.resampleQuality));
    AutoplaySwitch().IsOn(settings_.autoplay);
    QueueAutoplaySwitch().IsOn(settings_.autoplay);
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
    WasapiPollBufferBox().Value(settings_.wasapi.pollBufferMs);
    fill(WasapiMmcssTaskCombo(), tasks, int(settings_.wasapi.mmcssTask));
    fill(WasapiMmcssPriorityCombo(), priorities, int(settings_.wasapi.mmcssPriority));
    showTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), settings_.wasapi.timing);

    AsioBufferBox().Value(double(settings_.asio.bufferSize));
    AsioLeftBox().Value(settings_.asio.outLeft + 1);
    AsioRightBox().Value(settings_.asio.outRight + 1);
    fill(AsioRateCombo(), rates, rateIndex(settings_.asio.fixedRate));
    showTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), settings_.asio.timing);

    fill(KsModeCombo(), {tr(L"KsMode_Auto"), tr(L"KsMode_RtEvent"), tr(L"KsMode_RtPoll"), tr(L"KsMode_Standard")},
         int(settings_.ks.mode));
    KsBufferBox().Value(settings_.ks.bufferMs);
    KsPacketCountBox().Value(settings_.ks.packetCount);
    KsPacketMsBox().Value(settings_.ks.packetMs);
    fill(KsFormatCombo(), formats, formatIndex(settings_.ks.sampleType));
    fill(KsRateCombo(), rates, rateIndex(settings_.ks.fixedRate));
    fill(KsMmcssTaskCombo(), tasks, int(settings_.ks.mmcssTask));
    fill(KsMmcssPriorityCombo(), priorities, int(settings_.ks.mmcssPriority));
    showTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), settings_.ks.timing);

    const int lang = settings_.language == "zh-TW" ? 1 : settings_.language == "en-US" ? 2 : 0;
    fill(LanguageCombo(), {tr(L"Lang_System"), L"繁體中文", L"English"}, lang);
    fill(CloseActionCombo(), {tr(L"Close_Tray"), tr(L"Close_Exit")}, settings_.closeAction == CloseAction::Exit ? 1 : 0);
    populateDeviceLists(server_->snapshot().devices);
    populating_ = false;
    updateBackendGroups();
}

void MainWindow::populateDeviceLists(const DeviceCatalog& devices) {
    const bool was = populating_;
    populating_ = true;
    WasapiDeviceCombo().IsEnabled(devices.revision != 0);
    AsioDriverCombo().IsEnabled(devices.revision != 0);
    KsDeviceCombo().IsEnabled(devices.revision != 0);
    // A saved device that is missing stays selected as "not connected"; it is never replaced by another device.
    const auto withSaved = [](std::vector<DeviceInfo> list, const std::string& saved) {
        if (!saved.empty() && std::none_of(list.begin(), list.end(), [&](const DeviceInfo& d) { return d.id == saved; }))
            list.push_back({saved, ""});
        return list;
    };
    const auto label = [](const DeviceInfo& d) { return d.name.empty() ? tr(L"Device_NotConnected") : ui::utf8(d.name); };
    wasapiDevices_ = withSaved(devices.wasapi, settings_.wasapi.deviceId);
    std::vector<hstring> names{tr(L"Device_Default")};
    int sel = 0;
    for (size_t i = 0; i < wasapiDevices_.size(); i++) {
        names.push_back(label(wasapiDevices_[i]));
        if (wasapiDevices_[i].id == settings_.wasapi.deviceId) sel = int(i + 1);
    }
    fill(WasapiDeviceCombo(), names, sel);

    auto fillList = [&](ComboBox const& combo, const std::vector<DeviceInfo>& list, const std::string& current) {
        std::vector<hstring> n;
        int s = 0;
        for (size_t i = 0; i < list.size(); i++) {
            n.push_back(label(list[i]));
            if (list[i].id == current) s = int(i);
        }
        if (n.empty()) n.push_back(tr(L"Device_None"));
        fill(combo, n, s);
    };
    asioDrivers_ = withSaved(devices.asio, settings_.asio.driverClsid);
    fillList(AsioDriverCombo(), asioDrivers_, settings_.asio.driverClsid);
    ksPins_ = withSaved(devices.ks, settings_.ks.deviceId);
    fillList(KsDeviceCombo(), ksPins_, settings_.ks.deviceId);
    populating_ = was;
}

std::array<std::pair<ComboBox, AudioFormat>, 6> MainWindow::decoderCombos() {
    return {{{DecoderCombo(), AudioFormat::Flac},
             {AlacDecoderCombo(), AudioFormat::Alac},
             {WavDecoderCombo(), AudioFormat::Wav},
             {AiffDecoderCombo(), AudioFormat::Aiff},
             {AacDecoderCombo(), AudioFormat::Aac},
             {Mp3DecoderCombo(), AudioFormat::Mp3}}};
}

std::array<std::pair<ComboBox, AudioFormat>, 2> MainWindow::pcmBitsCombos() {
    return {{{AacBitsCombo(), AudioFormat::Aac}, {Mp3BitsCombo(), AudioFormat::Mp3}}};
}

void MainWindow::readSettingsFromUi() {
    auto idx = [](ComboBox const& c) { return std::max(0, c.SelectedIndex()); };
    settings_.backend = Backend(std::min(idx(BackendCombo()), 2));
    for (const auto& [combo, format] : decoderCombos()) {
        const auto registered = decodersFor(format);
        const DecoderKind chosen = registered[std::min<size_t>(idx(combo), registered.size() - 1)];
        if (chosen != decoderFor(settings_.decoderByFormat, format)) settings_.decoderByFormat[format] = chosen;
    }
    for (const auto& [combo, format] : pcmBitsCombos()) {
        const uint32_t bits = idx(combo) == 1 ? 16 : 24;
        if (bits != pcmBitsFor(settings_.pcmBitsByFormat, format)) settings_.pcmBitsByFormat[format] = bits;
    }
    settings_.convertWhenUnsupported = idx(ConvertCombo()) == 1;
    settings_.resampleQuality = ResampleQuality(std::min(idx(ResampleQualityCombo()), 2));
    settings_.autoplay = AutoplaySwitch().IsOn();

    const int dev = idx(WasapiDeviceCombo());
    if (shownDevicesRevision_ != 0)
        settings_.wasapi.deviceId = dev == 0 || dev > int(wasapiDevices_.size()) ? "" : wasapiDevices_[size_t(dev - 1)].id;
    settings_.wasapi.shared = idx(WasapiShareCombo()) == 1;
    settings_.wasapi.sharedResampleQuality = ResampleQuality(std::min(idx(WasapiSharedQualityCombo()), 2));
    settings_.wasapi.eventDriven = idx(WasapiModeCombo()) == 0;
    settings_.wasapi.period = WasapiPeriod(std::min(idx(WasapiPeriodCombo()), 2));
    settings_.wasapi.customPeriodMs = numberOr(WasapiPeriodBox(), settings_.wasapi.customPeriodMs);
    settings_.wasapi.sampleType = kFormatChoices[std::min(idx(WasapiFormatCombo()), 4)];
    settings_.wasapi.fixedRate = rateAt(idx(WasapiRateCombo()));
    settings_.wasapi.pollBufferMs = std::clamp(numberOr(WasapiPollBufferBox(), settings_.wasapi.pollBufferMs), 20.0, 2000.0);
    settings_.wasapi.mmcssTask = MmcssTask(std::min(idx(WasapiMmcssTaskCombo()), 2));
    settings_.wasapi.mmcssPriority = MmcssPriority(std::min(idx(WasapiMmcssPriorityCombo()), 2));
    readTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), settings_.wasapi.timing);

    const int drv = idx(AsioDriverCombo());
    if (drv < int(asioDrivers_.size())) settings_.asio.driverClsid = asioDrivers_[size_t(drv)].id;
    settings_.asio.bufferSize = long(numberOr(AsioBufferBox(), double(settings_.asio.bufferSize)));
    settings_.asio.outLeft = int(numberOr(AsioLeftBox(), settings_.asio.outLeft + 1)) - 1;
    settings_.asio.outRight = int(numberOr(AsioRightBox(), settings_.asio.outRight + 1)) - 1;
    settings_.asio.fixedRate = rateAt(idx(AsioRateCombo()));
    readTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), settings_.asio.timing);

    const int pin = idx(KsDeviceCombo());
    if (pin < int(ksPins_.size())) settings_.ks.deviceId = ksPins_[size_t(pin)].id;
    settings_.ks.mode = KsMode(std::min(idx(KsModeCombo()), 3));
    settings_.ks.bufferMs = numberOr(KsBufferBox(), settings_.ks.bufferMs);
    settings_.ks.packetCount = int(numberOr(KsPacketCountBox(), settings_.ks.packetCount));
    settings_.ks.packetMs = numberOr(KsPacketMsBox(), settings_.ks.packetMs);
    settings_.ks.sampleType = kFormatChoices[std::min(idx(KsFormatCombo()), 4)];
    settings_.ks.fixedRate = rateAt(idx(KsRateCombo()));
    settings_.ks.mmcssTask = MmcssTask(std::min(idx(KsMmcssTaskCombo()), 2));
    settings_.ks.mmcssPriority = MmcssPriority(std::min(idx(KsMmcssPriorityCombo()), 2));
    readTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), settings_.ks.timing);
    settings_.closeAction = idx(CloseActionCombo()) == 1 ? CloseAction::Exit : CloseAction::Tray;
}

void MainWindow::OnAutoplayToggled(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    if (populating_) return;
    const bool on = sender.as<ToggleSwitch>().IsOn();
    populating_ = true;
    AutoplaySwitch().IsOn(on);
    QueueAutoplaySwitch().IsOn(on);
    populating_ = false;
    settingsChanged();
}

void MainWindow::settingsChanged() {
    if (populating_ || !server_) return;
    const Backend previous = settings_.backend;
    readSettingsFromUi();
    if (settings_.backend != previous && !followDevice(previous) &&
        server_->snapshot().playback.state == PlaybackState::Playing) {
        // Never move sound to another device unannounced; its level may differ a lot.
        Command pause;
        pause.kind = CommandKind::Pause;
        submitCommand(std::move(pause));
    }
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    updateBackendGroups();
}

bool MainWindow::followDevice(Backend previous) {
    BackendNotice().IsOpen(false);
    const auto list = [&](Backend backend) -> const std::vector<DeviceInfo>& {
        return backend == Backend::Wasapi ? wasapiDevices_ : backend == Backend::Asio ? asioDrivers_ : ksPins_;
    };
    const auto savedId = [&](Backend backend) -> std::string& {
        return backend == Backend::Wasapi ? settings_.wasapi.deviceId
               : backend == Backend::Asio ? settings_.asio.driverClsid
                                          : settings_.ks.deviceId;
    };
    // An empty WASAPI choice follows the Windows default, whose name the catalog does not give; ASIO and KS open
    // their first entry.
    const auto& from = list(previous);
    const auto current = std::find_if(from.begin(), from.end(), [&](const DeviceInfo& d) { return d.id == savedId(previous); });
    const DeviceInfo* device = current != from.end() ? &*current : nullptr;
    if (!device && previous != Backend::Wasapi && savedId(previous).empty() && !from.empty()) device = &from.front();
    if (!device) return true;
    if (const auto match = matchDevice(previous, *device, settings_.backend, list(settings_.backend))) {
        savedId(settings_.backend) = *match;
        populateDeviceLists(server_->snapshot().devices);
        return true;
    }
    std::wstring message(tr(L"Backend_NoMatch"));
    const std::array<std::wstring, 2> values{
        std::wstring(ui::utf8(device->name)),
        std::wstring(BackendCombo().SelectedItem() ? unbox_value<hstring>(BackendCombo().SelectedItem()) : hstring{})};
    for (size_t i = 0; i < values.size(); ++i) {
        const std::wstring marker = L"{" + std::to_wstring(i) + L"}";
        if (const auto at = message.find(marker); at != std::wstring::npos) message.replace(at, marker.size(), values[i]);
    }
    BackendNotice().Title(tr(L"Backend_NoMatchTitle"));
    BackendNotice().Message(message);
    BackendNotice().IsOpen(true);
    return false;
}

void MainWindow::updateBackendGroups() {
    WasapiGroup().Visibility(settings_.backend == Backend::Wasapi ? Visibility::Visible : Visibility::Collapsed);
    AsioGroup().Visibility(settings_.backend == Backend::Asio ? Visibility::Visible : Visibility::Collapsed);
    KsGroup().Visibility(settings_.backend == Backend::Ks ? Visibility::Visible : Visibility::Collapsed);
    WasapiPeriodBox().IsEnabled(settings_.wasapi.period == WasapiPeriod::Custom);
    WasapiRateCombo().IsEnabled(!settings_.wasapi.shared); // the mixer sets the rate in shared mode
    WasapiPollBufferBox().IsEnabled(!settings_.wasapi.eventDriven);
}

void MainWindow::refreshFromServer() {
    if (!server_) return;
    applySnapshot(server_->snapshot());
    showScanProgress();
}

void MainWindow::showScanProgress() {
    LibraryScanProgress progress;
    try {
        progress = server_->libraryScanProgress();
    } catch (const std::exception&) {
        return; // no library configured
    }
    std::wstring text;
    if (progress.scans && progress.finishing == progress.scans) {
        text = tr(L"Library_Finishing");
    } else if (progress.scans) {
        text = tr(L"Library_Scanning");
        for (const auto& [marker, n] : {std::pair{L"{0}", progress.checked}, std::pair{L"{1}", progress.found}})
            if (const auto at = text.find(marker); at != std::wstring::npos) text.replace(at, 3, std::to_wstring(n));
    }
    if (text == shownScan_) return;
    shownScan_ = hstring(text);
    ScanText().Text(shownScan_);
    ScanText().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::submitCommand(Command command) {
    if (server_) cursor_.expect(server_->submit(std::move(command)));
}

void MainWindow::applySnapshot(const Snapshot& snapshot) {
    if (!server_ || !cursor_.accept(snapshot)) return;
    if (snapshot.devices.revision != shownDevicesRevision_) {
        shownDevicesRevision_ = snapshot.devices.revision;
        populateDeviceLists(snapshot.devices);
    }
    const auto& s = snapshot.playback;
    if (artworkPath_ != s.pathUtf8) {
        artworkPath_ = s.pathUtf8;
        artworkToken_.cancel();
        artworkToken_ = {};
        PlaybackCover().Source(nullptr);
        if (!artworkPath_.empty()) ui::loadArtwork(PlaybackCover(), server_, {ArtworkKind::Track, 0, artworkPath_}, artworkToken_);
    }
    const uint32_t rate = s.source ? s.source->sampleRate : 0;
    const double pos = rate ? framesToSeconds(s.position, rate) : 0.0;
    const double total = rate ? framesToSeconds(s.total, rate) : 0.0;

    PlayPauseIcon().Symbol(s.state == PlaybackState::Playing ? Symbol::Pause : Symbol::Play);
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
    RepeatIcon().Symbol(snapshot.queue.repeat == RepeatMode::One ? Symbol::RepeatOne : Symbol::RepeatAll);
    RepeatIcon().Opacity(snapshot.queue.repeat == RepeatMode::Off ? 0.35 : 1.0);
    if (camel::measure::measuring() && !queueShownMeasured_ && !snapshot.queue.entries.empty()) {
        queueShownMeasured_ = true;
        camel::log::info("measure: queue of %zu entries shown %.0f ms after process start", snapshot.queue.entries.size(),
                         camel::measure::sinceProcessStart());
    }
    if (shownQueueRevision_ != snapshot.queue.revision) {
        shownQueueRevision_ = snapshot.queue.revision;
        shownQueueEntryId_ = snapshot.queueEntryId;
        const bool windowed = snapshot.queue.entries.size() > ui::QueueCollection::kShown;
        std::wstring count(tr(windowed ? L"Queue_CountWindowed" : L"Library_Tracks"));
        if (const auto at = count.find(L"{0}"); at != std::wstring::npos)
            count.replace(at, 3, std::to_wstring(snapshot.queue.entries.size()));
        if (const auto at = count.find(L"{1}"); at != std::wstring::npos)
            count.replace(at, 3, std::to_wstring(ui::QueueCollection::kShown));
        QueueCountText().Text(count);
        QueueEmptyText().Visibility(snapshot.queue.entries.empty() ? Visibility::Visible : Visibility::Collapsed);
        std::vector<ui::QueueRow> rows;
        rows.reserve(snapshot.queue.entries.size());
        for (const auto& entry : snapshot.queue.entries)
            rows.push_back({entry.id,
                            entry.title.empty()    ? hstring(std::filesystem::path(toWide(entry.source.pathUtf8())).stem().wstring())
                            : entry.artist.empty() ? ui::utf8(entry.title)
                                                   : ui::utf8(entry.title) + L" · " + ui::utf8(entry.artist),
                            ui::utf8(entry.source.pathUtf8()), entry.automatic});
        queueItems_->update(std::move(rows), snapshot.queueEntryId);
    } else if (shownQueueEntryId_ != snapshot.queueEntryId) {
        shownQueueEntryId_ = snapshot.queueEntryId;
        queueItems_->setCurrent(snapshot.queueEntryId);
    }

    const auto currentEntry = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                           [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    if (s.pathUtf8.empty()) {
        TitleText().Text(tr(L"NoFile"));
        ArtistLinkText().Text(L"");
        AlbumLinkText().Text(L"");
        ArtistLink().Visibility(Visibility::Collapsed);
        AlbumLink().Visibility(Visibility::Collapsed);
        SourceText().Text(L"");
    } else {
        const std::filesystem::path p(toWide(s.pathUtf8));
        TitleText().Text(currentEntry != snapshot.queue.entries.end() && !currentEntry->title.empty()
                             ? ui::utf8(currentEntry->title)
                             : hstring(p.stem().wstring()));
        ArtistLinkText().Text(currentEntry == snapshot.queue.entries.end() ? hstring{} : ui::utf8(currentEntry->artist));
        ArtistLink().Visibility(currentEntry != snapshot.queue.entries.end() && !currentEntry->artist.empty()
                                    ? Visibility::Visible
                                    : Visibility::Collapsed);
        AlbumLinkText().Text(currentEntry == snapshot.queue.entries.end() ? hstring{} : ui::utf8(currentEntry->album));
        AlbumLink().Visibility(currentEntry != snapshot.queue.entries.end() && !currentEntry->album.empty()
                                   ? Visibility::Visible
                                   : Visibility::Collapsed);
        if (s.source) {
            wchar_t buf[256];
            if (s.format == AudioFormat::Dsd) // the decoder counts byte frames of eight DSD samples
                swprintf(buf, 256, L"DSD%u · %.4f MHz · %u ch · %s", s.source->sampleRate * 8 / 44100,
                         s.source->sampleRate * 8 / 1e6, s.source->channels, ui::formatTime(total).c_str());
            else
                swprintf(buf, 256, L"%s · %.1f kHz · %u ch · %s", ui::utf8(s.format ? formatName(*s.format) : "").c_str(),
                         s.source->sampleRate / 1000.0, s.source->channels, ui::formatTime(total).c_str());
            hstring text = buf;
            if (s.format != AudioFormat::Dsd && (!s.format || !isLossy(*s.format)))
                text = text + L" · " + to_hstring(s.source->bitsPerSample) + L"-bit";
            SourceText().Text(text);
        }
    }

    if (media_) {
        const bool known = currentEntry != snapshot.queue.entries.end();
        const std::string title = known && !currentEntry->title.empty()
                                      ? currentEntry->title
                                      : toUtf8(std::filesystem::path(toWide(s.pathUtf8)).stem().wstring());
        media_->update(s, title, known ? currentEntry->artist : std::string{}, known ? currentEntry->album : std::string{},
                       pos, total);
    }

    // Playing, paused, stopped or failed, never only implied by the play button's icon
    {
        const wchar_t* glyph = L"\xE71A";
        const wchar_t* key = L"State_Stopped";
        const wchar_t* color = L"TextFillColorSecondaryBrush";
        if (s.error) {
            glyph = L"\xE783";
            key = L"State_Error";
            color = L"SystemFillColorCriticalBrush";
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
        StateIcon().Foreground(themeBrush(color));
        setAnnounced(StateText(), tr(key));
        setBadgeTip(StateBadge(), tr(key), s.error ? ui::utf8(s.errorDetail) : hstring{});
    }

    // Bit-perfect indicator and signal path (docs/Plan.md §5.2, §16.2)
    const hstring decoder = ui::utf8(decoderName(s.decoder));
    if (s.output && s.source) {
        // A lossy source has no PCM depth of its own; its decoded output is passed on unchanged (docs/Plan.md §5.2).
        // Processing outranks the lossy-source badge, which must not hide it.
        const bool lossy = s.format && isLossy(*s.format);
        const OutputPlan plan = s.plan.value_or(OutputPlan{*s.output});
        switch (plan.state) {
        case PipelineState::BitPerfect:
            Lamp().Fill(themeBrush(L"SystemFillColorSuccessBrush"));
            setAnnounced(LampText(), tr(L"Lamp_BitPerfect"));
            break;
        case PipelineState::LosslessConversion:
            Lamp().Fill(themeBrush(L"SystemFillColorCautionBrush"));
            setAnnounced(LampText(), tr(L"Lamp_Lossless"));
            break;
        case PipelineState::Processed:
            Lamp().Fill(themeBrush(L"SystemFillColorCriticalBrush"));
            setAnnounced(LampText(), tr(L"Lamp_Processed"));
            break;
        case PipelineState::LossySource:
            Lamp().Fill(themeBrush(L"SystemFillColorNeutralBrush"));
            setAnnounced(LampText(), tr(L"Lamp_Lossy"));
            break;
        }
        // Source lossiness is its own field; a processed lossy file shows both (docs/Plan.md §5.2).
        SourceBadge().Visibility(lossy && plan.state != PipelineState::LossySource ? Visibility::Visible : Visibility::Collapsed);
        BypassBadge().Visibility(plan.dsd == DsdPath::Dop || plan.dsd == DsdPath::Native ? Visibility::Visible
                                                                                         : Visibility::Collapsed);
        BypassText().Text((plan.dsd == DsdPath::Dop ? hstring(L"DoP") : hstring(L"DSD")) + L" · " +
                          tr(L"SignalPath_DspBypassed"));
        setBadgeTip(BypassBadge(), BypassText().Text(), tr(L"Badge_DspBypassedTip"));
        const hstring source = ui::utf8(s.format ? formatName(*s.format) : "") +
                               (plan.dsd != DsdPath::None ? L""
                                : lossy                   ? L" → " + to_hstring(s.source->bitsPerSample) + L"-bit PCM"
                                                          : L" " + to_hstring(s.source->bitsPerSample) + L"-bit");
        FormatText().Text(source + L" → " + ui::utf8(s.outputMode) + L" · " + ui::utf8(describe(*s.output)) + L" · " + decoder);
        hstring path = tr(L"SignalPath_File") + L" → " + decoder;
        if (plan.duplicate) path = path + L" → " + tr(L"SignalPath_Duplicate");
        if (plan.downmix) path = path + L" → " + tr(L"SignalPath_Downmix") + L" " + to_hstring(s.source->channels) + L"→2";
        if (plan.resample && plan.dsd != DsdPath::Pcm) {
            wchar_t rates[64];
            swprintf(rates, 64, L" %.1f→%.1f kHz", s.source->sampleRate / 1000.0, s.output->sampleRate / 1000.0);
            path = path + L" → " + tr(L"SignalPath_Resample") + rates;
        }
        if (plan.dsd == DsdPath::Dop)
            path = path + L" → DoP · " + tr(L"SignalPath_DspBypassed");
        else if (plan.dsd == DsdPath::Native)
            path = path + L" → DSD · " + tr(L"SignalPath_DspBypassed");
        else if (plan.dsd == DsdPath::Pcm) {
            // The decimator always produces 352.8 kHz; soxr then takes it to the output rate.
            wchar_t rates[64];
            swprintf(rates, 64, L" 352.8→%.1f kHz", s.output->sampleRate / 1000.0);
            path = path + L" → " + tr(L"SignalPath_DsdToPcm") + L" 352.8 kHz";
            if (plan.resample) path = path + L" → " + tr(L"SignalPath_Resample") + rates;
            if (plan.widen)
                path = path + L" → float32";
            else
                path = path + L" → " + tr(L"SignalPath_Dither") + L" " + to_hstring(validBits(s.output->type)) + L"-bit";
        } else if (plan.widen)
            path = path + L" → float32";
        else if (plan.reduce || plan.resample || plan.downmix || lossy)
            path = path + L" → " + tr(L"SignalPath_Dither") + L" " + to_hstring(plan.reduce ? 16 : s.source->bitsPerSample) +
                   L"-bit";
        else
            path = path + L" → " + tr(L"SignalPath_Pad") + L" " + to_hstring(s.source->bitsPerSample);
        SignalPathText().Text(path + L" → " + ui::utf8(sampleTypeName(s.output->type)) + L" → " + ui::utf8(s.outputMode));
    } else {
        Lamp().Fill(themeBrush(L"ControlStrongFillColorDisabledBrush"));
        setAnnounced(LampText(), tr(L"Lamp_Idle"));
        SourceBadge().Visibility(Visibility::Collapsed);
        BypassBadge().Visibility(Visibility::Collapsed);
        FormatText().Text(ui::utf8(backendName(s.backend)) + L" · " + decoder);
        SignalPathText().Text(L"");
    }
    // Badge tooltips need a pointer; the signal path panel repeats them for keyboard and touch.
    SignalNoteText().Text(BypassBadge().Visibility() == Visibility::Visible ? tr(L"Badge_DspBypassedTip") : hstring{});

    // Errors: open the bar once per new problem so a dismissed bar stays closed
    if (s.revision != shownRevision_) {
        shownRevision_ = s.revision;
        if (s.error) {
            ErrorBar().Severity(InfoBarSeverity::Warning);
            ErrorBar().Title(ui::errorTitle(*s.error));
            ErrorBar().Message(ui::utf8(s.errorDetail));
            ErrorBar().IsOpen(true);
        } else {
            ErrorBar().IsOpen(false);
        }
    }

    if (snapshot.command.id != shownCommandId_) {
        shownCommandId_ = snapshot.command.id;
        if (snapshot.commandError) {
            ErrorBar().Severity(InfoBarSeverity::Warning);
            ErrorBar().Title(ui::errorTitle(snapshot.commandError->code));
            ErrorBar().Message(ui::utf8(snapshot.commandError->detail));
            ErrorBar().IsOpen(true);
        }
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

// Home's "View all" opens the full list in the library pages; Back returns to Home.
void MainWindow::viewAll(hstring const& list) {
    auto server = server_;
    const auto back = [this] { selectPage(L"home"); };
    const hstring unknown = tr(L"Library_Unknown"), playsFormat = tr(L"Home_Plays");
    const auto plays = [playsFormat](int64_t n) {
        std::wstring text(playsFormat);
        if (const auto at = text.find(L"{0}"); at != std::wstring::npos) text.replace(at, 3, std::to_wstring(n));
        return hstring(text);
    };
    const auto ranked = [server](int64_t offset, int64_t limit) {
        PlayStatisticsQuery query;
        query.topLimit = std::clamp<int64_t>(offset + limit, 1, 500);
        query.historyLimit = 0;
        auto stats = server->playStatistics(query);
        std::erase_if(stats.albums, [](const auto& a) { return a.plays == 0; }); // heard, but never counted as a play
        std::erase_if(stats.artists, [](const auto& a) { return a.plays == 0; });
        return stats;
    };
    Nav().SelectedItem(nullptr);
    Home().Visibility(Visibility::Collapsed);
    Library().Visibility(Visibility::Visible);
    auto view = get_self<LibraryView>(Library());
    const auto albums = [unknown](const LibraryAlbumPage& page) {
        ui::BrowsePage out{page.revision, page.total, {}};
        for (const auto& a : page.albums)
            out.rows.push_back({a.id, {}, a.title.empty() ? unknown : ui::utf8(a.title), ui::utf8(a.artist), {}, {}});
        return out;
    };
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
        view->showAlbumList(
            tr(L"Home_TopAlbums/Text"),
            [ranked, plays](int64_t offset, int64_t limit, std::optional<uint64_t>) {
                const auto stats = ranked(offset, limit);
                ui::BrowsePage out{0, int64_t(stats.albums.size()), {}};
                for (size_t i = size_t(offset); i < stats.albums.size() && i < size_t(offset + limit); ++i)
                    out.rows.push_back({stats.albums[i].album,
                                        {},
                                        ui::utf8(stats.albums[i].name),
                                        ui::utf8(stats.albums[i].artist),
                                        {},
                                        plays(stats.albums[i].plays)});
                return out;
            },
            back);
    } else if (list == L"topArtists") {
        view->showArtistList(
            tr(L"Home_TopArtists/Text"),
            [ranked, plays](int64_t offset, int64_t limit, std::optional<uint64_t>) {
                const auto stats = ranked(offset, limit);
                ui::BrowsePage out{0, int64_t(stats.artists.size()), {}};
                for (size_t i = size_t(offset); i < stats.artists.size() && i < size_t(offset + limit); ++i)
                    out.rows.push_back(
                        {0, stats.artists[i].name, ui::utf8(stats.artists[i].name), {}, {}, plays(stats.artists[i].plays)});
                return out;
            },
            back);
    }
}

void MainWindow::showPage(hstring const& tag) {
    shownTag_ = tag;
    Home().Visibility(tag == L"home" ? Visibility::Visible : Visibility::Collapsed);
    if (tag == L"home") get_self<HomeView>(Home())->refresh();
    SettingsPanel().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);
    const bool library = tag == L"albums" || tag == L"artists" || tag == L"composers" || tag == L"genres" ||
                         tag == L"years" || tag == L"tracks" || tag == L"folders";
    Library().Visibility(library ? Visibility::Visible : Visibility::Collapsed);
    auto view = get_self<LibraryView>(Library());
    if (library) view->show(tag, PlaceholderTitle().Text());
    else view->hide();
    const std::wstring_view page(tag);
    const bool playlists = page == L"playlists" || page.starts_with(L"playlist:");
    Playlists().Visibility(playlists ? Visibility::Visible : Visibility::Collapsed);
    auto playlistView = get_self<PlaylistView>(Playlists());
    if (page == L"playlists") playlistView->showAll();
    else if (playlists) playlistView->showPlaylist(std::stoll(std::wstring(page.substr(9))));
    else playlistView->hide();
    const bool placeholder = !library && !playlists && tag != L"settings" && tag != L"home";
    PlaceholderPanel().Visibility(placeholder ? Visibility::Visible : Visibility::Collapsed);
    if (tag == L"settings") {
        refreshRoots();
        showDataLocation();
    }
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
        where.Foreground(Application::Current().Resources().Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Media::Brush>());
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

// A folder, or a Windows library expanded to the folders it holds now (later library changes are not followed).
std::vector<std::wstring> pickMusicFolders(HWND owner) {
    std::vector<std::wstring> folders;
    auto dialog = winrt::create_instance<IFileOpenDialog>(CLSID_FileOpenDialog);
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_ALLNONSTORAGEITEMS);
    winrt::com_ptr<IShellItem> music;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_MusicLibrary, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(music.put()))))
        dialog->SetFolder(music.get());
    if (FAILED(dialog->Show(owner))) return folders;
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return folders;
    const auto add = [&](IShellItem* folder) {
        PWSTR path = nullptr;
        if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            folders.emplace_back(path);
            CoTaskMemFree(path);
        }
    };
    winrt::com_ptr<IShellLibrary> library;
    winrt::com_ptr<IShellItemArray> members;
    if (SUCCEEDED(SHLoadLibraryFromItem(item.get(), STGM_READ, IID_PPV_ARGS(library.put()))) &&
        SUCCEEDED(library->GetFolders(LFF_FORCEFILESYSTEM, IID_PPV_ARGS(members.put())))) {
        DWORD count = 0;
        members->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            winrt::com_ptr<IShellItem> member;
            if (SUCCEEDED(members->GetItemAt(i, member.put()))) add(member.get());
        }
    } else {
        add(item.get());
    }
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

void MainWindow::OnRemoveFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    const int index = RootList().SelectedIndex();
    if (index < 0 || size_t(index) >= roots_.size()) return;
    Command command;
    command.kind = CommandKind::RemoveLibraryRoot;
    command.rootId = roots_[size_t(index)].id;
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
        combination.Content(box_value(capturing_ == action ? tr(L"Hotkey_Press") : current.empty() ? tr(L"Hotkey_None") : hstring(current)));
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
        status.Foreground(Application::Current().Resources().Lookup(box_value(L"SystemFillColorCautionBrush")).as<Media::Brush>());
        Grid::SetColumn(status, 3);
        row.Children().Append(status);
        HotkeyRows().Children().Append(row);
    }
}

void MainWindow::OnWindowKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
    using Windows::System::VirtualKey;
    const auto key = e.Key();
    if (key != VirtualKey::Space && key != VirtualKey::Left && key != VirtualKey::Right) return;
    if (capturing_ || !server_ || !Content().XamlRoot()) return;
    if (Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(Content().XamlRoot()).Size() > 0) return;
    const auto focused = Input::FocusManager::GetFocusedElement(Content().XamlRoot());
    if (focused && (focused.try_as<TextBox>() || focused.try_as<PasswordBox>() || focused.try_as<RichEditBox>() ||
                    focused.try_as<NumberBox>() || focused.try_as<AutoSuggestBox>()))
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
        target = std::clamp<int64_t>(target, 0, s.total ? int64_t(s.total) : target);
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
                             camel::measure::milliseconds(typed, camel::measure::now()), camel::measure::milliseconds(typed, found));
    });
}

void MainWindow::showSuggestions(const LibrarySearchResults& results, hstring const& text) {
    const hstring tracksLabel = tr(L"Search_Tracks"), albumsLabel = tr(L"Search_Albums"), artistsLabel = tr(L"Search_Artists");
    suggestions_.clear();
    auto items = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
    const auto add = [&](Suggestion suggestion, hstring subtitle, hstring group) {
        auto item = make<BrowseItem>();
        get_self<BrowseItem>(item)->fill({0, {}, suggestion.title, subtitle, {}, group});
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
        case Suggestion::Kind::Album:
            selectPage(L"albums");
            view->showAlbum(suggestion.id, suggestion.title);
            return;
        case Suggestion::Kind::Artist:
            selectPage(L"albums");
            view->showArtist(suggestion.title);
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
        leave = [this, tag = shownTag_] {
            if (tag == L"settings") {
                Nav().SelectedItem(Nav().SettingsItem());
                return;
            }
            for (const auto& entry : Nav().MenuItems())
                if (const auto item = entry.try_as<NavigationViewItem>();
                    item && unbox_value_or<hstring>(item.Tag(), L"") == tag) {
                    Nav().SelectedItem(item);
                    return;
                }
        };
    }
    Nav().SelectedItem(nullptr);
    SettingsPanel().Visibility(Visibility::Collapsed);
    PlaceholderPanel().Visibility(Visibility::Collapsed);
    Home().Visibility(Visibility::Collapsed);
    Library().Visibility(Visibility::Visible);
    get_self<LibraryView>(Library())->showSearch(query, std::move(leave));
}

void MainWindow::OnSearchAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& e) {
    if (Nav().IsPaneOpen() == false) Nav().IsPaneOpen(true); // the box is hidden behind its icon in the compact pane
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
    std::error_code error;
    const auto size = [&](const std::filesystem::path& path) -> uintmax_t {
        if (std::filesystem::is_regular_file(path, error)) return std::filesystem::file_size(path, error);
        uintmax_t total = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, error))
            if (entry.is_regular_file(error)) total += entry.file_size(error);
        return total;
    };
    const uintmax_t library = size(data / L"library.db") + size(data / L"library.db-wal") + size(data / L"library.db-shm");
    const uintmax_t covers = size(data / L"thumbs"), settings = size(camel::settingsPath()), logs = size(data / L"logs");
    queue.TryEnqueue([weak, library, covers, settings, logs] {
        auto self = weak.get();
        if (!self) return;
        const auto bytes = [](uintmax_t n) {
            wchar_t text[32];
            if (n >= (1ull << 30)) swprintf(text, 32, L"%.1f GB", n / double(1ull << 30));
            else if (n >= (1ull << 20)) swprintf(text, 32, L"%.1f MB", n / double(1ull << 20));
            else swprintf(text, 32, L"%.1f KB", n / 1024.0);
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

// Settings › Reset (docs/Plan.md §8.2): every reset says what it changes and asks first; resetting everything also
// needs a checked acknowledgement because play statistics and playlists go with it.
winrt::fire_and_forget MainWindow::OnResetClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
    auto strong = get_strong();
    const hstring kind = unbox_value_or<hstring>(sender.as<FrameworkElement>().Tag(), L"");
    const std::wstring key = kind == L"playback"  ? L"Playback"
                             : kind == L"interface" ? L"Interface"
                             : kind == L"library"   ? L"Library"
                             : kind == L"covers"    ? L"Covers"
                                                    : L"All";
    ContentDialog dialog;
    dialog.XamlRoot(ContentLayout().XamlRoot());
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
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;

    if (kind == L"playback") {
        settings_ = settingsDto(camel::resetPlayback(nativeSettings(settings_)));
        applySettingsNow();
    } else if (kind == L"interface") {
        settings_ = settingsDto(camel::resetInterface(nativeSettings(settings_)));
        queueOpen_ = false;
        updateQueuePanel();
        applySettingsNow();
        LanguageNote().Visibility(Visibility::Visible); // the language follows Windows again after a restart
    } else if (kind == L"library" || kind == L"covers") {
        auto server = server_;
        auto queue = DispatcherQueue();
        const bool library = kind == L"library";
        co_await winrt::resume_background();
        try {
            if (library) server->rereadLibrary();
            else server->clearArtworkCache();
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
    for (const auto* name : {L"WASAPI", L"ASIO", L"Kernel Streaming"}) backend.Items().Append(box_value(hstring(name)));
    ComboBox device;
    device.Header(box_value(tr(L"Settings_Device/Header")));
    device.Width(420);
    auto devices = std::make_shared<std::vector<std::string>>(); // ids in the order shown
    const auto fillDevices = [this, device, devices](Backend chosen) {
        const auto catalog = server_->snapshot().devices;
        const auto& list = chosen == Backend::Wasapi ? catalog.wasapi : chosen == Backend::Asio ? catalog.asio : catalog.ks;
        const std::string& saved = chosen == Backend::Wasapi ? settings_.wasapi.deviceId
                                   : chosen == Backend::Asio ? settings_.asio.driverClsid
                                                             : settings_.ks.deviceId;
        device.Items().Clear();
        devices->clear();
        if (chosen == Backend::Wasapi) {
            device.Items().Append(box_value(tr(L"Device_Default")));
            devices->push_back({});
        }
        for (const auto& entry : list) {
            device.Items().Append(box_value(ui::utf8(entry.name)));
            devices->push_back(entry.id);
        }
        const auto selected = std::find(devices->begin(), devices->end(), saved);
        device.SelectedIndex(devices->empty() ? -1 : selected == devices->end() ? 0 : int32_t(selected - devices->begin()));
    };
    backend.SelectionChanged(
        [fillDevices, backend](auto&&, auto&&) { fillDevices(Backend(std::max(0, backend.SelectedIndex()))); });
    backend.SelectedIndex(int32_t(settings_.backend));
    output.Children().Append(backend);
    output.Children().Append(device);

    StackPanel done;
    done.Children().Append(text(L"Guide_Done"));

    const std::array<std::pair<const wchar_t*, UIElement>, 4> steps{{{L"Guide_WelcomeTitle", welcome},
                                                                     {L"Guide_FoldersTitle", folders},
                                                                     {L"Guide_OutputTitle", output},
                                                                     {L"Guide_DoneTitle", done}}};
    ContentDialog dialog;
    dialog.XamlRoot(ContentLayout().XamlRoot());
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
    const auto result = co_await dialog.ShowAsync();
    guideFolders_ = nullptr;

    // Finishing applies the chosen output; skipping keeps the defaults. Either way the guide is done.
    if (result == ContentDialogResult::Primary) {
        settings_.backend = Backend(std::max(0, backend.SelectedIndex()));
        const auto index = device.SelectedIndex();
        if (index >= 0 && size_t(index) < devices->size()) {
            const std::string& id = (*devices)[size_t(index)];
            if (settings_.backend == Backend::Wasapi)
                settings_.wasapi.deviceId = id;
            else if (settings_.backend == Backend::Asio)
                settings_.asio.driverClsid = id;
            else
                settings_.ks.deviceId = id;
        }
    }
    settings_.firstRunDone = true;
    Command command;
    command.kind = CommandKind::ApplySettings;
    command.settings = settings_;
    submitCommand(std::move(command));
    populateSettings();
}

void MainWindow::OnFormatClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    const bool open = SignalPathPanel().Visibility() == Visibility::Collapsed;
    SignalPathPanel().Visibility(open ? Visibility::Visible : Visibility::Collapsed);
    FormatChevron().Glyph(open ? L"\xE70E" : L"\xE70D");
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
        if (change) self->showPlaylistChange(*change, true);
        else self->showPlaylistNotice(tr(L"Queue_SaveFailed"), {}, true);
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
                    selected = selected || (clicked >= uint32_t(range.FirstIndex()) && clicked <= uint32_t(range.LastIndex()));
            if (!selected) {
                add(queueMenuItem_);
            } else {
                for (const auto& range : QueueList().SelectedRanges())
                    for (auto i = range.FirstIndex(); i <= range.LastIndex(); ++i) add(queueItems_->GetAt(uint32_t(i)));
            }
            return [sources] { return sources; };
        });
        queueMenu_.Items().RemoveAt(0); // the separator before the playlist entries
    }
    Primitives::FlyoutShowOptions options;
    if (Windows::Foundation::Point point; e.TryGetPosition(element, point)) options.Position(point);
    queueMenu_.ShowAt(element, options);
    e.Handled(true);
}

void MainWindow::extendPlaylistMenu(MenuFlyout const& menu, std::function<ui::SourceFetch()> selection) {
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
        try {
            sources = fetch();
            preview = sources.empty() ? SelectionPreview{} : server->previewSelection(sources);
        } catch (const std::exception& e) {
            camel::log::warn("playlist selection: %s", e.what());
        }
        dispatcher.TryEnqueue([weak, sources = std::move(sources), preview, playlist, name] {
            auto self = weak.get();
            if (!self) return;
            if (!preview) self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
            else if (preview->tracks == 0) self->showPlaylistNotice(tr(L"Playlist_NothingAdded"), tr(L"Playlist_NothingAddedText"), false);
            else self->applyPlaylistAction(sources, *preview, playlist, name);
        });
    }).detach();
}

winrt::fire_and_forget MainWindow::applyPlaylistAction(std::vector<PlaylistSource> sources, SelectionPreview preview,
                                                       std::optional<int64_t> playlist, std::string name) {
    auto strong = get_strong();
    if (preview.tracks > 1000) {
        ContentDialog dialog;
        dialog.XamlRoot(ContentLayout().XamlRoot());
        dialog.Title(box_value(tr(L"Playlist_ConfirmTitle")));
        std::wstring count = std::to_wstring(preview.tracks);
        for (auto at = int64_t(count.size()) - 3; at > 0; at -= 3) count.insert(size_t(at), L",");
        const int64_t minutes = preview.durationMs / 60'000;
        const std::wstring length =
            minutes < 24 * 60 ? ui::formatTime(double(preview.durationMs) / 1000.0)
                              : std::wstring(ui::format(L"Home_DaysHours", {std::to_wstring(minutes / (24 * 60)),
                                                                            std::to_wstring(minutes / 60 % 24)}));
        dialog.Content(box_value(ui::format(L"Playlist_ConfirmText", {count, length})));
        dialog.PrimaryButtonText(tr(playlist ? L"Playlist_ConfirmAdd" : L"Playlist_ConfirmCreate"));
        dialog.CloseButtonText(tr(L"Playlist_Cancel"));
        dialog.DefaultButton(ContentDialogButton::Primary);
        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
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
        if (change) self->showPlaylistChange(*change, created);
        else self->showPlaylistNotice(tr(L"Playlist_Failed"), {}, true);
    });
}

winrt::fire_and_forget MainWindow::askNewPlaylist(ui::SourceFetch fetch) {
    auto strong = get_strong();
    ContentDialog dialog;
    dialog.XamlRoot(ContentLayout().XamlRoot());
    dialog.Title(box_value(tr(L"Playlist_NewTitle")));
    TextBox name;
    name.PlaceholderText(tr(L"Playlist_NamePlaceholder"));
    dialog.Content(name);
    dialog.PrimaryButtonText(tr(L"Playlist_ConfirmCreate"));
    dialog.CloseButtonText(tr(L"Playlist_Cancel"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    if (co_await dialog.ShowAsync() != ContentDialogResult::Primary) co_return;
    const auto text = name.Text();
    runPlaylistAction(std::move(fetch), std::nullopt, toUtf8(std::wstring_view(text.c_str(), text.size())));
}

void MainWindow::showPlaylistChange(const PlaylistChange& change, bool created) {
    std::wstring message(ui::format(L"Library_Tracks", {std::to_wstring(change.added)}));
    if (const auto skipped = change.skippedUnavailable + change.skippedDuplicates + change.skippedExisting)
        message += L" · " + std::wstring(ui::format(L"Playlist_Skipped", {std::to_wstring(skipped)}));
    const std::wstring name(ui::utf8(change.name));
    if (!created && change.added == 0)
        showPlaylistNotice(ui::format(L"Playlist_NothingAddedTo", {name}), hstring(message), false, std::nullopt, change.playlist);
    else
        showPlaylistNotice(ui::format(created ? L"Playlist_Created" : L"Playlist_Added", {name}), hstring(message), false,
                           change.undo, change.playlist);
    get_self<PlaylistView>(Playlists())->reload(change.playlist);
    if (created) refreshPlaylists([this, id = change.playlist] { renameInNavigation(id); });
    else refreshPlaylists();
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
    if (actions.Children().Size()) bar.Content(actions);
    else bar.Content(nullptr);
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
            if (undone && undo.created && view->shown() == undo.playlist) self->selectPage(L"playlists");
            else view->reload(undo.playlist);
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
    if (item.IsLoaded()) show();
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
    if (!server_ || server_->snapshot().playback.pathUtf8.empty()) return;
    selectPage(L"albums");
    const auto snapshot = server_->snapshot();
    const auto current = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                      [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    if (current != snapshot.queue.entries.end() && current->albumId > 0) {
        Library().Visibility(Visibility::Visible);
        get_self<LibraryView>(Library())->showAlbum(current->albumId, ui::utf8(current->album));
    } else {
        showPage(L"albums");
    }
}

void MainWindow::OnAlbumLinkClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    const auto snapshot = server_->snapshot();
    const auto current = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                      [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    if (current == snapshot.queue.entries.end() || current->albumId <= 0) return;
    selectPage(L"albums");
    Library().Visibility(Visibility::Visible);
    get_self<LibraryView>(Library())->showAlbum(current->albumId, ui::utf8(current->album));
}

void MainWindow::OnArtistLinkClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!server_) return;
    const auto snapshot = server_->snapshot();
    const auto current = std::find_if(snapshot.queue.entries.begin(), snapshot.queue.entries.end(),
                                      [&](const auto& entry) { return entry.id == snapshot.queueEntryId; });
    if (current == snapshot.queue.entries.end() || current->artist.empty()) return;
    Nav().SelectedItem(Nav().MenuItems().GetAt(1));
    Library().Visibility(Visibility::Visible);
    get_self<LibraryView>(Library())->showArtist(ui::utf8(current->artist));
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
        showPage(L"settings");
        return;
    }
    auto item = e.SelectedItemContainer();
    if (!item) return;
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
    LanguageNote().Visibility(Visibility::Visible); // takes effect after restart (docs/Plan.md §8.5)
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

} // namespace winrt::CamelPlayer::implementation
