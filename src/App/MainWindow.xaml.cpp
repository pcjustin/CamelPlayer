#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <cmath>
#include <filesystem>

#include "UiText.h"
#include "util/Log.h"
#include "util/Text.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;

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

Media::SolidColorBrush brush(uint8_t r, uint8_t g, uint8_t b) { return Media::SolidColorBrush(Windows::UI::Color{255, r, g, b}); }

}  // namespace

void MainWindow::InitializeComponent() {
    MainWindowT<MainWindow>::InitializeComponent();
    AppWindow().Resize({1180, 780});

    settings_ = loadSettings();
    engine_ = std::make_unique<PlaybackEngine>(settings_);
    auto dq = DispatcherQueue();
    engine_->setListener([dq, weak = get_weak()] {
        dq.TryEnqueue([weak] {
            if (auto self = weak.get()) self->refreshFromEngine();
        });
    });
    using namespace std::chrono_literals;
    uiTimer_ = dq.CreateTimer();
    uiTimer_.Interval(250ms);
    uiTimer_.Tick([this](auto&&, auto&&) { refreshFromEngine(); });
    uiTimer_.Start();
    seekTimer_ = dq.CreateTimer();
    seekTimer_.Interval(200ms);
    seekTimer_.IsRepeating(false);
    seekTimer_.Tick([this](auto&&, auto&&) {
        auto s = engine_->snapshot();
        if (s.source) engine_->seek(secondsToFrames(pendingSeek_, s.source->sampleRate));
    });

    for (auto [button, key] : {std::pair{OpenButton(), L"Tip_Open"}, std::pair{PlayPauseButton(), L"Tip_PlayPause"}, std::pair{StopButton(), L"Tip_Stop"}}) {
        ToolTipService::SetToolTip(button, box_value(tr(key)));
        Automation::AutomationProperties::SetName(button, tr(key));  // icon-only buttons need a name for screen readers
    }

    populateSettings();
    Nav().SelectedItem(Nav().MenuItems().GetAt(0));
    std::error_code ec;
    if (!settings_.lastFile.empty() && std::filesystem::exists(settings_.lastFile, ec)) engine_->load(settings_.lastFile);  // no autoplay (§2)

    Closed([this](auto&&, auto&&) {
        uiTimer_.Stop();
        seekTimer_.Stop();
        engine_.reset();  // closes the device before the window goes away
    });
    refreshFromEngine();
}


void MainWindow::populateSettings() {
    populating_ = true;
    fill(BackendCombo(), {L"WASAPI", L"ASIO", L"Kernel Streaming"}, int(settings_.backend));
    std::vector<hstring> decoders;
    for (DecoderKind k : kAllDecoders) decoders.push_back(ui::utf8(decoderName(k)));
    fill(DecoderCombo(), decoders, int(settings_.decoder));

    fill(WasapiModeCombo(), {tr(L"Mode_Event"), tr(L"Mode_Polling")}, settings_.wasapi.eventDriven ? 0 : 1);
    fill(WasapiPeriodCombo(), {tr(L"Period_Default"), tr(L"Period_Minimum"), tr(L"Period_Custom")}, int(settings_.wasapi.period));
    WasapiPeriodBox().Value(settings_.wasapi.customPeriodMs);
    const std::vector<hstring> formats = {tr(L"Format_Auto"), L"16", L"24", L"24-in-32", L"32"};
    fill(WasapiFormatCombo(), formats, formatIndex(settings_.wasapi.sampleType));

    AsioBufferBox().Value(double(settings_.asio.bufferSize));
    AsioLeftBox().Value(settings_.asio.outLeft + 1);
    AsioRightBox().Value(settings_.asio.outRight + 1);

    fill(KsModeCombo(), {tr(L"KsMode_Auto"), tr(L"KsMode_RtEvent"), tr(L"KsMode_RtPoll"), tr(L"KsMode_Standard")}, int(settings_.ks.mode));
    KsBufferBox().Value(settings_.ks.bufferMs);
    KsPacketCountBox().Value(settings_.ks.packetCount);
    KsPacketMsBox().Value(settings_.ks.packetMs);
    fill(KsFormatCombo(), formats, formatIndex(settings_.ks.sampleType));

    const int lang = settings_.language == "zh-TW" ? 1 : settings_.language == "en-US" ? 2 : 0;
    fill(LanguageCombo(), {tr(L"Lang_System"), L"繁體中文", L"English"}, lang);
    refreshDeviceLists();
    populating_ = false;
    updateBackendGroups();
}

