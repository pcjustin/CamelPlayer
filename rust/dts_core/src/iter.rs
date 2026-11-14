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

use crate::header::{detect_sync, parse_frame_header};
use crate::{Error, Result, header::DtsFrameHeader, header::SyncWordEncoding};

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

/// One step of a [`FrameIterator`] over a raw-16-bit DTS Core
/// stream.
#[derive(Debug, Clone, Copy)]
pub struct FrameView<'a> {
    /// Parsed header.
    pub header: DtsFrameHeader,
    /// Absolute byte offset of the frame's first sync byte within
    /// the input passed to [`iter_frames`].
    pub offset: usize,
    /// Byte length of the frame (from
    /// [`DtsFrameHeader::frame_size_bytes`]). Always 96..=16384.
    pub len: usize,
    /// Borrowed frame bytes (`bytes[offset..offset + len]`).
    pub data: &'a [u8],
}

impl<'a> FrameView<'a> {
    /// SUBFRAMES region of the frame: the bytes that follow the
    /// fully-decoded frame-sync header.
    ///
    /// Equivalent to `&self.data[self.header.header_byte_length()..]`.
    /// The wiki snapshot (`docs/audio/dts/wiki/DTS.wiki`) marks this
    /// region as `'''TODO'''`; subband / QMF / Huffman / VQ decoding
    /// remains gated on the §5.3.1 value tables and the §5.4 polyphase
    /// filterbank landing in `docs/`. The helper is exposed so
    /// downstream code (re-muxers, payload-CRC validators, future
    /// subframe decoders) can carve out the region without recomputing
    /// the header boundary.
    ///
    /// Always non-empty for well-formed frames: the smallest documented
    /// frame size is 96 B and the largest header window is 15 B
    /// (`crc_present == true`), so at least 81 B of SUBFRAMES region
    /// is guaranteed.
    pub fn payload(&self) -> &'a [u8] {
        &self.data[self.header.header_byte_length()..]
    }

    /// Locate and parse the frame's §5.7.1 Auxiliary Data chunk (the
    /// optional end-of-frame time stamp + dynamic downmix
    /// coefficients), returning `Ok(None)` when the frame carries no
    /// DWORD-aligned `nSYNCAUX` word. Composition of
    /// [`crate::aux_data::parse_aux_data`] over the frame's own bytes + header.
    pub fn aux_data(&self) -> crate::Result<Option<crate::aux_data::AuxData>> {
        crate::aux_data::parse_aux_data(self.data, &self.header)
    }

    /// Locate and parse the frame's §5.7.2 Rev2 Auxiliary Data Chunk
    /// (embedded-ES downmix scale + broadcast DRC / dialnorm),
    /// returning `Ok(None)` when the frame carries no DWORD-aligned
    /// `nSYNCRev2AUX` word. Composition of [`crate::rev2_aux::parse_rev2_aux`]
    /// over the frame's own bytes + header.
    pub fn rev2_aux(&self) -> crate::Result<Option<crate::rev2_aux::Rev2AuxChunk>> {
        crate::rev2_aux::parse_rev2_aux(self.data, &self.header)
    }
}

/// Iterator that walks a raw-16-bit DTS Core byte buffer frame by
/// frame.
///
/// Each [`Iterator::next`] step:
/// 1. Calls [`find_next_sync`] from the current cursor to handle any
///    leading garbage / inter-frame padding the source may have
///    introduced. The iterator does NOT assume `bytes[0]` is a sync
///    byte; it scans for one.
/// 2. Calls [`parse_frame_header`] at the located offset. A parse
///    failure (no sync within reach, truncated header, NBLKS or
///    FSIZE out of range) is returned as the next item's `Err`
///    variant; subsequent calls then return `None`.
/// 3. On success, yields a [`FrameView`] borrowing the frame's
///    bytes from the input and advances the cursor by
///    `header.frame_size_bytes` so the following step parses the
///    next sync.
///
/// The iterator only accepts raw 16-bit encodings (`RawBigEndian` /
/// `RawLittleEndian`). When [`find_next_sync`] locates a 14-bit
/// sync, the iterator yields a single [`Error::UnsupportedFourteenBit`]
/// item and then terminates. Callers walking 14-bit container
/// streams must externally partition the input into frame-sized
/// slices and feed each slice to [`crate::header::parse_frame_header_14bit`]
/// directly. See `README.md`'s round-6 docs gap #7 for the
/// container-byte-advance rule.
#[derive(Debug)]
pub struct FrameIterator<'a> {
    bytes: &'a [u8],
    cursor: usize,
    done: bool,
}

