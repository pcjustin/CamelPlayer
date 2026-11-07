// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// DSD64 to DSD512 on Chord/DAVE (docs/Plan.md §5.8, M2-R09), gated on what the device reports: native DSD on ASIO and
// DoP on WASAPI, ASIO and KS. A DoP carrier the device does not offer (DSD512 needs 1411.2 kHz) must be refused with
// that reason, never assumed. KS native DSD has no case: no tested pin reports a DSD data format. Generated low-level
// tones; opt in with --gtest_also_run_disabled_tests --gtest_filter=M2/DsdRateHardware.*
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "DsdFixture.h"
#include "Engine/PlaybackEngine.h"
#include "TestUtil.h"

using namespace camel;
using namespace camel::audio;
using namespace std::chrono_literals;

namespace {

struct RateCase {
    Backend backend;
    DsdOutput output;
    uint32_t multiple; // of DSD64
};

std::vector<RateCase> rateCases() {
    std::vector<RateCase> out;
    for (const uint32_t multiple : {1u, 2u, 4u, 8u}) {
        out.push_back({Backend::Asio, DsdOutput::Native, multiple});
        for (const Backend backend : {Backend::Wasapi, Backend::Asio, Backend::Ks})
            out.push_back({backend, DsdOutput::Dop, multiple});
    }
    return out;
}

bool selectChord(Settings& settings, Backend backend) {
    const auto devices = backend == Backend::Wasapi ? listWasapiDevices()
                         : backend == Backend::Asio ? listAsioDrivers()
                                                    : listKsPins();
    const auto it = std::find_if(devices.begin(), devices.end(), [](const DeviceEntry& d) {
        return d.name.find("Chord") != std::string::npos && d.name.find("unavailable") == std::string::npos;
    });
    if (it == devices.end()) return false;
    settings.backend = backend;
    if (backend == Backend::Wasapi)
        settings.wasapi.deviceId = it->id;
    else if (backend == Backend::Asio)
        settings.asio.driverClsid = it->id;
    else
        settings.ks.deviceId = it->id;
    return true;
}

std::filesystem::path toneFile(uint32_t multiple) {
    const auto path = testutil::tempDir() / ("dsd-rate-" + std::to_string(64 * multiple) + ".dsf");
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        const uint32_t rate = dsdfixture::kDsd64 * multiple;
        dsdfixture::writeFile(path, dsdfixture::dsf(dsdfixture::dsdSine(rate, 1000, 0.1, size_t(rate / 8) * 3), {.rate = rate}));
    }
    return path;
}

} // namespace

class DsdRateHardware : public ::testing::TestWithParam<RateCase> {};

std::string dsdRateName(const ::testing::TestParamInfo<RateCase>& info) {
    return std::string(backendId(info.param.backend)) + (info.param.output == DsdOutput::Native ? "_native_" : "_dop_") + "DSD" +
           std::to_string(64 * info.param.multiple);
}

TEST_P(DsdRateHardware, DISABLED_PlaysWhatTheDeviceOffers) {
    const auto& c = GetParam();
    Settings settings;
    ASSERT_TRUE(selectChord(settings, c.backend));
    settings.asio.dsdOutput = settings.ks.dsdOutput = c.output;
    const auto file = toneFile(c.multiple);
    RecordProperty("case", dsdRateName({c, 0}));
    PlaybackEngine engine(settings);
    engine.openAndPlay(file);
    engine.waitIdle();
    auto snap = engine.snapshot();
    const uint32_t carrier = dsdfixture::kDsd64 * c.multiple / 16;
    if (c.output == DsdOutput::Dop && carrier > 768000 && snap.error) {
        // A carrier above 768 kHz plays only where the device reports it. Refused up front when the device does not
        // offer it; a driver that accepts the rate but cannot start (Chord ASIO at 1411.2 kHz) fails with its error.
        // Either way nothing falls back to PCM.
        const bool refused =
            *snap.error == ErrorCode::FormatNotSupported && snap.errorDetail.find("1411.2 kHz") != std::string::npos;
        EXPECT_TRUE(refused || *snap.error == ErrorCode::DriverError) << errorCodeName(*snap.error) << ": " << snap.errorDetail;
        EXPECT_FALSE(snap.output);
        RecordProperty("result", std::string(refused ? "refused: " : "driver failed: ") + snap.errorDetail);
        return;
    }
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_TRUE(snap.plan && snap.output);
    EXPECT_EQ(snap.plan->dsd, c.output == DsdOutput::Native ? DsdPath::Native : DsdPath::Dop);
    EXPECT_EQ(snap.output->sampleRate, c.output == DsdOutput::Native ? dsdfixture::kDsd64 * c.multiple / 8 : carrier);
    const auto position = snap.position;
    std::this_thread::sleep_for(1s);
    snap = engine.snapshot();
    EXPECT_FALSE(snap.error) << snap.errorDetail;
    EXPECT_GT(snap.position, position);
    EXPECT_EQ(snap.underruns, 0u);
    RecordProperty("result", describe(*snap.output) + " via " + snap.outputStatus.mode);
    engine.stop();
    engine.waitIdle();
}

INSTANTIATE_TEST_SUITE_P(M2, DsdRateHardware, ::testing::ValuesIn(rateCases()), dsdRateName);