void MainWindow::refreshDeviceLists() {
    const bool was = populating_;
    populating_ = true;
    wasapiDevices_ = listWasapiDevices();
    std::vector<hstring> names{tr(L"Device_Default")};
    int sel = 0;
    for (size_t i = 0; i < wasapiDevices_.size(); i++) {
        names.push_back(ui::utf8(wasapiDevices_[i].name));
        if (wasapiDevices_[i].id == settings_.wasapi.deviceId) sel = int(i + 1);
    }
    fill(WasapiDeviceCombo(), names, sel);

    auto fillList = [&](ComboBox const& combo, const std::vector<DeviceEntry>& list, const std::string& current) {
        std::vector<hstring> n;
        int s = 0;
        for (size_t i = 0; i < list.size(); i++) {
            n.push_back(ui::utf8(list[i].name));
            if (list[i].id == current) s = int(i);
        }
        if (n.empty()) n.push_back(tr(L"Device_None"));
        fill(combo, n, s);
    };
    asioDrivers_ = listAsioDrivers();
    fillList(AsioDriverCombo(), asioDrivers_, settings_.asio.driverClsid);
    ksPins_ = listKsPins();
    fillList(KsDeviceCombo(), ksPins_, settings_.ks.deviceId);
    populating_ = was;
}

void MainWindow::readSettingsFromUi() {
    auto idx = [](ComboBox const& c) { return std::max(0, c.SelectedIndex()); };
    settings_.backend = Backend(std::min(idx(BackendCombo()), 2));
    settings_.decoder = kAllDecoders[std::min<size_t>(idx(DecoderCombo()), std::size(kAllDecoders) - 1)];

    const int dev = idx(WasapiDeviceCombo());
    settings_.wasapi.deviceId = dev == 0 || dev > int(wasapiDevices_.size()) ? "" : wasapiDevices_[size_t(dev - 1)].id;
    settings_.wasapi.eventDriven = idx(WasapiModeCombo()) == 0;
    settings_.wasapi.period = WasapiPeriod(std::min(idx(WasapiPeriodCombo()), 2));
    settings_.wasapi.customPeriodMs = numberOr(WasapiPeriodBox(), settings_.wasapi.customPeriodMs);
    settings_.wasapi.sampleType = kFormatChoices[std::min(idx(WasapiFormatCombo()), 4)];

    const int drv = idx(AsioDriverCombo());
    if (drv < int(asioDrivers_.size())) settings_.asio.driverClsid = asioDrivers_[size_t(drv)].id;
    settings_.asio.bufferSize = long(numberOr(AsioBufferBox(), double(settings_.asio.bufferSize)));
    settings_.asio.outLeft = int(numberOr(AsioLeftBox(), settings_.asio.outLeft + 1)) - 1;
    settings_.asio.outRight = int(numberOr(AsioRightBox(), settings_.asio.outRight + 1)) - 1;

    const int pin = idx(KsDeviceCombo());
    if (pin < int(ksPins_.size())) settings_.ks.deviceId = ksPins_[size_t(pin)].id;
    settings_.ks.mode = KsMode(std::min(idx(KsModeCombo()), 3));
    settings_.ks.bufferMs = numberOr(KsBufferBox(), settings_.ks.bufferMs);
    settings_.ks.packetCount = int(numberOr(KsPacketCountBox(), settings_.ks.packetCount));
    settings_.ks.packetMs = numberOr(KsPacketMsBox(), settings_.ks.packetMs);
    settings_.ks.sampleType = kFormatChoices[std::min(idx(KsFormatCombo()), 4)];
}

