// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The Engine's ring under Loom, which runs each test through the thread interleavings and memory orderings it allows.

#![cfg(test)]

#[path = "../../audio_engine/src/ring.rs"]
mod ring;

#[path = "../../audio_engine/src/boundary.rs"]
mod boundary;

use boundary::BoundaryQueue;
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

// Each boundary render takes gives its own track's start, however publishing and reaching interleave.
#[test]
fn a_boundary_reached_gives_its_own_tracks_start() {
    model(|| {
        let queue = Arc::new(BoundaryQueue::<2>::new());
        let stage = {
            let queue = Arc::clone(&queue);
            thread::spawn(move || {
                assert!(queue.publish(10, 100));
                assert!(queue.publish(20, 200));
            })
        };
        let mut taken = 0;
        while taken < 2 {
            match queue.reach(25) {
                Some((count, at, start)) => {
                    assert_eq!(start, at * 10);
                    taken += count;
                }
                None => thread::yield_now(),
            }
        }
        stage.join().unwrap();
        assert_eq!(taken, 2);
    });
}

// A boundary past what render has sent stays for later, and those before it are taken together.
#[test]
fn render_takes_only_the_boundaries_it_has_sent() {
    model(|| {
        let queue = Arc::new(BoundaryQueue::<2>::new());
        let stage = {
            let queue = Arc::clone(&queue);
            thread::spawn(move || {
                assert!(queue.publish(10, 100));
                assert!(queue.publish(30, 300));
            })
        };
        let first = queue.reach(15);
        stage.join().unwrap();
        assert!(first.is_none() || first == Some((1, 10, 100)));
        let rest = queue.reach(40);
        assert_eq!(
            rest.map(|(count, _, _)| count),
            Some(if first.is_some() { 1 } else { 2 })
        );
        assert_eq!(rest.map(|(_, at, start)| (at, start)), Some((30, 300)));
    });
}

// A full queue takes no more until render has taken the oldest; nothing is lost or overwritten.
#[test]
fn a_full_queue_waits_for_render() {
    model(|| {
        let queue = Arc::new(BoundaryQueue::<2>::new());
        let stage = {
            let queue = Arc::clone(&queue);
            thread::spawn(move || {
                for at in [10, 20, 30] {
                    while !queue.publish(at, at * 10) {
                        thread::yield_now();
                    }
                }
            })
        };
        let mut taken = 0;
        let mut last = 0;
        while taken < 3 {
            match queue.reach(u64::MAX) {
                Some((count, at, start)) => {
                    assert_eq!(start, at * 10);
                    assert!(at > last);
                    last = at;
                    taken += count;
                }
                None => thread::yield_now(),
            }
        }
        stage.join().unwrap();
        assert_eq!((taken, last), (3, 30));
    });
}

// A reset drops what waits, with render drained; what is published after it is all render takes.
#[test]
fn a_reset_drops_what_waits() {
    model(|| {
        let queue = Arc::new(BoundaryQueue::<2>::new());
        assert!(queue.publish(10, 100));
        assert!(queue.publish(20, 200));
        assert!(queue.full());
        queue.clear();
        assert_eq!(queue.reached(), queue.published());
        assert!(!queue.full());
        let stage = {
            let queue = Arc::clone(&queue);
            thread::spawn(move || assert!(queue.publish(5, 50)))
        };
        let taken = loop {
            if let Some(taken) = queue.reach(u64::MAX) {
                break taken;
            }
            thread::yield_now();
        };
        stage.join().unwrap();
        assert_eq!(taken, (1, 5, 50));
        assert!(queue.reach(u64::MAX).is_none());
    });
}