impl<'a> FrameIterator<'a> {
    /// Construct an iterator positioned at byte 0 of `bytes`.
    pub fn new(bytes: &'a [u8]) -> Self {
        FrameIterator {
            bytes,
            cursor: 0,
            done: false,
        }
    }

    /// Current cursor offset. After a successful step the cursor
    /// points at the next frame's expected sync byte (or one past
    /// end-of-buffer if the previous frame was the last). After a
    /// failure the cursor stays at the point of failure.
    pub fn cursor(&self) -> usize {
        self.cursor
    }
}

impl<'a> Iterator for FrameIterator<'a> {
    type Item = Result<FrameView<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done {
            return None;
        }
        let sync_match = find_next_sync(self.bytes, self.cursor)?;
        match sync_match.encoding {
            SyncWordEncoding::FourteenBitBigEndian | SyncWordEncoding::FourteenBitLittleEndian => {
                // 14-bit container streams need an out-of-band
                // container-byte advance rule which the wiki snapshot
                // does not enumerate. Surface the limitation and
                // terminate so the caller switches to the
                // single-frame 14-bit entry point.
                self.done = true;
                return Some(Err(Error::UnsupportedFourteenBit));
            }
            _ => {}
        }
        let off = sync_match.offset;
        let parse_result = parse_frame_header(&self.bytes[off..]);
        let hdr = match parse_result {
            Ok(h) => h,
            Err(e) => {
                self.done = true;
                self.cursor = off;
                return Some(Err(e));
            }
        };
        let len = hdr.frame_size_bytes as usize;
        if off + len > self.bytes.len() {
            // Header says the frame extends past end-of-buffer. We
            // still surface the header (the caller may want to know
            // the truncation occurred at this offset with this
            // size), but mark the iterator done.
            self.done = true;
            self.cursor = off;
            return Some(Err(Error::UnexpectedEof));
        }
        let view = FrameView {
            header: hdr,
            offset: off,
            len,
            data: &self.bytes[off..off + len],
        };
        self.cursor = off + len;
        Some(Ok(view))
    }
}

/// Convenience constructor — equivalent to [`FrameIterator::new`].
pub fn iter_frames(bytes: &[u8]) -> FrameIterator<'_> {
    FrameIterator::new(bytes)
}

