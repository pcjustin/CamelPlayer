// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#![no_main]

use dts_core::{Decoder, find_sync, frame_info};
use libfuzzer_sys::fuzz_target;
use sancov_bounds as _;

// As DtsStage reads a file: from each sync a frame's header, then its decode, past it on success and two bytes on
// failure. A frame that fails part-way leaves its filter state to the next, as the Engine resets only after a panic.
fuzz_target!(|data: &[u8]| {
    let mut decoder = Decoder::new();
    let mut start = 0;
    while let Some(sync) = find_sync(data, start) {
        let frame = &data[sync.offset..];
        let Ok(info) = frame_info(frame) else {
            start = sync.offset + 2;
            continue;
        };
        if frame.len() < info.container_bytes {
            break;
        }
        start = sync.offset
            + match decoder.decode(frame) {
                Ok(_) => info.container_bytes,
                Err(_) => 2,
            };
    }
});
