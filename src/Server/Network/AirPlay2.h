// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// What an AirPlay 2 realtime session sends besides AirPlay 1's (docs/Plan.md §5.10), for the receivers AirPlay 1 does not
// play: the session's and the stream's SETUP as binary plists, with NTP timing on this computer's port, and RTP packets
// whose payload is sealed with ChaCha20-Poly1305 under the stream's key. Written from Emanuele Cozzi's "AirPlay 2
// Internals" and checked on the owner's Mac and Sony TV (2026-11-18).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Server/Network/Pairing.h"
#include "Server/Network/Plist.h"

namespace camel::server::airplay {

inline constexpr std::string_view kPlistType = "application/x-apple-binary-plist";
inline constexpr size_t kStreamKeyBytes = 32;

std::string randomUuid(); // upper case, as Apple's senders write it

// The session's SETUP: who sends, and that the receiver asks this computer's `timingPort` for NTP time.
Plist sessionSetup(uint16_t timingPort, std::string_view groupUuid);
// The stream's SETUP: realtime ALAC 44.1 kHz/16-bit stereo in packets of 352 frames, sealed under `key`, with this
// computer's `controlPort` for sync and resends. A Sony TV answers 400 without `sr`, `streamConnectionID` and
// `supportsDynamicStreamID` false.
Plist streamSetup(std::string_view key, uint16_t controlPort, int64_t connectionId);

struct StreamPorts {
    uint16_t data = 0, control = 0;
};
std::optional<StreamPorts> streamPortsFrom(const Plist& answer); // the receiver's, from its answer to the stream's SETUP
std::optional<uint16_t> eventPortFrom(const Plist& answer);      // from its answer to the session's SETUP

// The RTP packet `rtp` with its payload sealed: the header's timestamp and SSRC as AAD, and the 8-byte nonce after the tag.
std::string sealedRtpPacket(std::string_view rtp, std::string_view key, uint64_t nonce);

// What a receiver asks of the sender on its event channel (`POST /command`, `sendMediaRemoteCommand`), as the owner's TV
// and Mac sent them (2026-11-18): its remote's play/pause (`plps`), next (`nitm`) and previous (`pitm`), a pause when it is
// turned off (`pbpr`, `pbal`), and its own volume (`dvlc`).
enum class ReceiverCommand : uint8_t { TogglePause, Pause, Next, Previous, Volume };
struct ReceiverEvent {
    ReceiverCommand command = ReceiverCommand::Pause;
    double volume = 0; // 0 to 1 over AirPlay's -30 to 0 dB, for Volume
    bool muted = false;
    bool operator==(const ReceiverEvent&) const = default;
};
std::optional<ReceiverEvent> receiverEventFrom(const Plist& command); // nothing for one CamelPlayer does not follow

// A session's event channel, sealed as its RTSP connection with keys of its own: the receiver's requests, read without
// waiting, each answered 200.
class EventChannel {
public:
    EventChannel(const std::string& address, uint16_t port, std::string_view sessionKey, int timeoutMs);
    ~EventChannel();
    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;

    std::vector<ReceiverEvent> read();

private:
    uintptr_t socket_ = 0; // a SOCKET
    SecureChannel channel_;
    std::string sealed_, plain_;
    bool damaged_ = false;
};

} // namespace camel::server::airplay
