// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Common/util/Text.h"
#include "Server/Api/ServerApi.h"

namespace camel::server {

SettingsDto settingsDto(const Settings& s) {
    return {s.backend,
            s.decoderByFormat,
            s.pcmBitsByFormat,
            s.convertWhenUnsupported,
            s.resampleQuality,
            s.convertDsdWhenUnsupported,
            s.wasapi,
            s.asio,
            s.ks,
            s.language,
            s.closeAction,
            s.trayNoticeShown,
            s.firstRunDone,
            toUtf8(s.lastFile)};
}

Settings nativeSettings(const SettingsDto& s) {
    return {s.backend,
            s.decoderByFormat,
            s.pcmBitsByFormat,
            s.convertWhenUnsupported,
            s.resampleQuality,
            s.convertDsdWhenUnsupported,
            s.wasapi,
            s.asio,
            s.ks,
            s.language,
            s.closeAction,
            s.trayNoticeShown,
            s.firstRunDone,
            toWide(s.lastFileUtf8)};
}

} // namespace camel::server
