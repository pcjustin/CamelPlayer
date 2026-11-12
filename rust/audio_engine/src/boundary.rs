// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Render's side of the boundary between tracks, which the decoding thread publishes by storing the next track's start
//! frame and then the output frame it starts at, once render has reached the previous one.

#[cfg(loom)]
use loom::sync::atomic::{AtomicBool, AtomicU64, Ordering};
#[cfg(not(loom))]
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

pub(crate) const NO_BOUNDARY: u64 = u64::MAX;

/// Render has sent the device up to output frame `after`: a published boundary at or before it is taken, leaving room
/// for the next, and its output frame and its track's start frame are returned.
pub(crate) fn reach(
    at: &AtomicU64,
    start_frame: &AtomicU64,
    pending: &AtomicBool,
    after: u64,
) -> Option<(u64, u64)> {
    let boundary = at.load(Ordering::Acquire);
    if boundary == NO_BOUNDARY || boundary > after {
        return None;
    }
    // The start is read and the pending mark cleared before the boundary is given up: once it is, the decoding thread
    // may publish the next track's start, and a crossfade mark its own boundary pending.
    let start = start_frame.load(Ordering::Acquire);
    pending.store(false, Ordering::Release);
    at.store(NO_BOUNDARY, Ordering::Release);
    Some((boundary, start))
}
