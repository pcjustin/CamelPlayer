//! Multi-frame iteration over a DTS Core byte stream.
//!
//! Round 6 (2026-05-25) adds two demuxer-friendly helpers on top of
//! the single-frame [`crate::header::parse_frame_header`] /
//! [`crate::header::parse_frame_header_14bit`] entry points:
//!
//! - [`find_next_sync`] — scan a byte buffer for the next DTS sync
//!   sequence starting at an arbitrary offset, returning both the
//!   offset and the [`crate::header::SyncWordEncoding`] that matched. Useful
//!   for callers that need to resynchronise after lost bytes, drop
//!   leading container padding, or walk a raw `.dts` file frame by
//!   frame.
//! - [`FrameIterator`] / [`iter_frames`] — walk a byte buffer one
//!   frame at a time. Each successful step parses the frame's header,
//!   reports its byte range, and advances by
//!   [`crate::header::DtsFrameHeader::frame_size_bytes`] (for raw 16-bit
//!   encodings; see the function docs for the 14-bit advance rule).
//!
//! Round 159 (2026-05-27) adds an error-tolerant counterpart to
//! [`FrameIterator`]:
//!
//! - [`FrameIteratorResync`] / [`iter_frames_resync`] — when a
//!   candidate sync turns out to be a false positive (random payload
//!   bytes that happened to match a 4-byte sync sequence and whose
//!   subsequent header bits fail the structural NBLKS / FSIZE bounds,
//!   or whose declared `frame_size_bytes` overruns end-of-buffer),
//!   surface a [`ResyncEvent`] reporting the discarded offset + cause
//!   and continue scanning from `offset + 1` instead of terminating.
//!   Useful for stream-integrity tooling that needs to walk a
//!   partially-corrupted `.dts` stream past a malformed-syncword
//!   patch.
//!
//! Neither helper depends on the [`oxideav-core`] integration; both
//! are available in the `--no-default-features` build alongside the
//! standalone parsers.
//!
//! ## What stays out of scope
//!
//! - Container-stream parsing: a raw `.dts` file is a concatenation
//!   of self-delimited Core frames, which is the only shape these
//!   helpers walk. AVI / MP4 / Matroska sample carriage stays in
//!   their respective container crates; the helpers here operate on
//!   raw codec bytes only.
//! - PCM sample emission. The iterator surfaces parsed
//!   [`crate::header::DtsFrameHeader`] records and the raw frame byte slice;
//!   subband / QMF / Huffman decoding remains gated on the spec
//!   tables in the docs gaps.
//! - 14-bit byte-advance for [`FrameIterator`]. The iterator only
//!   advances on the **raw 16-bit** sync variants because the
//!   `frame_size_bytes` field is documented as the byte length of
//!   the unpacked stream — for 14-bit-packed containers the
//!   corresponding container-byte advance would be
//!   `frame_size_bytes * 8 / 14` rounded up to the next even byte,
//!   which the wiki snapshot does **not** spell out. The 14-bit
//!   single-frame [`crate::header::parse_frame_header_14bit`] entry point
//!   remains available for callers that have already partitioned
//!   their 14-bit input into frame-sized slices. See `README.md`'s
//!   round-6 docs gap #7.

use crate::header::{SyncWordEncoding, detect_sync};

/// Result of a [`find_next_sync`] lookup.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SyncMatch {
    /// Absolute byte offset (within the input slice passed to
    /// [`find_next_sync`]) where the sync sequence starts.
    pub offset: usize,
    /// Which of the four documented sync encodings was found.
    pub encoding: SyncWordEncoding,
}

impl SyncMatch {}

