// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <cstdint>

namespace camel::audio {

// Where the data after the ID3v2 tags at the start of a file begins: some taggers leave two tags in a row, and a tag may
// end in a 10-byte footer. `read(offset, header)` fills the 10 bytes at `offset`, false when it cannot.
template <class Read> uint64_t afterId3Tags(Read&& read) {
    uint64_t pos = 0;
    for (uint8_t h[10]; read(pos, h) && h[0] == 'I' && h[1] == 'D' && h[2] == '3';)
        pos += 10 +
               ((uint64_t(h[6] & 0x7f) << 21) | (uint64_t(h[7] & 0x7f) << 14) | (uint64_t(h[8] & 0x7f) << 7) |
                uint64_t(h[9] & 0x7f)) +
               ((h[5] & 0x10) ? 10 : 0); // synchsafe size, optional footer
    return pos;
}

} // namespace camel::audio
