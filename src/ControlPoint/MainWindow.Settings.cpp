// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "MainWindowParts.h"

#include <cmath>
#include <span>
#include <utility>

#include "UiText.h"
#include "TrackColumns.h"
#include "Common/settings/Settings.h"
#include "Server/Playback/Outputs.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace camel;
using namespace camel::audio;
using ui::tr;
using namespace camel::server;

namespace winrt::CamelPlayer::implementation {

using namespace mainwindow;

namespace {

constexpr std::optional<SampleType> kFormatChoices[] = {std::nullopt, SampleType::Int16, SampleType::Int24, SampleType::Int24In32,
                                                        SampleType::Int32};

int formatIndex(std::optional<SampleType> t) {
    for (int i = 0; i < 5; i++)
        if (kFormatChoices[i] == t) return i;
    return 0;
}

double numberOr(NumberBox const& box, double fallback) {
    double v = box.Value();
    return std::isnan(v) ? fallback : v;
}

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

// An output's sample-rate choice, conversion filter and DSD upsampling (docs/Plan.md §6), as WASAPI exclusive, ASIO, KS
// and network players have them. The choice lists follow the file, the three upsampling modes, then the set rates; DSD
// upsampling lists off, its two modes, then the set DSD rates.
struct RateControls {
    ComboBox rate, multiple, quality, phase, passband, dsdRate, modulator, dsdUpsampling, dsdMultiple;
    ToggleSwitch pcmAsDsd;
    ComboBox pcmDsdRate;
    ToggleSwitch dspOnDsd;
};

constexpr int kDsdUpsamplingModes = 3;

int indexIn(std::span<const uint32_t> values, uint32_t value) {
    const auto at = std::ranges::find(values, value);
    return at == values.end() ? 0 : int(at - values.begin());
}

uint32_t valueIn(std::span<const uint32_t> values, int index) {
    return values[size_t(std::clamp(index, 0, int(values.size()) - 1))];
}

constexpr int kUpsamplingModes = 3;

template <class T> void showRateControls(const RateControls& c, const T& t) {
    auto rates = rateItems();
    rates.insert(rates.begin() + 1, {tr(L"Rate_IntegerHighest"), tr(L"Rate_IntegerMultiple"), tr(L"Rate_AnyHighest")});
    const int setRate = rateIndex(t.fixedRate);
    fill(c.rate, rates, setRate ? setRate + kUpsamplingModes : int(t.upsampling));
    std::vector<hstring> multiples;
    for (const uint32_t m : kUpsamplingMultiples) multiples.push_back(to_hstring(m) + L"×");
    const auto multiple = std::ranges::find(kUpsamplingMultiples, t.upsamplingMultiple);
    fill(c.multiple, multiples, int(multiple - std::begin(kUpsamplingMultiples)));
    fill(c.quality, {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")}, int(t.filter.quality));
    fill(c.phase, {tr(L"Phase_Linear"), tr(L"Phase_Intermediate"), tr(L"Phase_Minimum")}, int(t.filter.phase));
    fill(c.passband, {tr(L"Passband_Standard"), tr(L"Passband_Steep"), tr(L"Passband_Gentle")}, int(t.filter.passband));
    fill(c.modulator, {tr(L"Modulator_Order5"), tr(L"Modulator_Order7")}, indexIn(kModulatorOrders, t.filter.modulatorOrder));
    std::vector<hstring> dsdRates;
    for (const uint32_t rate : kDsdSetRates) dsdRates.emplace_back(L"DSD" + to_hstring(rate));
    std::vector<hstring> modes{tr(L"DsdUpsampling_Off"), tr(L"DsdUpsampling_Highest"), tr(L"DsdUpsampling_Multiple")};
    modes.insert(modes.end(), dsdRates.begin(), dsdRates.end());
    const auto& d = t.dsdUpsampling;
    fill(c.dsdUpsampling, modes,
         d.mode == DsdUpsampling::Set ? kDsdUpsamplingModes + indexIn(kDsdSetRates, d.rate) : int(d.mode));
    std::vector<hstring> dsdMultiples;
    for (const uint32_t m : kDsdUpsamplingMultiples) dsdMultiples.push_back(to_hstring(m) + L"×");
    fill(c.dsdMultiple, dsdMultiples, indexIn(kDsdUpsamplingMultiples, d.multiple));
    c.pcmAsDsd.IsOn(d.pcm);
    fill(c.pcmDsdRate, dsdRates, indexIn(kDsdSetRates, d.rate));
    c.dspOnDsd.IsOn(t.dspOnDsd || d.mode != DsdUpsampling::Off);
}

template <class T> void readRateControls(const RateControls& c, T& t) {
    const int choice = std::max(0, c.rate.SelectedIndex());
    t.upsampling = choice <= kUpsamplingModes ? Upsampling(choice) : Upsampling::Off;
    t.fixedRate = choice > kUpsamplingModes ? rateAt(choice - kUpsamplingModes) : 0;
    t.upsamplingMultiple =
        kUpsamplingMultiples[std::clamp(c.multiple.SelectedIndex(), 0, int(std::size(kUpsamplingMultiples)) - 1)];
    t.filter.quality = ResampleQuality(std::clamp(c.quality.SelectedIndex(), 0, 2));
    t.filter.phase = FilterPhase(std::clamp(c.phase.SelectedIndex(), 0, 2));
    t.filter.passband = FilterPassband(std::clamp(c.passband.SelectedIndex(), 0, 2));
    t.filter.modulatorOrder = valueIn(kModulatorOrders, c.modulator.SelectedIndex());
    auto& d = t.dsdUpsampling;
    const int mode = std::max(0, c.dsdUpsampling.SelectedIndex());
    d.mode = mode < kDsdUpsamplingModes ? DsdUpsampling(mode) : DsdUpsampling::Set;
    // The set rate is the one rate both lists show: the mode's, else PCM's.
    d.rate = valueIn(kDsdSetRates, mode >= kDsdUpsamplingModes ? mode - kDsdUpsamplingModes : c.pcmDsdRate.SelectedIndex());
    d.multiple = valueIn(kDsdUpsamplingMultiples, c.dsdMultiple.SelectedIndex());
    d.pcm = c.pcmAsDsd.IsOn();
    // Greyed, as with DSD upsampling or DSD to PCM, the switch shows what it means, not the output's own choice.
    if (c.dspOnDsd.IsEnabled()) t.dspOnDsd = c.dspOnDsd.IsOn();
}

// The multiple only for upsampling by one, phase and passband not for soxr's quick cubic interpolator, and DSD to PCM's
// own rate only while the choice follows the file, as DSD converted to PCM otherwise follows the choice. DSD upsampling
// only while DSD is sent as DSD, the modulator and PCM as DSD only while it is on, its multiple only by a multiple, and
// PCM's DSD rate only while PCM goes as DSD by a multiple, showing the set rate otherwise, as both lists hold the one
// rate; PCM sent as DSD takes no PCM rate. What depends on a choice not made is not shown; what shows a value in effect
// is greyed.
template <class T> void enableRateControls(const RateControls& c, const T& t, bool enabled) {
    const auto shown = [](bool on) { return on ? Visibility::Visible : Visibility::Collapsed; };
    const auto& d = t.dsdUpsampling;
    const bool dsd = enabled && t.dsdOutput != DsdOutput::Pcm;
    const bool upsampling = dsd && d.mode != DsdUpsampling::Off;
    const bool pcmRate = enabled && !(upsampling && d.pcm);
    c.rate.IsEnabled(pcmRate);
    c.multiple.Visibility(shown(pcmRate && !t.fixedRate && t.upsampling == Upsampling::IntegerMultiple));
    c.quality.IsEnabled(enabled);
    c.phase.IsEnabled(enabled && t.filter.quality != ResampleQuality::Quick);
    c.passband.IsEnabled(enabled && t.filter.quality != ResampleQuality::Quick);
    c.dsdRate.IsEnabled(!enabled || (!t.fixedRate && t.upsampling == Upsampling::Off));
    c.modulator.Visibility(shown(upsampling));
    c.dsdUpsampling.IsEnabled(dsd);
    c.dsdMultiple.Visibility(shown(upsampling && d.mode == DsdUpsampling::Multiple));
    c.pcmAsDsd.Visibility(shown(upsampling));
    c.pcmDsdRate.Visibility(shown(upsampling && d.pcm));
    if (const int rate = indexIn(kDsdSetRates, d.rate); d.mode == DsdUpsampling::Set && c.pcmDsdRate.SelectedIndex() != rate)
        c.pcmDsdRate.SelectedIndex(rate);
    // DSD upsampling always takes DSD sources through the DSP. Greyed first, so that showing it on is not read back as
    // the output's own choice.
    c.dspOnDsd.IsEnabled(dsd && !upsampling);
    if (const bool on = t.dspOnDsd || d.mode != DsdUpsampling::Off; c.dspOnDsd.IsOn() != on) c.dspOnDsd.IsOn(on);
    c.pcmDsdRate.IsEnabled(upsampling && d.pcm && d.mode == DsdUpsampling::Multiple);
}

RateControls wasapiRateControls(MainWindow& w) {
    return {w.WasapiRateCombo(),        w.WasapiMultipleCombo(),  w.WasapiQualityCombo(),    w.WasapiPhaseCombo(),
            w.WasapiPassbandCombo(),    w.WasapiDsdRateCombo(),   w.WasapiModulatorCombo(),  w.WasapiDsdUpsamplingCombo(),
            w.WasapiDsdMultipleCombo(), w.WasapiPcmAsDsdSwitch(), w.WasapiPcmDsdRateCombo(), w.WasapiDspOnDsdSwitch()};
}

RateControls asioRateControls(MainWindow& w) {
    return {w.AsioRateCombo(),        w.AsioMultipleCombo(),  w.AsioQualityCombo(),    w.AsioPhaseCombo(),
            w.AsioPassbandCombo(),    w.AsioDsdRateCombo(),   w.AsioModulatorCombo(),  w.AsioDsdUpsamplingCombo(),
            w.AsioDsdMultipleCombo(), w.AsioPcmAsDsdSwitch(), w.AsioPcmDsdRateCombo(), w.AsioDspOnDsdSwitch()};
}

RateControls ksRateControls(MainWindow& w) {
    return {w.KsRateCombo(),        w.KsMultipleCombo(),  w.KsQualityCombo(),    w.KsPhaseCombo(),
            w.KsPassbandCombo(),    w.KsDsdRateCombo(),   w.KsModulatorCombo(),  w.KsDsdUpsamplingCombo(),
            w.KsDsdMultipleCombo(), w.KsPcmAsDsdSwitch(), w.KsPcmDsdRateCombo(), w.KsDspOnDsdSwitch()};
}

RateControls networkRateControls(MainWindow& w) {
    return {w.NetworkSampleRateCombo(),  w.NetworkMultipleCombo(),  w.NetworkQualityCombo(),    w.NetworkPhaseCombo(),
            w.NetworkPassbandCombo(),    w.NetworkDsdRateCombo(),   w.NetworkModulatorCombo(),  w.NetworkDsdUpsamplingCombo(),
            w.NetworkDsdMultipleCombo(), w.NetworkPcmAsDsdSwitch(), w.NetworkPcmDsdRateCombo(), w.NetworkDspOnDsdSwitch()};
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

// The groups of OutputSettingsPanel, from `p`.
void MainWindow::showOutputParameters(const SettingsDto& p) {
    fill(WasapiShareCombo(), {tr(L"Share_Exclusive"), tr(L"Share_Shared")}, p.wasapi.shared ? 1 : 0);
    fill(WasapiSharedQualityCombo(), {tr(L"Quality_Quick"), tr(L"Quality_High"), tr(L"Quality_VeryHigh")},
         int(p.wasapi.sharedResampleQuality));
    fill(WasapiModeCombo(), {tr(L"Mode_Event"), tr(L"Mode_Polling")}, p.wasapi.eventDriven ? 0 : 1);
    fill(WasapiPeriodCombo(), {tr(L"Period_Default"), tr(L"Period_Minimum"), tr(L"Period_Custom")}, int(p.wasapi.period));
    WasapiPeriodBox().Value(p.wasapi.customPeriodMs);
    const std::vector<hstring> formats = {tr(L"Format_Auto"), L"16", L"24", L"24-in-32", L"32"};
    fill(WasapiFormatCombo(), formats, formatIndex(p.wasapi.sampleType));
    const auto rates = rateItems();
    const std::vector<hstring> tasks = {L"Pro Audio", L"Playback", tr(L"Mmcss_Disabled")};
    const std::vector<hstring> priorities = {tr(L"Priority_Normal"), tr(L"Priority_High"), tr(L"Priority_Critical")};
    showRateControls(wasapiRateControls(*this), p.wasapi);
    // WASAPI and KS have no native DSD; a stored Native plays as DoP.
    const std::vector<hstring> dopOrPcm = {tr(L"Dsd_Dop"), tr(L"Dsd_Pcm")};
    const auto dsdRates = dsdRateItems();
    fill(WasapiDsdCombo(), dopOrPcm, p.wasapi.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(WasapiDsdRateCombo(), dsdRates, dsdRateIndex(p.wasapi.dsdPcmRate));
    auto deviceLimits = rates;
    deviceLimits.front() = tr(L"Limit_FollowDevice");
    const auto deviceDsdLimits = maxDsdItems(tr(L"Limit_FollowDevice"));
    fill(WasapiMaxRateCombo(), deviceLimits, rateIndex(p.wasapi.maxSampleRate));
    fill(WasapiMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(p.wasapi.maxDsdRate));
    WasapiPollBufferBox().Value(p.wasapi.pollBufferMs);
    fill(WasapiMmcssTaskCombo(), tasks, int(p.wasapi.mmcssTask));
    fill(WasapiMmcssPriorityCombo(), priorities, int(p.wasapi.mmcssPriority));
    showTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), p.wasapi.timing);

    AsioBufferBox().Value(double(p.asio.bufferSize));
    AsioLeftBox().Value(p.asio.outLeft + 1);
    AsioRightBox().Value(p.asio.outRight + 1);
    showRateControls(asioRateControls(*this), p.asio);
    fill(AsioDsdCombo(), {tr(L"Dsd_Native"), tr(L"Dsd_Dop"), tr(L"Dsd_Pcm")}, int(p.asio.dsdOutput));
    fill(AsioDsdRateCombo(), dsdRates, dsdRateIndex(p.asio.dsdPcmRate));
    fill(AsioMaxRateCombo(), deviceLimits, rateIndex(p.asio.maxSampleRate));
    fill(AsioMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(p.asio.maxDsdRate));
    showTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), p.asio.timing);

    fill(KsModeCombo(), {tr(L"KsMode_Auto"), tr(L"KsMode_RtEvent"), tr(L"KsMode_RtPoll"), tr(L"KsMode_Standard")},
         int(p.ks.mode));
    KsBufferBox().Value(p.ks.bufferMs);
    KsPacketCountBox().Value(p.ks.packetCount);
    KsPacketMsBox().Value(p.ks.packetMs);
    fill(KsFormatCombo(), formats, formatIndex(p.ks.sampleType));
    showRateControls(ksRateControls(*this), p.ks);
    fill(KsDsdCombo(), dopOrPcm, p.ks.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(KsDsdRateCombo(), dsdRates, dsdRateIndex(p.ks.dsdPcmRate));
    fill(KsMaxRateCombo(), deviceLimits, rateIndex(p.ks.maxSampleRate));
    fill(KsMaxDsdCombo(), deviceDsdLimits, maxDsdIndex(p.ks.maxDsdRate));
    fill(KsMmcssTaskCombo(), tasks, int(p.ks.mmcssTask));
    fill(KsMmcssPriorityCombo(), priorities, int(p.ks.mmcssPriority));
    showTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), p.ks.timing);

