// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The boundaries between tracks on their way to the device: the DSP stage publishes each (the output frame the track
//! starts at, and the frame its position restarts at), render takes them as it sends those frames. One producer, the
//! DSP stage under the stage lock, and one consumer, render.

#[cfg(loom)]
use loom::sync::atomic::{AtomicU64, Ordering};
#[cfg(not(loom))]
use std::sync::atomic::{AtomicU64, Ordering};

pub(crate) struct BoundaryQueue<const N: usize> {
    at: [AtomicU64; N],
    start: [AtomicU64; N],
    // Counts since the engine was created: the boundaries published, and those render has taken.
    published: AtomicU64,
    reached: AtomicU64,
}

impl<const N: usize> BoundaryQueue<N> {
    pub(crate) fn new() -> Self {
        BoundaryQueue {
            at: std::array::from_fn(|_| AtomicU64::new(0)),
            start: std::array::from_fn(|_| AtomicU64::new(0)),
            published: AtomicU64::new(0),
            reached: AtomicU64::new(0),
        }
    }

    pub(crate) fn published(&self) -> u64 {
        self.published.load(Ordering::Acquire)
    }

    pub(crate) fn reached(&self) -> u64 {
        self.reached.load(Ordering::Acquire)
    }

    pub(crate) fn full(&self) -> bool {
        self.published() - self.reached() == N as u64
    }

    /// The producer: the next track starts at output frame `at`, its position at `start`. False while the queue is
    /// full, until render takes the oldest. The entry is stored before the count that hands it to render.
    pub(crate) fn publish(&self, at: u64, start: u64) -> bool {
        let published = self.published.load(Ordering::Relaxed);
        if published - self.reached.load(Ordering::Acquire) == N as u64 {
            return false;
        }
        let slot = (published % N as u64) as usize;
        self.start[slot].store(start, Ordering::Relaxed);
        self.at[slot].store(at, Ordering::Relaxed);
        self.published.store(published + 1, Ordering::Release);
        true
    }

    /// The consumer: render has sent the device up to output frame `after`. Takes every boundary at or before it and
    /// returns how many, with the output frame and position start of the last.
    pub(crate) fn reach(&self, after: u64) -> Option<(u64, u64, u64)> {
        let reached = self.reached.load(Ordering::Relaxed);
        let published = self.published.load(Ordering::Acquire);
        let mut taken = 0;
        let mut last = None;
        while reached + taken < published {
            let slot = ((reached + taken) % N as u64) as usize;
            let at = self.at[slot].load(Ordering::Relaxed);
            if at > after {
                break;
            }
            last = Some((at, self.start[slot].load(Ordering::Relaxed)));
            taken += 1;
        }
        let (at, start) = last?;
        // The slots are read before they are given back to the producer.
        self.reached.store(reached + taken, Ordering::Release);
        Some((taken, at, start))
    }

    /// Drops what is still to be reached. Only with render drained and the producer's lock held.
    pub(crate) fn clear(&self) {
        self.reached.store(self.published(), Ordering::Release);
    }
}
