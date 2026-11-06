// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The Engine's ring under Loom, which runs each test through the thread interleavings and memory orderings it allows.

#![cfg(test)]

#[path = "../../audio_engine/src/ring.rs"]
mod ring;

use loom::sync::Arc;
use loom::thread;
use ring::SpscByteRing;

fn model(test: impl Fn() + Sync + Send + 'static) {
    let mut builder = loom::model::Builder::new();
    builder.preemption_bound = Some(3);
    builder.check(test);
}

fn receive(ring: &SpscByteRing, count: usize) -> Vec<u8> {
    let mut received = Vec::new();
    while received.len() < count {
        let mut bytes = [0; 2];
        let read = ring.read(&mut bytes);
        received.extend_from_slice(&bytes[..read]);
        if read == 0 {
            thread::yield_now();
        }
    }
    received
}

fn send(ring: &SpscByteRing, bytes: &[u8]) {
    let mut sent = 0;
    while sent < bytes.len() {
        let written = ring.write(&bytes[sent..]);
        sent += written;
        if written == 0 {
            thread::yield_now();
        }
    }
}

#[test]
fn what_is_written_arrives_whole() {
    model(|| {
        let ring = Arc::new(SpscByteRing::new(3).unwrap());
        let producer = {
            let ring = Arc::clone(&ring);
            thread::spawn(move || send(&ring, &[1, 2]))
        };
        assert_eq!(receive(&ring, 2), [1, 2]);
        producer.join().unwrap();
        assert_eq!(ring.readable(), 0);
    });
}

#[test]
fn a_write_across_the_end_arrives_in_order() {
    model(|| {
        let ring = Arc::new(SpscByteRing::new(3).unwrap());
        assert_eq!(ring.write(&[9, 9]), 2);
        assert_eq!(receive(&ring, 2), [9, 9]);
        let producer = {
            let ring = Arc::clone(&ring);
            thread::spawn(move || send(&ring, &[1, 2]))
        };
        assert_eq!(receive(&ring, 2), [1, 2]);
        producer.join().unwrap();
    });
}

#[test]
fn a_full_ring_loses_nothing_and_never_overwrites() {
    model(|| {
        let ring = Arc::new(SpscByteRing::new(2).unwrap());
        let producer = {
            let ring = Arc::clone(&ring);
            thread::spawn(move || send(&ring, &[1, 2, 3]))
        };
        assert_eq!(receive(&ring, 3), [1, 2, 3]);
        producer.join().unwrap();
        assert_eq!(ring.writable(), ring.capacity());
    });
}

#[test]
fn the_reader_clearing_keeps_what_follows_in_order() {
    model(|| {
        let ring = Arc::new(SpscByteRing::new(4).unwrap());
        let producer = {
            let ring = Arc::clone(&ring);
            thread::spawn(move || {
                send(&ring, &[1]);
                send(&ring, &[2]);
            })
        };
        ring.clear();
        producer.join().unwrap();
        let left = ring.readable();
        let received = receive(&ring, left);
        assert!(received.as_slice() == [1, 2] || received.as_slice() == [2] || received.is_empty());
    });
}
