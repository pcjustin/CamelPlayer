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