// Closing a native DSD stream returns the driver to PCM; its PCM sample type must be offered again.
TEST(DsdRateHardwareSwitch, DISABLED_AsioPlaysPcmAfterNativeDsd) {
    Settings settings;
    ASSERT_TRUE(selectChord(settings, Backend::Asio));
    settings.asio.dsdOutput = DsdOutput::Native;
    PlaybackEngine engine(settings);
    engine.openAndPlay(toneFile(1));
    engine.waitIdle();
    auto snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_TRUE(snap.plan);
    EXPECT_EQ(snap.plan->dsd, DsdPath::Native);
    engine.stop();
    engine.waitIdle();

    engine.openAndPlay(testutil::generatedFile({"asio-pcm-after-dsd-44.1k", 44100, 24, 2, 1.0, true, -40}));
    engine.waitIdle();
    snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_TRUE(snap.plan && snap.output);
    EXPECT_EQ(snap.plan->dsd, DsdPath::None);
    EXPECT_EQ(snap.output->sampleRate, 44100u);
    engine.stop();
    engine.waitIdle();
}

namespace {

// An output's highest PCM or DSD rate on Chord/DAVE (docs/Plan.md §6): the source above it reaches the device at the
// limited rate, played in real time without underruns.
struct LimitCase {
    const char* name;
    Backend backend;
    DsdOutput output;
    uint32_t sourceMultiple; // of DSD64; 0 for PCM at `pcmRate`
    uint32_t pcmRate;
    uint32_t maxSampleRate;
    uint32_t maxDsdRate;
    uint32_t expectRate; // the device format's rate
};

const LimitCase kLimitCases[] = {
    {"asio_native_DSD128_to_DSD64", Backend::Asio, DsdOutput::Native, 2, 0, 0, 64, 352800},
    {"asio_native_DSD256_to_DSD64", Backend::Asio, DsdOutput::Native, 4, 0, 0, 64, 352800},
    {"asio_native_DSD256_to_DSD128", Backend::Asio, DsdOutput::Native, 4, 0, 0, 128, 705600},
    {"wasapi_dop_DSD128_to_DSD64", Backend::Wasapi, DsdOutput::Dop, 2, 0, 0, 64, 176400},
    {"ks_dop_DSD256_to_DSD128", Backend::Ks, DsdOutput::Dop, 4, 0, 0, 128, 352800},
    {"wasapi_dop_DSD128_under_192k", Backend::Wasapi, DsdOutput::Dop, 2, 0, 192000, 0, 176400},
    {"wasapi_pcm_352k_to_176k", Backend::Wasapi, DsdOutput::Dop, 0, 352800, 192000, 0, 176400},
    {"asio_pcm_384k_to_192k", Backend::Asio, DsdOutput::Dop, 0, 384000, 192000, 0, 192000},
    {"ks_pcm_705k_to_176k", Backend::Ks, DsdOutput::Dop, 0, 705600, 192000, 0, 176400},
};

} // namespace

class RateLimitHardware : public ::testing::TestWithParam<LimitCase> {};

TEST_P(RateLimitHardware, DISABLED_PlaysAtTheLimitedRate) {
    const auto& c = GetParam();
    Settings settings;
    ASSERT_TRUE(selectChord(settings, c.backend));
    settings.convertWhenUnsupported = false; // the limit alone permits the conversion
    settings.asio.dsdOutput = settings.ks.dsdOutput = c.output;
    settings.wasapi.maxSampleRate = settings.asio.maxSampleRate = settings.ks.maxSampleRate = c.maxSampleRate;
    settings.wasapi.maxDsdRate = settings.asio.maxDsdRate = settings.ks.maxDsdRate = c.maxDsdRate;
    const auto file =
        c.sourceMultiple ? toneFile(c.sourceMultiple) : testutil::generatedFile({c.name, c.pcmRate, 24, 2, 4.0, true, -20});
    RecordProperty("case", c.name);
    PlaybackEngine engine(settings);
    engine.openAndPlay(file);
    engine.waitIdle();
    auto snap = engine.snapshot();
    ASSERT_FALSE(snap.error) << snap.errorDetail;
    ASSERT_TRUE(snap.plan && snap.output);
    EXPECT_EQ(snap.output->sampleRate, c.expectRate);
    EXPECT_EQ(snap.plan->state, PipelineState::Processed);
    if (c.sourceMultiple) {
        EXPECT_EQ(snap.plan->dsd, c.output == DsdOutput::Native ? DsdPath::Native : DsdPath::Dop);
        EXPECT_TRUE(snap.plan->remodulate);
    } else {
        EXPECT_EQ(snap.plan->dsd, DsdPath::None);
        EXPECT_TRUE(snap.plan->resample);
    }
    const auto position = snap.position;
    std::this_thread::sleep_for(2s);
    snap = engine.snapshot();
    EXPECT_FALSE(snap.error) << snap.errorDetail;
    EXPECT_GT(snap.position, position);
    EXPECT_EQ(snap.underruns, 0u);
    RecordProperty("result", describe(*snap.output) + " via " + snap.outputStatus.mode);
    engine.stop();
    engine.waitIdle();
}

INSTANTIATE_TEST_SUITE_P(M2, RateLimitHardware, ::testing::ValuesIn(kLimitCases),
                         [](const ::testing::TestParamInfo<LimitCase>& info) { return std::string(info.param.name); });