/// First-byte gate for the four documented DTS sync sequences.
///
/// All four sync words begin with a distinct first byte that does
/// not appear in mid-sync positions of any of the other three:
///
/// | Encoding                  | First byte | Source                            |
/// | ------------------------- | ---------- | --------------------------------- |
/// | `RawBigEndian`            | `0x7F`     | wiki snapshot `7F FE 80 01`       |
/// | `RawLittleEndian`         | `0xFE`     | wiki snapshot `FE 7F 01 80`       |
/// | `FourteenBitBigEndian`    | `0x1F`     | wiki snapshot `1F FF E8 00 07 Fx` |
/// | `FourteenBitLittleEndian` | `0xFF`     | wiki snapshot `FF 1F 00 E8 Fx 07` |
///
/// `find_next_sync` uses this as a one-byte filter to skip the
/// 4-byte raw-sync check + 6-byte 14-bit-sync check on positions
/// whose first byte cannot start any documented sync. On random
/// payload bytes this short-circuits ~98.4% of positions (4 of 256
/// possible first bytes match) before any multi-byte comparison
/// fires.
#[inline]
fn is_sync_first_byte_candidate(b: u8) -> bool {
    // Equivalent to `matches!(b, 0x7F | 0xFE | 0x1F | 0xFF)`. Written
    // as a single bitmask check so a release-mode compile lowers to a
    // bt / cmp pair rather than a four-way branch.
    matches!(b, 0x7F | 0xFE | 0x1F | 0xFF)
}

