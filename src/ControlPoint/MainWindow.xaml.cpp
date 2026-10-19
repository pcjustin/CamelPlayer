#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <cmath>
#include <filesystem>

#include "UiText.h"
#include "Common/util/Text.h"

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

Media::SolidColorBrush brush(uint8_t r, uint8_t g, uint8_t b) {
    return Media::SolidColorBrush(Windows::UI::Color{255, r, g, b});
}

} // namespace

void MainWindow::InitializeComponent() {
    MainWindowT<MainWindow>::InitializeComponent();
    AppWindow().Resize({1180, 780});
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

    for (auto [button, key] : {std::pair{OpenButton(), L"Tip_Open"}, std::pair{PlayPauseButton(), L"Tip_PlayPause"},
                               std::pair{StopButton(), L"Tip_Stop"}}) {
        ToolTipService::SetToolTip(button, box_value(tr(key)));
        Automation::AutomationProperties::SetName(button, tr(key)); // icon-only buttons need a name for screen readers
    }

    populateSettings();
    Nav().SelectedItem(Nav().MenuItems().GetAt(0));
    Command refresh;
    refresh.kind = CommandKind::RefreshDevices;
    submitCommand(std::move(refresh));

    Closed([this](auto&&, auto&&) {
        uiTimer_.Stop();
        seekTimer_.Stop();
        server_->setListener({});
        submitCommand(Command{});
        server_.reset();
    });
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

    fill(WasapiShareCombo(), {tr(L"Share_Exclusive"), tr(L"Share_Shared")}, settings_.wasapi.shared ? 1 : 0);
    fill(WasapiSharedQualityCombo(), {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")},
         int(settings_.wasapi.sharedResampleQuality));
    fill(WasapiModeCombo(), {tr(L"Mode_Event"), tr(L"Mode_Polling")}, settings_.wasapi.eventDriven ? 0 : 1);
    fill(WasapiPeriodCombo(), {tr(L"Period_Default"), tr(L"Period_Minimum"), tr(L"Period_Custom")}, int(settings_.wasapi.period));
    WasapiPeriodBox().Value(settings_.wasapi.customPeriodMs);
    const std::vector<hstring> formats = {tr(L"Format_Auto"), L"16", L"24", L"24-in-32", L"32"};
    fill(WasapiFormatCombo(), formats, formatIndex(settings_.wasapi.sampleType));

    AsioBufferBox().Value(double(settings_.asio.bufferSize));
    AsioLeftBox().Value(settings_.asio.outLeft + 1);
    AsioRightBox().Value(settings_.asio.outRight + 1);

    fill(KsModeCombo(), {tr(L"KsMode_Auto"), tr(L"KsMode_RtEvent"), tr(L"KsMode_RtPoll"), tr(L"KsMode_Standard")},
         int(settings_.ks.mode));
    KsBufferBox().Value(settings_.ks.bufferMs);
    KsPacketCountBox().Value(settings_.ks.packetCount);
    KsPacketMsBox().Value(settings_.ks.packetMs);
    fill(KsFormatCombo(), formats, formatIndex(settings_.ks.sampleType));

    const int lang = settings_.language == "zh-TW" ? 1 : settings_.language == "en-US" ? 2 : 0;
    fill(LanguageCombo(), {tr(L"Lang_System"), L"繁體中文", L"English"}, lang);
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

    const int dev = idx(WasapiDeviceCombo());
    if (shownDevicesRevision_ != 0)
        settings_.wasapi.deviceId = dev == 0 || dev > int(wasapiDevices_.size()) ? "" : wasapiDevices_[size_t(dev - 1)].id;
    settings_.wasapi.shared = idx(WasapiShareCombo()) == 1;
    settings_.wasapi.sharedResampleQuality = ResampleQuality(std::min(idx(WasapiSharedQualityCombo()), 2));
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
    if (populating_ || !server_) return;
    readSettingsFromUi();
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
    WasapiPeriodBox().IsEnabled(settings_.wasapi.period == WasapiPeriod::Custom);
}

void MainWindow::refreshFromServer() {
    if (server_) applySnapshot(server_->snapshot());
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

    // Now playing
    if (s.pathUtf8.empty()) {
        TitleText().Text(tr(L"NoFile"));
        PathText().Text(L"");
        SourceText().Text(L"");
    } else {
        const std::filesystem::path p(toWide(s.pathUtf8));
        TitleText().Text(p.stem().wstring());
        PathText().Text(ui::utf8(s.pathUtf8));
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
            if (s.format == AudioFormat::Flac) text = text + L" · STREAMINFO MD5 " + (s.source->md5 ? L"✓" : L"—");
            SourceText().Text(text);
        }
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
            Lamp().Fill(brush(0x10, 0xB9, 0x10));
            LampText().Text(L"Bit-perfect");
            break;
        case PipelineState::LosslessConversion:
            Lamp().Fill(brush(0xE0, 0xB0, 0x00));
            LampText().Text(tr(L"Lamp_Lossless"));
            break;
        case PipelineState::Processed:
            Lamp().Fill(brush(0xD0, 0x30, 0x30));
            LampText().Text(lossy ? tr(L"Lamp_Processed") + L" · " + tr(L"Lamp_Lossy") : tr(L"Lamp_Processed"));
            break;
        case PipelineState::LossySource:
            Lamp().Fill(brush(0xD0, 0xD0, 0xD0));
            LampText().Text(tr(L"Lamp_Lossy"));
            break;
        }
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

    if (snapshot.command.id != shownCommandId_) {
        shownCommandId_ = snapshot.command.id;
        if (snapshot.commandError) {
            ErrorBar().Title(ui::errorTitle(snapshot.commandError->code));
            ErrorBar().Message(ui::utf8(snapshot.commandError->detail));
            ErrorBar().IsOpen(true);
        }
    }

    // Status area on the settings page
    if (s.output) {
        wchar_t buf[512];
        swprintf(buf, 512, L"%s\n%s\n%s\nunderrun: %llu", ui::utf8(describe(*s.output)).c_str(), ui::utf8(s.outputMode).c_str(),
                 ui::utf8(s.outputDetail).c_str(), static_cast<unsigned long long>(s.underruns));
        hstring status = buf;
        for (const auto& adjustment : s.outputAdjustments)
            status = status + L"\n" + tr(L"Status_Adjusted") + L" " + ui::utf8(adjustment);
        StatusText().Text(status);
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
    // The decoder is chosen from the file's contents; the extension only filters the list.
    for (const wchar_t* extension : {L".flac", L".m4a", L".alac", L".wav", L".aif", L".aiff", L".aifc", L".aac", L".mp3"})
        picker.FileTypeFilter().Append(extension);
    picker.SuggestedStartLocation(Microsoft::Windows::Storage::Pickers::PickerLocationId::MusicLibrary);
    auto result = co_await picker.PickSingleFileAsync();
    if (!result || !server_) co_return;
    Command command;
    command.kind = CommandKind::OpenAndPlay;
    command.source.emplace(toUtf8(std::wstring_view(result.Path())));
    submitCommand(std::move(command));
}

void MainWindow::OnPlayPauseClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const& args) {
    if (!server_) return;
    if (server_->snapshot().playback.pathUtf8.empty()) {
        OnOpenClick(sender, args);
        return;
    }
    Command command;
    command.kind = CommandKind::TogglePause;
    submitCommand(std::move(command));
}

void MainWindow::OnStopClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    submitCommand(Command{});
}

void MainWindow::OnSliderValueChanged(Windows::Foundation::IInspectable const&,
                                      Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (sliderUpdating_ || !server_) return;
    pendingSeek_ = e.NewValue();
    seekTimer_.Stop();
    seekTimer_.Start(); // seek once the user pauses dragging
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