// ---------------------------------------------------------------------
// Round 192 — 14-bit container-byte frame iterator (`iter_frames_14bit`)
//
// The fail-fast [`FrameIterator`] from round 6 walks raw-16-bit streams
// only; a 14-bit sync at the cursor yields `Error::UnsupportedFourteenBit`
// and terminates because the iterator's `frame_size_bytes` advance only
// makes sense in the unpacked domain. Round 189 added the analytical
// half — [`DtsFrameHeader::frame_size_container_bytes`] — which converts
// the unpacked-domain frame size to a container-domain byte count for
// the 14-bit encodings. Round 192 wires that accessor into a dedicated
// 14-bit frame iterator: each step calls [`parse_frame_header_14bit`]
// at the matched container offset (which internally unpacks just enough
// containers to read the 13/15-byte unpacked header window), then steps
// the cursor by `frame_size_container_bytes(encoding)` container bytes.
//
// We deliberately introduce a separate [`FrameView14`] type rather than
// reusing [`FrameView`] because the semantics of `len` differ: in the
// 14-bit case `len` is a container-byte advance derived from the
// 14-bit `frame_size_container_bytes` formula (not the raw
// `header.frame_size_bytes` field), and `data` borrows the
// container-byte window — not the unpacked logical bytes. Sharing the
// `FrameView` type would require silently overloading `len`'s meaning,
// which is exactly the kind of footgun the wiki/`docs/` ambiguity asks
// us to avoid.
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::header::FrameType;

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

    // ---------------------------------------------------------------
    // Round 138 — FrameView::payload()
    // ---------------------------------------------------------------

    /// Hand-build a 96-byte raw-BE termination frame (NBLKS=5,
    /// FSIZE=95, crc_present=0) padded with zeros for the SUBFRAMES
    /// region; then confirm `FrameView::payload()` returns exactly
    /// `frame.len - header_byte_length()` bytes (= 96 - 13 = 82).
    #[test]
    fn frame_view_payload_slice_length_matches_header_boundary_no_crc() {
        // The base 13-byte header window is followed by 82 bytes of
        // SUBFRAMES region we fill with a distinctive 0xCD pattern
        // so the slice content can be checked too.
        let mut buf = vec![0u8; 96];
        // Sync.
        buf[0..4].copy_from_slice(&[0x7F, 0xFE, 0x80, 0x01]);
        // FTYPE=0 (termination, MSB), SHORT=0, CRC_PRESENT=0,
        // NBLKS=5 (7 bits), FSIZE-1=94 (14 bits = frame_size 95),
        // AMODE=0 (6 bits), SFREQ=0 (4 bits), RATE=0 (5 bits) +
        // 13 zero trailing bits + 16 zero post-CRC bits = 75 bits
        // after the sync = 13 bytes total.
        // Easier: just call build_be_header equivalent through
        // parse_frame_header on a synthesised buffer.
        // Bytes layout (after sync):
        //  byte 4: FTYPE(1)=0 SHORT(5)=0 CRC_PRESENT(1)=0 NBLKS_hi(1)=0 -> 0b00000000 = 0x00
        //  byte 5: NBLKS_lo(6)=000101 FSIZE_hi(2)=00 -> 0b00010100 = 0x14
        //  byte 6: FSIZE_mid(8)=00010111 -> 0x17  (FSIZE-1 = 94 = 0b00_00000101_1110, hi 2 bits=00, mid 8=00010111? — recompute below)
        // Rather than hand-bit-fiddle, sidestep the layout: use
        // `parse_frame_header` on a 95-byte buffer we synthesise via
        // the header.rs test helper indirectly by re-deriving the
        // bit layout here. Simpler: directly construct via a tiny
        // local builder.
        fn push(bv: &mut Vec<bool>, value: u32, width: u32) {
            for i in (0..width).rev() {
                bv.push(((value >> i) & 1) == 1);
            }
        }
        let mut bv: Vec<bool> = Vec::new();
        push(&mut bv, 0x7FFE_8001, 32);
        push(&mut bv, 0, 1); // ftype = termination
        push(&mut bv, 0, 5); // sample_count_m1
        push(&mut bv, 0, 1); // crc_present
        push(&mut bv, 5, 7); // nblks
        push(&mut bv, 95, 14); // fsize-1 = 95 -> frame_size = 96
        push(&mut bv, 0, 6); // amode
        push(&mut bv, 0, 4); // sfreq
        push(&mut bv, 0, 5); // rate
        push(&mut bv, 0, 13); // trailing flags
        push(&mut bv, 0, 16); // post-CRC window
        while !bv.len().is_multiple_of(8) {
            bv.push(false);
        }
        // Convert bit-vector to bytes (MSB-first).
        for (i, chunk) in bv.chunks(8).enumerate() {
            let mut b: u8 = 0;
            for (k, bit) in chunk.iter().enumerate() {
                if *bit {
                    b |= 1 << (7 - k);
                }
            }
            buf[i] = b;
        }
        // Fill the SUBFRAMES region (bytes 13..96) with a pattern so
        // the payload-slice contents can be verified.
        for byte in buf.iter_mut().skip(13) {
            *byte = 0xCD;
        }

        let mut it = iter_frames(&buf);
        let view = it.next().expect("frame must yield").expect("must parse");
        assert_eq!(view.offset, 0);
        assert_eq!(view.len, 96);
        assert_eq!(view.header.frame_size_bytes, 96);
        assert!(!view.header.crc_present);
        assert_eq!(view.header.header_byte_length(), 13);

        let payload = view.payload();
        assert_eq!(payload.len(), 96 - 13);
        assert!(payload.iter().all(|&b| b == 0xCD));
        // No more frames in the buffer.
        assert!(it.next().is_none());
    }

    /// `FrameView::payload()` shifts by 2 bytes (15 vs 13) when
    /// `crc_present == 1` because the optional HEADER_CRC slot
    /// extends the header window from 104 to 120 bits.
    #[test]
    fn frame_view_payload_offset_shifts_when_crc_present() {
        // Build a 96-byte crc-present termination frame.
        let mut buf = vec![0u8; 96];
        fn push(bv: &mut Vec<bool>, value: u32, width: u32) {
            for i in (0..width).rev() {
                bv.push(((value >> i) & 1) == 1);
            }
        }
        let mut bv: Vec<bool> = Vec::new();
        push(&mut bv, 0x7FFE_8001, 32);
        push(&mut bv, 0, 1);
        push(&mut bv, 0, 5);
        push(&mut bv, 1, 1); // crc_present = true
        push(&mut bv, 5, 7);
        push(&mut bv, 95, 14);
        push(&mut bv, 0, 6);
        push(&mut bv, 0, 4);
        push(&mut bv, 0, 5);
        push(&mut bv, 0, 13);
        push(&mut bv, 0xABCD, 16); // header_crc
        push(&mut bv, 0, 16); // post-CRC window
        while !bv.len().is_multiple_of(8) {
            bv.push(false);
        }
        for (i, chunk) in bv.chunks(8).enumerate() {
            let mut b: u8 = 0;
            for (k, bit) in chunk.iter().enumerate() {
                if *bit {
                    b |= 1 << (7 - k);
                }
            }
            buf[i] = b;
        }
        // Fill bytes 15..96 with a distinct pattern.
        for byte in buf.iter_mut().skip(15) {
            *byte = 0xEF;
        }

        let view = iter_frames(&buf).next().unwrap().unwrap();
        assert!(view.header.crc_present);
        assert_eq!(view.header.header_byte_length(), 15);
        let payload = view.payload();
        assert_eq!(payload.len(), 96 - 15);
        assert!(payload.iter().all(|&b| b == 0xEF));
    }

    // ---------------------------------------------------------------
    // Round 151 — find_all_syncs() bulk-scan helper.
    //
    // Counterpart to find_next_sync() that returns every sync match in
    // a buffer, useful for stream-integrity tooling that needs to know
    // about every resync point up front rather than walking one at a
    // time.
    // ---------------------------------------------------------------

    // ---------------------------------------------------------------
    // Round 151 — iter_frames coverage for raw-LE streams.
    //
    // The iterator's `frame_size_bytes`-based advance is documented as
    // applying to "raw 16-bit encodings" (both RawBigEndian AND
    // RawLittleEndian — the FSIZE field is the byte length of the
    // raw 16-bit-per-word on-wire stream, which is byte-equivalent
    // between BE and LE because the LE form is just a pairwise
    // byte-swap of the BE form). The existing
    // `multi_frame_iter.rs::iter_frames_walks_all_five_frames_in_fixture`
    // test exercises the BE path against the ffmpeg fixture; the
    // raw-LE path is exercised here by byte-swapping that fixture
    // (synthesised inline so the iterator's LE walk is covered without
    // adding a new fixture file).
    // ---------------------------------------------------------------

    /// Build a multi-frame raw-LE byte buffer by byte-swapping a
    /// raw-BE frame buffer pairwise. The frame layout (count, sizes,
    /// SUBFRAMES contents) is preserved because raw-LE is defined by
    /// the wiki as the 16-bit-word-swapped form of raw-BE.
    fn build_raw_le_two_frame_stream() -> Vec<u8> {
        // Two back-to-back 96-byte termination frames (NBLKS=5,
        // FSIZE=96). We hand-build the BE form via the same bit-
        // table the parser consumes and then word-swap to LE.
        let mut be = Vec::with_capacity(2 * 96);
        for _frame in 0..2 {
            let mut bv: Vec<bool> = Vec::new();
            fn push(bv: &mut Vec<bool>, value: u32, width: u32) {
                for i in (0..width).rev() {
                    bv.push(((value >> i) & 1) == 1);
                }
            }
            push(&mut bv, 0x7FFE_8001, 32);
            push(&mut bv, 0, 1); // ftype = termination
            push(&mut bv, 0, 5); // sample_count_m1
            push(&mut bv, 0, 1); // crc_present
            push(&mut bv, 5, 7); // nblks
            push(&mut bv, 95, 14); // fsize-1 = 95 -> frame_size = 96
            push(&mut bv, 0, 6); // amode
            push(&mut bv, 0, 4); // sfreq
            push(&mut bv, 0, 5); // rate
            push(&mut bv, 0, 13); // trailing flags
            push(&mut bv, 0, 16); // post-CRC window
            while !bv.len().is_multiple_of(8) {
                bv.push(false);
            }
            // Convert MSB-first bit-vector to bytes.
            let mut frame_bytes = vec![0u8; 96];
            for (i, chunk) in bv.chunks(8).enumerate() {
                let mut b: u8 = 0;
                for (k, bit) in chunk.iter().enumerate() {
                    if *bit {
                        b |= 1 << (7 - k);
                    }
                }
                frame_bytes[i] = b;
            }
            // Fill bytes 13..96 (SUBFRAMES region) with a distinct
            // per-frame pattern so payload checks can differentiate.
            for byte in frame_bytes.iter_mut().skip(13) {
                *byte = 0xCD;
            }
            be.extend_from_slice(&frame_bytes);
        }
        // Word-swap each 16-bit pair to produce the raw-LE form.
        for pair in be.as_chunks_mut::<2>().0 {
            pair.swap(0, 1);
        }
        be
    }

    #[test]
    fn iter_frames_walks_raw_le_two_frame_stream() {
        let buf = build_raw_le_two_frame_stream();
        // Sanity: starts with the canonical raw-LE sync.
        assert_eq!(&buf[..4], &[0xFE, 0x7F, 0x01, 0x80]);
        // Second frame's sync at offset 96.
        assert_eq!(&buf[96..100], &[0xFE, 0x7F, 0x01, 0x80]);

        let frames: Vec<_> = iter_frames(&buf)
            .collect::<core::result::Result<Vec<_>, _>>()
            .expect("raw-LE multi-frame stream must walk");
        assert_eq!(frames.len(), 2);

        for (i, frame) in frames.iter().enumerate() {
            assert_eq!(
                frame.header.sync_word_encoding,
                SyncWordEncoding::RawLittleEndian,
                "frame {i} encoding",
            );
            assert_eq!(frame.offset, i * 96);
            assert_eq!(frame.len, 96);
            assert_eq!(frame.header.frame_size_bytes, 96);
            assert_eq!(frame.header.blocks_per_frame, 5);
            assert_eq!(frame.header.frame_type, FrameType::Termination);
            assert!(!frame.header.crc_present);
            // The byte-length accessor returns the unpacked-bitstream
            // (raw-BE) header byte count regardless of the on-wire
            // sync encoding — 13 bytes when `crc_present == 0`.
            assert_eq!(frame.header.header_byte_length(), 13);
        }
    }

    /// The raw-LE walk is robust to leading garbage in the same way
    /// the raw-BE walk is — `find_next_sync` skips past unrelated
    /// bytes and lands on the first LE sync.
    #[test]
    fn iter_frames_raw_le_handles_leading_garbage() {
        let stream = build_raw_le_two_frame_stream();
        let mut prefixed = Vec::with_capacity(7 + stream.len());
        prefixed.extend_from_slice(&[0xAA; 7]);
        prefixed.extend_from_slice(&stream);

        let frames: Vec<_> = iter_frames(&prefixed)
            .collect::<core::result::Result<Vec<_>, _>>()
            .expect("garbage-prefixed raw-LE stream must still walk");
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].offset, 7);
        assert_eq!(frames[1].offset, 7 + 96);
        for f in &frames {
            assert_eq!(
                f.header.sync_word_encoding,
                SyncWordEncoding::RawLittleEndian
            );
        }
    }

    // -----------------------------------------------------------------
    // Round 159 — FrameIteratorResync.
    //
    // Error-tolerant counterpart to FrameIterator: a candidate sync
    // whose subsequent header bits fail the structural NBLKS / FSIZE
    // bounds (or whose declared frame_size_bytes overruns
    // end-of-buffer) is treated as a false-positive sync rather than a
    // hard fault; the iterator yields a ResyncEvent and continues
    // scanning from offset + 1. Useful for walking partially-corrupted
    // streams past malformed-sync patches.
    // -----------------------------------------------------------------

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

    // ---------------------------------------------------------------
    // Round 192 — iter_frames_14bit (14-bit container-stream walker)
    //
    // Each test builds a raw-BE frame (96 bytes, the minimum FSIZE+1
    // the spec allows), packs it through `pack_16bit_to_14bit` into a
    // 14-bit-packed container buffer, and exercises the iterator
    // against the resulting bytes. The container-byte advance is
    // verified to equal `frame_size_container_bytes(encoding)`
    // (= 110 bytes for FSIZE+1 = 96: ceil(96 * 8 / 14) * 2 = 110).
    // ---------------------------------------------------------------
}
