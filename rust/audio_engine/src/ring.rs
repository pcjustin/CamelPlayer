// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::cell::UnsafeCell;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};

pub(crate) struct SpscByteRing {
    buffer: Box<[UnsafeCell<u8>]>,
    head: AtomicUsize,
    tail: AtomicUsize,
}

// SAFETY: The producer and consumer own disjoint slots; release/acquire cursors publish each side's writes.
unsafe impl Sync for SpscByteRing {}

impl SpscByteRing {
    pub(crate) fn new(capacity: usize) -> Result<Self, ()> {
        let mut buffer = Vec::new();
        buffer.try_reserve_exact(capacity).map_err(|_| ())?;
        buffer.resize_with(capacity, || UnsafeCell::new(0));
        Ok(Self {
            buffer: buffer.into_boxed_slice(),
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
        })
    }

    pub(crate) fn readable(&self) -> usize {
        self.head
            .load(Ordering::Acquire)
            .wrapping_sub(self.tail.load(Ordering::Acquire))
            .min(self.buffer.len())
    }

    pub(crate) fn writable(&self) -> usize {
        self.buffer.len().saturating_sub(self.readable())
    }

    pub(crate) fn write(&self, source: &[u8]) -> usize {
        let head = self.head.load(Ordering::Relaxed);
        let tail = self.tail.load(Ordering::Acquire);
        let occupied = head.wrapping_sub(tail).min(self.buffer.len());
        let length = source.len().min(self.buffer.len() - occupied);
        if length == 0 {
            return 0;
        }

        let offset = head % self.buffer.len();
        let first = length.min(self.buffer.len() - offset);
        let destination = self.buffer.as_ptr().cast::<u8>().cast_mut();
        unsafe {
            ptr::copy_nonoverlapping(source.as_ptr(), destination.add(offset), first);
            ptr::copy_nonoverlapping(source.as_ptr().add(first), destination, length - first);
        }
        self.head
            .store(head.wrapping_add(length), Ordering::Release);
        length
    }

    pub(crate) fn read(&self, destination: &mut [u8]) -> usize {
        let tail = self.tail.load(Ordering::Relaxed);
        let head = self.head.load(Ordering::Acquire);
        let occupied = head.wrapping_sub(tail).min(self.buffer.len());
        let length = destination.len().min(occupied);
        if length == 0 {
            return 0;
        }

        let offset = tail % self.buffer.len();
        let first = length.min(self.buffer.len() - offset);
        let source = self.buffer.as_ptr().cast::<u8>();
        unsafe {
            ptr::copy_nonoverlapping(source.add(offset), destination.as_mut_ptr(), first);
            ptr::copy_nonoverlapping(source, destination.as_mut_ptr().add(first), length - first);
        }
        self.tail
            .store(tail.wrapping_add(length), Ordering::Release);
        length
    }

    pub(crate) fn clear(&self) {
        let head = self.head.load(Ordering::Acquire);
        self.tail.store(head, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::SpscByteRing;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn wraps_and_preserves_fifo_order() {
        let ring = SpscByteRing::new(7).unwrap();
        assert_eq!(ring.write(&[0, 1, 2, 3, 4]), 5);
        let mut first = [0; 4];
        assert_eq!(ring.read(&mut first), 4);
        assert_eq!(first, [0, 1, 2, 3]);
        assert_eq!(ring.write(&[5, 6, 7, 8, 9, 10]), 6);
        assert_eq!(ring.write(&[11]), 0);
        let mut rest = [0; 7];
        assert_eq!(ring.read(&mut rest), 7);
        assert_eq!(rest, [4, 5, 6, 7, 8, 9, 10]);
        assert_eq!(ring.writable(), 7);
    }

    #[test]
    fn concurrent_producer_consumer_preserve_all_bytes() {
        let ring = Arc::new(SpscByteRing::new(257).unwrap());
        let producer_ring = Arc::clone(&ring);
        let producer = thread::spawn(move || {
            let mut next = 0u8;
            for _ in 0..100_000 {
                let byte = [next];
                while producer_ring.write(&byte) == 0 {
                    thread::yield_now();
                }
                next = next.wrapping_add(1);
            }
        });

        let mut expected = 0u8;
        let mut output = [0u8; 31];
        for _ in 0..100_000 {
            while ring.read(&mut output[..1]) == 0 {
                thread::yield_now();
            }
            assert_eq!(output[0], expected);
            expected = expected.wrapping_add(1);
        }
        producer.join().unwrap();
        assert_eq!(ring.readable(), 0);
    }
}