    showRateControls(networkRateControls(*this), p.network);
    auto limits = rates;
    limits.front() = tr(L"Limit_FollowPlayer");
    fill(NetworkRateCombo(), limits, rateIndex(p.network.maxSampleRate));
    fill(NetworkBitsCombo(), {tr(L"Limit_FollowPlayer"), L"16"}, p.network.maxBits ? 1 : 0);
    fill(NetworkDsdCombo(), {tr(L"Dsd_Dsf"), tr(L"Dsd_Dop"), tr(L"Dsd_Pcm")}, int(p.network.dsdOutput));
    fill(NetworkDsdRateCombo(), dsdRates, dsdRateIndex(p.network.dsdPcmRate));
    fill(NetworkMaxDsdCombo(), maxDsdItems(tr(L"Limit_FollowPlayer")), maxDsdIndex(p.network.maxDsdRate));
    NetworkGaplessSwitch().IsOn(p.network.gapless);
    showTiming(NetworkPredecodeBox(), NetworkSilenceBox(), NetworkReleaseBox(), p.network.timing);
    fill(AirPlayChannelsCombo(), {tr(L"Channels_Stereo"), tr(L"Channels_Left"), tr(L"Channels_Right")}, int(p.airplay.channels));
    fill(HQPlayerDsdCombo(), {tr(L"Dsd_DsfHQPlayer"), tr(L"Dsd_Pcm")}, p.hqplayer.dsdOutput == DsdOutput::Pcm ? 1 : 0);
    fill(HQPlayerDsdRateCombo(), dsdRates, dsdRateIndex(p.hqplayer.dsdPcmRate));
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

