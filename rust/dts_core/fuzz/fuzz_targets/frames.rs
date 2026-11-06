// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#![no_main]

use dts_core::{Decoder, find_sync, frame_info};
use libfuzzer_sys::fuzz_target;
use sancov_bounds as _;

// As the Engine reads a file: every sync, a frame's header, its decode, and the state a bad frame leaves.
fuzz_target!(|data: &[u8]| {
    let mut decoder = Decoder::new();
    let mut start = 0;
    while let Some(sync) = find_sync(data, start) {
        let frame = &data[sync.offset..];
        let _ = frame_info(frame);
        if decoder.decode(frame).is_err() {
            decoder.reset();
        }
        start = sync.offset + 1;
    }
});
