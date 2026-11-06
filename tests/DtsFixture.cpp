// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "DtsFixture.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "TestUtil.h"

namespace testutil {

namespace {

constexpr int kChannels = 6;

} // namespace

DtsCd copyDtsCd(const std::filesystem::path& path) {
    copyFixture("dts-cd.wav", path);
    std::ifstream in(fixtureFile("dts-cd.f32"), std::ios::binary);
    if (!in) throw std::runtime_error("DTS fixture: no stored decode");
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    DtsCd file;
    file.reference.resize(bytes.size() / sizeof(float));
    std::memcpy(file.reference.data(), bytes.data(), file.reference.size() * sizeof(float));
    file.frames = file.reference.size() / kChannels;
    return file;
}

} // namespace testutil