void MainWindow::settingsChanged() {
    if (populating_ || !engine_) return;
    readSettingsFromUi();
    try {
        saveSettings(settings_);
    } catch (const std::exception& e) {
        log::error("saving settings failed: %s", e.what());
    }
    engine_->applySettings(settings_);  // immediately, from the current position (docs/計劃書.md §2)
    updateBackendGroups();
}

void MainWindow::updateBackendGroups() {
    WasapiGroup().Visibility(settings_.backend == Backend::Wasapi ? Visibility::Visible : Visibility::Collapsed);
    AsioGroup().Visibility(settings_.backend == Backend::Asio ? Visibility::Visible : Visibility::Collapsed);
    KsGroup().Visibility(settings_.backend == Backend::Ks ? Visibility::Visible : Visibility::Collapsed);
    WasapiPeriodBox().IsEnabled(settings_.wasapi.period == WasapiPeriod::Custom);
}

void MainWindow::refreshFromEngine() {
    if (!engine_) return;
    const EngineSnapshot s = engine_->snapshot();
    const uint32_t rate = s.source ? s.source->sampleRate : 0;
    const double pos = rate ? framesToSeconds(s.position, rate) : 0.0;
    const double total = rate ? framesToSeconds(s.total, rate) : 0.0;

    PlayPauseIcon().Symbol(s.state == PlayState::Playing ? Symbol::Pause : Symbol::Play);
    if (!seekTimer_.IsRunning()) {  // do not fight the user while they drag
        sliderUpdating_ = true;
        PositionSlider().Maximum(std::max(1.0, total));
        PositionSlider().Value(std::min(pos, std::max(1.0, total)));
        sliderUpdating_ = false;
    }
    TimeText().Text(ui::formatTime(pos) + L" / " + ui::formatTime(total));

    // Now playing
    if (s.file.empty()) {
        TitleText().Text(tr(L"NoFile"));
        PathText().Text(L"");
        SourceText().Text(L"");
    } else {
        const std::filesystem::path p(s.file);
        TitleText().Text(p.stem().wstring());
        PathText().Text(s.file);
        if (s.source) {
            wchar_t buf[256];
            swprintf(buf, 256, L"FLAC · %u-bit · %.1f kHz · %u ch · %s · STREAMINFO MD5 %s", s.source->bitsPerSample,
                     s.source->sampleRate / 1000.0, s.source->channels, ui::formatTime(total).c_str(), s.source->md5 ? L"✓" : L"—");
            SourceText().Text(buf);
        }
    }

    // Bit-perfect indicator and signal path (docs/計劃書.md §5.2, §16.2)
    const hstring decoder = ui::utf8(decoderName(s.decoder));
    if (s.output && s.source) {
        Lamp().Fill(brush(0x10, 0xB9, 0x10));
        LampText().Text(L"Bit-perfect");
        FormatText().Text(hstring(L"FLAC ") + to_hstring(s.source->bitsPerSample) + L"-bit → " + ui::utf8(s.outputStatus.mode) + L" · " +
                          ui::utf8(describe(*s.output)) + L" · " + decoder);
        SignalPathText().Text(tr(L"SignalPath_File") + L" → " + decoder + L" → " + tr(L"SignalPath_Pad") + L" " +
                              to_hstring(s.source->bitsPerSample) + L" → " + ui::utf8(sampleTypeName(s.output->type)) + L" → " +
                              ui::utf8(s.outputStatus.mode));
    } else {
        Lamp().Fill(brush(0x80, 0x80, 0x80));
        LampText().Text(tr(L"Lamp_Idle"));
        FormatText().Text(ui::utf8(backendName(s.backend)) + L" · " + decoder);
        SignalPathText().Text(L"");
    }

    // Errors: open the bar once per new problem so a dismissed bar stays closed
    if (s.revision != shownRevision_) {
        shownRevision_ = s.revision;
        if (s.error) {
            ErrorBar().Title(ui::errorTitle(*s.error));
            ErrorBar().Message(ui::utf8(s.errorDetail));
            ErrorBar().IsOpen(true);
        } else {
            ErrorBar().IsOpen(false);
        }
    }

    // Status area on the settings page
    if (s.output) {
        wchar_t buf[512];
        swprintf(buf, 512, L"%s\n%s\n%s\nunderrun: %llu", ui::utf8(describe(*s.output)).c_str(), ui::utf8(s.outputStatus.mode).c_str(),
                 ui::utf8(s.outputStatus.detail).c_str(), static_cast<unsigned long long>(s.underruns));
        StatusText().Text(buf);
    } else {
        StatusText().Text(tr(L"Status_Idle"));
    }
}