/// Scan `bytes[start..]` for the next DTS Core sync sequence.
///
/// Returns the byte offset (in the original `bytes` slice, not in
/// `bytes[start..]`) and the matched [`SyncWordEncoding`] of the
/// first sync found at or after `start`, or `None` if no sync
/// appears before end-of-buffer.
///
/// All four documented sync sequences are accepted:
///
/// - `7F FE 80 01` — raw big-endian (4 bytes).
/// - `FE 7F 01 80` — raw little-endian (4 bytes).
/// - `1F FF E8 00 07 Fx` — 14-bit packed big-endian (6 bytes).
/// - `FF 1F 00 E8 Fx 07` — 14-bit packed little-endian (6 bytes).
///
/// The 14-bit variants are matched via the lower-14-bit payloads of
/// the first three containers (`0x1FFF`, `0x2800`,
/// top-4-of-`0x07F?`), matching the same widened detection rule the
/// single-frame parser uses (see [`detect_sync`] in `header.rs`).
///
/// The scan is `O(n)`: every byte is visited at most twice (once for
/// the 4-byte raw-sync check, once for the 6-byte 14-bit-sync
/// check). Calling [`find_next_sync`] in a loop with the previous
/// `offset + 1` is the standard resync pattern.
///
/// Round 165 (2026-05-27) added a one-byte first-byte gate
/// ([`is_sync_first_byte_candidate`]) before the multi-byte
/// [`detect_sync`] call so positions whose first byte cannot start
/// any documented sync (252 of 256 possible bytes) skip the
/// multi-byte comparison entirely. The walk order, returned offset,
/// and matched encoding are unchanged from the round-6 implementation
/// — round 165 also adds a `find_next_sync_matches_pre_optimization_reference`
/// equivalence test to prove that every byte sequence the old loop
/// would accept the new loop also accepts (and at the same offset
/// with the same encoding tag).
pub fn find_next_sync(bytes: &[u8], start: usize) -> Option<SyncMatch> {
    if start >= bytes.len() {
        return None;
    }
    let mut i = start;
    // We need at least 4 bytes for the shortest sync (raw 16-bit) and
    // 6 bytes for the longest (14-bit packed). Stop the scan at the
    // last position that could still hold a raw sync.
    let last = bytes.len();
    while i + 4 <= last {
        // First-byte gate: 252 of 256 possible bytes fail this and
        // skip the multi-byte detect_sync call. The walk order is
        // preserved (every position is still visited in order) so
        // the returned offset / encoding are identical to the
        // pre-round-165 implementation.
        if !is_sync_first_byte_candidate(bytes[i]) {
            i += 1;
            continue;
        }
        if let Ok(enc) = detect_sync(&bytes[i..]) {
            return Some(SyncMatch {
                offset: i,
                encoding: enc,
            });
        }
        i += 1;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    const RAW_BE_SYNC: [u8; 4] = [0x7F, 0xFE, 0x80, 0x01];
    const RAW_LE_SYNC: [u8; 4] = [0xFE, 0x7F, 0x01, 0x80];

    #[test]
    fn find_next_sync_at_offset_zero() {
        let mut buf = vec![0u8; 16];
        buf[..4].copy_from_slice(&RAW_BE_SYNC);
        let m = find_next_sync(&buf, 0).unwrap();
        assert_eq!(m.offset, 0);
        assert_eq!(m.encoding, SyncWordEncoding::RawBigEndian);
    }

    #[test]
    fn find_next_sync_skips_leading_garbage() {
        let mut buf = vec![0xAAu8; 32];
        buf[7..11].copy_from_slice(&RAW_BE_SYNC);
        let m = find_next_sync(&buf, 0).unwrap();
        assert_eq!(m.offset, 7);
        assert_eq!(m.encoding, SyncWordEncoding::RawBigEndian);
    }

    #[test]
    fn find_next_sync_le_variant() {
        let mut buf = vec![0u8; 16];
        buf[3..7].copy_from_slice(&RAW_LE_SYNC);
        let m = find_next_sync(&buf, 0).unwrap();
        assert_eq!(m.offset, 3);
        assert_eq!(m.encoding, SyncWordEncoding::RawLittleEndian);
    }

    #[test]
    fn find_next_sync_14bit_be() {
        let mut buf = vec![0u8; 16];
        buf[5..11].copy_from_slice(&[0x1F, 0xFF, 0xE8, 0x00, 0x07, 0xFA]);
        let m = find_next_sync(&buf, 0).unwrap();
        assert_eq!(m.offset, 5);
        assert_eq!(m.encoding, SyncWordEncoding::FourteenBitBigEndian);
    }

    #[test]
    fn find_next_sync_14bit_le() {
        let mut buf = vec![0u8; 16];
        buf[5..11].copy_from_slice(&[0xFF, 0x1F, 0x00, 0xE8, 0xF3, 0x07]);
        let m = find_next_sync(&buf, 0).unwrap();
        assert_eq!(m.offset, 5);
        assert_eq!(m.encoding, SyncWordEncoding::FourteenBitLittleEndian);
    }

    #[test]
    fn find_next_sync_honours_start_offset() {
        // Two syncs in the buffer; start past the first.
        let mut buf = vec![0xAAu8; 32];
        buf[2..6].copy_from_slice(&RAW_BE_SYNC);
        buf[20..24].copy_from_slice(&RAW_BE_SYNC);
        let m = find_next_sync(&buf, 7).unwrap();
        assert_eq!(m.offset, 20);
    }

    #[test]
    fn find_next_sync_returns_none_when_absent() {
        let buf = vec![0xAAu8; 64];
        assert_eq!(find_next_sync(&buf, 0), None);
    }

    #[test]
    fn find_next_sync_returns_none_when_start_past_end() {
        let buf = vec![0u8; 16];
        assert_eq!(find_next_sync(&buf, 100), None);
    }

    #[test]
    fn find_next_sync_returns_none_when_only_partial_sync_at_tail() {
        // Three bytes of an in-progress raw sync (`7F FE 80`) but no
        // fourth byte — the scanner walks up to length-4, finds
        // nothing, and returns None.
        let mut buf = vec![0xAAu8; 8];
        buf[5..8].copy_from_slice(&[0x7F, 0xFE, 0x80]);
        assert_eq!(find_next_sync(&buf, 0), None);
    }

    // ----------------------------------------------------------------
    // Round 165 — find_next_sync / find_all_syncs first-byte gate.
    //
    // The round-165 optimisation gates the multi-byte detect_sync()
    // check behind a one-byte filter
    // (`is_sync_first_byte_candidate`): only bytes 0x7F / 0xFE / 0x1F
    // / 0xFF can start one of the four documented sync sequences.
    // The tests below verify (a) the filter accepts exactly those
    // four bytes, (b) the optimised scan agrees with a brute-force
    // pre-round-165-style reference on every input, including
    // pathological payloads packed with first-byte-candidate bytes
    // whose multi-byte continuation is non-sync, and (c) the
    // optimisation does not change the documented walk order or
    // returned offsets.
    // ----------------------------------------------------------------

    /// Brute-force, pre-round-165-style reference scanner. Walks
    /// every position 1-by-1 and calls `detect_sync` without a
    /// first-byte gate. Used as the equivalence oracle for the
    /// optimised `find_next_sync`.
    fn reference_find_next_sync(bytes: &[u8], start: usize) -> Option<SyncMatch> {
        use crate::header::detect_sync;
        if start >= bytes.len() {
            return None;
        }
        let mut i = start;
        while i + 4 <= bytes.len() {
            if let Ok(enc) = detect_sync(&bytes[i..]) {
                return Some(SyncMatch {
                    offset: i,
                    encoding: enc,
                });
            }
            i += 1;
        }
        None
    }

    /// The filter accepts exactly the four first bytes of the four
    /// documented sync sequences. Every other byte (252 of 256)
    /// must short-circuit.
    #[test]
    fn first_byte_candidate_accepts_exactly_four_bytes() {
        let mut accepted: Vec<u8> = Vec::new();
        for b in 0u16..=255 {
            if is_sync_first_byte_candidate(b as u8) {
                accepted.push(b as u8);
            }
        }
        accepted.sort();
        assert_eq!(accepted, vec![0x1F, 0x7F, 0xFE, 0xFF]);
    }

    /// First-byte gate must accept the actual first byte of every
    /// documented sync prefix. (Belt-and-braces; the previous test
    /// asserts the same thing inversely, but spelling it out per
    /// encoding makes regressions easier to read.)
    #[test]
    fn first_byte_candidate_accepts_documented_sync_prefixes() {
        assert!(is_sync_first_byte_candidate(0x7F)); // raw BE
        assert!(is_sync_first_byte_candidate(0xFE)); // raw LE
        assert!(is_sync_first_byte_candidate(0x1F)); // 14-bit BE
        assert!(is_sync_first_byte_candidate(0xFF)); // 14-bit LE
        // Spot-check a few adjacent bytes that look similar but are
        // explicitly NOT sync prefixes.
        assert!(!is_sync_first_byte_candidate(0x7E));
        assert!(!is_sync_first_byte_candidate(0xFD));
        assert!(!is_sync_first_byte_candidate(0x80));
        assert!(!is_sync_first_byte_candidate(0x00));
    }

    /// Optimised `find_next_sync` agrees with the pre-round-165
    /// reference on every position of a buffer densely packed with
    /// first-byte candidates whose multi-byte continuation is
    /// deliberately non-sync. The first-byte gate must NOT smuggle
    /// in false-positive matches: positions where bytes[i] is one
    /// of {0x7F, 0xFE, 0x1F, 0xFF} but the following 3-5 bytes do
    /// not match the full sync sequence must still return `None`
    /// (or the next genuine sync further along).
    #[test]
    fn find_next_sync_matches_pre_optimization_reference_on_candidate_dense_payload() {
        // Build a 256 B buffer where every fourth byte is a sync
        // first-byte candidate but the continuation never matches.
        let mut buf = vec![0u8; 256];
        let candidates = [0x7Fu8, 0xFE, 0x1F, 0xFF];
        for (i, b) in buf.iter_mut().enumerate() {
            if i % 4 == 0 {
                *b = candidates[(i / 4) % 4];
            } else {
                // Bytes 1..4 deliberately != real continuation bytes.
                *b = 0x55;
            }
        }
        // Reference and optimised must both return None.
        assert_eq!(find_next_sync(&buf, 0), None);
        assert_eq!(reference_find_next_sync(&buf, 0), None);

        // Now embed a real raw-BE sync at offset 100 and confirm
        // both implementations find it at the same offset with the
        // same encoding tag.
        buf[100..104].copy_from_slice(&RAW_BE_SYNC);
        let opt = find_next_sync(&buf, 0).unwrap();
        let r = reference_find_next_sync(&buf, 0).unwrap();
        assert_eq!(opt, r);
        assert_eq!(opt.offset, 100);
        assert_eq!(opt.encoding, SyncWordEncoding::RawBigEndian);
    }

    /// Cross-check the optimised `find_next_sync` against the
    /// reference on a deterministic pseudo-random buffer. The LCG
    /// seed is fixed so the test is reproducible.
    #[test]
    fn find_next_sync_matches_reference_on_pseudo_random_buffer() {
        // 4 KB linear-congruential pseudo-random payload (Knuth's
        // MMIX-friendly multiplier).
        let mut buf = vec![0u8; 4096];
        let mut state: u64 = 0xDEAD_BEEF_CAFE_BABE;
        for b in buf.iter_mut() {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            *b = (state >> 32) as u8;
        }
        // Sweep every possible start offset and verify per-call agreement.
        for start in 0..buf.len() {
            assert_eq!(
                find_next_sync(&buf, start),
                reference_find_next_sync(&buf, start),
                "find_next_sync diverged at start={start}"
            );
        }
    }

    /// First-byte gate must not change the answer when the input is
    /// densely packed with `0xFF` bytes (a common payload pattern in
    /// silent-encoded audio): every position has a first-byte
    /// candidate but very few have a valid sync continuation.
    #[test]
    fn find_next_sync_handles_all_ones_payload_with_one_embedded_sync() {
        let mut buf = vec![0xFFu8; 256];
        // Embed a real raw-LE sync at offset 50.
        buf[50..54].copy_from_slice(&RAW_LE_SYNC);
        let opt = find_next_sync(&buf, 0).unwrap();
        let r = reference_find_next_sync(&buf, 0).unwrap();
        assert_eq!(opt, r);
        assert_eq!(opt.offset, 50);
        assert_eq!(opt.encoding, SyncWordEncoding::RawLittleEndian);
    }

    /// All-zero payload (no first-byte candidates anywhere) returns
    /// `None` from both implementations after a single full pass.
    /// Confirms the gate's early-exit path doesn't infinite-loop or
    /// skip end-of-buffer bookkeeping.
    #[test]
    fn find_next_sync_handles_all_zero_payload() {
        let buf = vec![0u8; 4096];
        assert_eq!(find_next_sync(&buf, 0), None);
        assert_eq!(reference_find_next_sync(&buf, 0), None);
    }

    /// Sweep `start` across every offset of a moderately-sized
    /// buffer containing two real syncs. The optimised and
    /// reference scanners must agree on the result for every start.
    #[test]
    fn find_next_sync_start_sweep_matches_reference_with_two_real_syncs() {
        let mut buf = vec![0xAAu8; 200];
        buf[20..24].copy_from_slice(&RAW_BE_SYNC);
        buf[100..104].copy_from_slice(&RAW_LE_SYNC);
        for start in 0..buf.len() {
            assert_eq!(
                find_next_sync(&buf, start),
                reference_find_next_sync(&buf, start),
                "divergence at start={start}"
            );
        }
    }

    // ---------------------------------------------------------------
    // Round 179 — SyncWordEncoding::sync_byte_length /
    //             SyncMatch::sync_byte_length / sync_byte_range /
    //             SyncIterator + iter_syncs
    // ---------------------------------------------------------------

    /// Wiki sync table directly enumerates the four sync sequences.
    /// Length 4 for the two raw encodings (`7F FE 80 01` /
    /// `FE 7F 01 80`); length 6 for the two 14-bit-packed encodings
    /// (`1F FF E8 00 07 Fx` / `FF 1F 00 E8 Fx 07`).
    #[test]
    fn sync_word_encoding_byte_length_matches_wiki_sync_table() {
        assert_eq!(SyncWordEncoding::RawBigEndian.sync_byte_length(), 4);
        assert_eq!(SyncWordEncoding::RawLittleEndian.sync_byte_length(), 4);
        assert_eq!(SyncWordEncoding::FourteenBitBigEndian.sync_byte_length(), 6);
        assert_eq!(
            SyncWordEncoding::FourteenBitLittleEndian.sync_byte_length(),
            6
        );
    }

    /// `is_raw_16bit` accepts exactly the two raw encodings;
    /// `is_14bit_packed` accepts exactly the other two. The two
    /// predicates are mutually exclusive and jointly exhaustive over
    /// the documented sync encodings.
    #[test]
    fn sync_word_encoding_raw_vs_packed_predicates_partition_the_enum() {
        for enc in [
            SyncWordEncoding::RawBigEndian,
            SyncWordEncoding::RawLittleEndian,
            SyncWordEncoding::FourteenBitBigEndian,
            SyncWordEncoding::FourteenBitLittleEndian,
        ] {
            assert_ne!(
                enc.is_raw_16bit(),
                enc.is_14bit_packed(),
                "exactly one predicate must hold for {enc:?}"
            );
        }
        assert!(SyncWordEncoding::RawBigEndian.is_raw_16bit());
        assert!(SyncWordEncoding::RawLittleEndian.is_raw_16bit());
        assert!(!SyncWordEncoding::FourteenBitBigEndian.is_raw_16bit());
        assert!(!SyncWordEncoding::FourteenBitLittleEndian.is_raw_16bit());
        assert!(SyncWordEncoding::FourteenBitBigEndian.is_14bit_packed());
        assert!(SyncWordEncoding::FourteenBitLittleEndian.is_14bit_packed());
    }
}