    showOutputParameters(outputParameters());

    const int lang = settings_.language == "zh-TW" ? 1 : settings_.language == "en-US" ? 2 : 0;
    fill(LanguageCombo(), {tr(L"Lang_System"), L"繁體中文", L"English"}, lang);
    fill(CloseActionCombo(), {tr(L"Close_Tray"), tr(L"Close_Exit")}, settings_.closeAction == CloseAction::Exit ? 1 : 0);
    showStartWithWindows();
    populateComputers();
    fill(ThemeCombo(), {tr(L"Theme_System"), tr(L"Theme_Light"), tr(L"Theme_Dark"), tr(L"Theme_Black")}, int(settings_.theme));
    populateOutputs();
    populating_ = false;
    updateBackendGroups();
    applyTheme();
}

std::array<std::pair<ComboBox, AudioFormat>, 3> MainWindow::decoderCombos() {
    return {
        {{DecoderCombo(), AudioFormat::Flac}, {WavDecoderCombo(), AudioFormat::Wav}, {AiffDecoderCombo(), AudioFormat::Aiff}}};
}

// The groups of OutputSettingsPanel, into `p`.
void MainWindow::readOutputParameters(SettingsDto& p) {
    auto idx = [](ComboBox const& c) { return std::max(0, c.SelectedIndex()); };
    p.wasapi.shared = idx(WasapiShareCombo()) == 1;
    p.wasapi.sharedResampleQuality = ResampleQuality(std::min(idx(WasapiSharedQualityCombo()), 2));
    p.wasapi.eventDriven = idx(WasapiModeCombo()) == 0;
    p.wasapi.period = WasapiPeriod(std::min(idx(WasapiPeriodCombo()), 2));
    p.wasapi.customPeriodMs = numberOr(WasapiPeriodBox(), p.wasapi.customPeriodMs);
    p.wasapi.sampleType = kFormatChoices[std::min(idx(WasapiFormatCombo()), 4)];
    readRateControls(wasapiRateControls(*this), p.wasapi);
    p.wasapi.dsdOutput = idx(WasapiDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Dop;
    p.wasapi.dsdPcmRate = dsdRateAt(idx(WasapiDsdRateCombo()));
    p.wasapi.maxSampleRate = rateAt(idx(WasapiMaxRateCombo()));
    p.wasapi.maxDsdRate = maxDsdAt(idx(WasapiMaxDsdCombo()));
    p.wasapi.pollBufferMs = std::clamp(numberOr(WasapiPollBufferBox(), p.wasapi.pollBufferMs), 20.0, 2000.0);
    p.wasapi.mmcssTask = MmcssTask(std::min(idx(WasapiMmcssTaskCombo()), 2));
    p.wasapi.mmcssPriority = MmcssPriority(std::min(idx(WasapiMmcssPriorityCombo()), 2));
    readTiming(WasapiPredecodeBox(), WasapiSilenceBox(), WasapiReleaseBox(), p.wasapi.timing);

    p.asio.bufferSize = long(numberOr(AsioBufferBox(), double(p.asio.bufferSize)));
    p.asio.outLeft = int(numberOr(AsioLeftBox(), p.asio.outLeft + 1)) - 1;
    p.asio.outRight = int(numberOr(AsioRightBox(), p.asio.outRight + 1)) - 1;
    readRateControls(asioRateControls(*this), p.asio);
    p.asio.dsdOutput = DsdOutput(std::min(idx(AsioDsdCombo()), 2));
    p.asio.dsdPcmRate = dsdRateAt(idx(AsioDsdRateCombo()));
    p.asio.maxSampleRate = rateAt(idx(AsioMaxRateCombo()));
    p.asio.maxDsdRate = maxDsdAt(idx(AsioMaxDsdCombo()));
    readTiming(AsioPredecodeBox(), AsioSilenceBox(), AsioReleaseBox(), p.asio.timing);

    p.ks.mode = KsMode(std::min(idx(KsModeCombo()), 3));
    p.ks.bufferMs = numberOr(KsBufferBox(), p.ks.bufferMs);
    p.ks.packetCount = int(numberOr(KsPacketCountBox(), p.ks.packetCount));
    p.ks.packetMs = numberOr(KsPacketMsBox(), p.ks.packetMs);
    p.ks.sampleType = kFormatChoices[std::min(idx(KsFormatCombo()), 4)];
    readRateControls(ksRateControls(*this), p.ks);
    p.ks.dsdOutput = idx(KsDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Dop;
    p.ks.dsdPcmRate = dsdRateAt(idx(KsDsdRateCombo()));
    p.ks.maxSampleRate = rateAt(idx(KsMaxRateCombo()));
    p.ks.maxDsdRate = maxDsdAt(idx(KsMaxDsdCombo()));
    p.ks.mmcssTask = MmcssTask(std::min(idx(KsMmcssTaskCombo()), 2));
    p.ks.mmcssPriority = MmcssPriority(std::min(idx(KsMmcssPriorityCombo()), 2));
    readTiming(KsPredecodeBox(), KsSilenceBox(), KsReleaseBox(), p.ks.timing);

    readRateControls(networkRateControls(*this), p.network);
    p.network.maxSampleRate = rateAt(idx(NetworkRateCombo()));
    p.network.maxBits = idx(NetworkBitsCombo()) == 1 ? 16 : 0;
    p.network.dsdOutput = DsdOutput(std::min(idx(NetworkDsdCombo()), 2));
    p.network.dsdPcmRate = dsdRateAt(idx(NetworkDsdRateCombo()));
    p.network.maxDsdRate = maxDsdAt(idx(NetworkMaxDsdCombo()));
    p.network.gapless = NetworkGaplessSwitch().IsOn();
    readTiming(NetworkPredecodeBox(), NetworkSilenceBox(), NetworkReleaseBox(), p.network.timing);
    p.airplay.channels = AirPlayChannels(std::clamp(idx(AirPlayChannelsCombo()), 0, 2));
    p.hqplayer.dsdOutput = idx(HQPlayerDsdCombo()) == 1 ? DsdOutput::Pcm : DsdOutput::Native;
    p.hqplayer.dsdPcmRate = dsdRateAt(idx(HQPlayerDsdRateCombo()));
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

    readOutputParameters(settings_);
    settings_.closeAction = idx(CloseActionCombo()) == 1 ? CloseAction::Exit : CloseAction::Tray;
    settings_.theme = Theme(std::clamp(idx(ThemeCombo()), 0, 3));
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

void MainWindow::OnSettingToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    settingsChanged();
}

void MainWindow::OnQueueAutoplayClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    AutoplaySwitch().IsOn(QueueAutoplayItem().IsChecked());
}

void MainWindow::settingsChanged() {
    if (populating_ || !server_) return;
    if (editingOutput_) {
        auto& editing = *editingOutput_;
        readOutputParameters(editing.parameters);
        auto real = nativeSettings(settings_);
        camel::server::keepOutputParameters(real, nativeSettings(editing.parameters), editing.backend, editing.id,
                                            server_->snapshot().devices);
        settings_ = settingsDto(real);
        Command command;
        command.kind = CommandKind::ApplySettings;
        command.settings = settings_;
        submitCommand(std::move(command));
        updateBackendGroups();
        return;
    }
    readSettingsFromUi();
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
    const auto& p = outputParameters();
    WasapiGroup().Visibility(p.backend == Backend::Wasapi ? Visibility::Visible : Visibility::Collapsed);
    AsioGroup().Visibility(p.backend == Backend::Asio ? Visibility::Visible : Visibility::Collapsed);
    KsGroup().Visibility(p.backend == Backend::Ks ? Visibility::Visible : Visibility::Collapsed);
    NetworkGroup().Visibility(p.backend == Backend::Network ? Visibility::Visible : Visibility::Collapsed);
    AirPlayGroup().Visibility(p.backend == Backend::AirPlay ? Visibility::Visible : Visibility::Collapsed);
    HQPlayerGroup().Visibility(p.backend == Backend::HQPlayer ? Visibility::Visible : Visibility::Collapsed);
    WasapiPeriodBox().IsEnabled(p.wasapi.period == WasapiPeriod::Custom);
    enableRateControls(wasapiRateControls(*this), p.wasapi, !p.wasapi.shared); // the mixer sets the rate in shared mode
    enableRateControls(asioRateControls(*this), p.asio, true);
    enableRateControls(ksRateControls(*this), p.ks, true);
    enableRateControls(networkRateControls(*this), p.network, true);
    WasapiMaxRateCombo().IsEnabled(!p.wasapi.shared);
    WasapiMaxDsdCombo().IsEnabled(!p.wasapi.shared);
    WasapiPollBufferBox().IsEnabled(!p.wasapi.eventDriven);
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