void MainWindow::showPage(hstring const& tag) {
    NowPlayingPanel().Visibility(tag == L"nowplaying" ? Visibility::Visible : Visibility::Collapsed);
    SettingsPanel().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);
    const bool placeholder = tag != L"nowplaying" && tag != L"settings";
    PlaceholderPanel().Visibility(placeholder ? Visibility::Visible : Visibility::Collapsed);
}

winrt::fire_and_forget MainWindow::OnOpenClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    Microsoft::Windows::Storage::Pickers::FileOpenPicker picker(AppWindow().Id());
    picker.FileTypeFilter().Append(L".flac");
    picker.SuggestedStartLocation(Microsoft::Windows::Storage::Pickers::PickerLocationId::MusicLibrary);
    auto result = co_await picker.PickSingleFileAsync();
    if (!result) co_return;
    const std::wstring path{result.Path()};
    settings_.lastFile = path;
    try {
        saveSettings(settings_);
    } catch (...) {
    }
    engine_->openAndPlay(path);
}

void MainWindow::OnPlayPauseClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const& args) {
    if (engine_->snapshot().file.empty()) {
        OnOpenClick(sender, args);
        return;
    }
    engine_->togglePause();
}

void MainWindow::OnStopClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { engine_->stop(); }

void MainWindow::OnSliderValueChanged(Windows::Foundation::IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (sliderUpdating_ || !engine_) return;
    pendingSeek_ = e.NewValue();
    seekTimer_.Stop();
    seekTimer_.Start();  // seek once the user pauses dragging
}

void MainWindow::OnNavSelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& e) {
    if (e.IsSettingsSelected()) {
        showPage(L"settings");
        return;
    }
    auto item = e.SelectedItemContainer();
    if (!item) return;
    const hstring tag = unbox_value_or<hstring>(item.Tag(), L"nowplaying");
    if (tag != L"nowplaying") PlaceholderTitle().Text(unbox_value_or<hstring>(item.Content(), L""));
    showPage(tag);
}

void MainWindow::OnSettingChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) { settingsChanged(); }

void MainWindow::OnNumberChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) { settingsChanged(); }

void MainWindow::OnLanguageChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    if (populating_) return;
    const int i = LanguageCombo().SelectedIndex();
    settings_.language = i == 1 ? "zh-TW" : i == 2 ? "en-US" : "";
    try {
        saveSettings(settings_);
    } catch (...) {
    }
    LanguageNote().Visibility(Visibility::Visible);  // takes effect after restart (docs/計劃書.md §8.5)
}

winrt::fire_and_forget MainWindow::OnAsioPanelClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    auto strong = get_strong();
    auto* engine = engine_.get();
    co_await winrt::resume_background();  // some drivers show a modal dialog
    engine->showControlPanel();
}

void MainWindow::OnRefreshDevicesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { refreshDeviceLists(); }

}  // namespace winrt::CamelPlayer::implementation
