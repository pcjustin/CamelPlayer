//! §D.10 Vector-Quantization code books — the wire facts (dimensions,
//! index widths, entry packing, element scaling), the structural
//! scanner for the §5.5 phase-1 high-frequency VQ indices, and — since
//! round 439 — the **books themselves**, built in
//! ([`VqCodebooks::builtin`]).
//!
//! ETSI TS 102 114 defines both books normatively but omits their
//! numeric contents, once for each of §D.10.1 and §D.10.2: "Due to
//! its extensive size, this table is not included here" (PDF p.255).
//! For a long time that was this crate's recorded docs gap
//! (`docs/audio/dts/dts-d10-vq-tables-GAP.md`, now **CLOSED**): the
//! GAP doc's container-level forensics proved the values were never in
//! the PDF in any form. The books are now staged as clean-room *data*
//! under `docs/audio/dts/tables/` — `dts-d10-1-adpcm-coeff-vq.csv`
//! (4096 × 4) and `dts-d10-2-hfreq-vq.csv` (1024 × 32), each with a
//! `.meta.md` sidecar; chain of custody in
//! `docs/audio/dts/provenance/11-extractor-d10-vq.md`, with two
//! independent sources agreeing on every value in both books. This
//! crate transcribes them in [`crate::d10_tables`] and exposes them as
//! ready-to-use books via [`HfVqCodebook::builtin`] /
//! [`AdpcmVqCodebook::builtin`] / [`VqCodebooks::builtin`] — the
//! default state of every decoder
//! ([`crate::subframe_pcm::SubframePcmDecoder::new`]), so `nVQSUB < nSUBS` (HF-VQ)
//! and `PMODE != 0` (ADPCM) subbands decode out of the box. The typed
//! [`crate::audio_array::AudioArrayError::VqCodebookUnavailable`] refusal now fires
//! only when a caller explicitly strips the books
//! ([`VqCodebooks::none`]).
//!
//! What this module defines around the data:
//!
//! * the wire facts (index widths, book sizes, vector lengths) as
//!   constants, from the §5.4/§5.5 walkers and the §D.10 definitions;
//! * the §D.10 entry-decoding primitives —
//!   [`unpack_hfreq_vq_entry`] (16-bit entry → two 8-bit signed
//!   elements, low byte first, **each ÷ 2⁴**) and [`adpcm_vq_coeff`]
//!   (stored integer ÷ 2¹³, spec anchor: entry `9928` →
//!   `1.2119140625`);
//! * [`scan_hf_vq_indices_at`], the purely structural §5.5 phase-1
//!   walk (`nVQIndex = ExtractBits(10)` per HF subband) that captures
//!   the indices the lookup consumes;
//! * the caller-supplied-book containers ([`HfVqCodebook`] /
//!   [`AdpcmVqCodebook`], rounds 408/434) that the built-in books now
//!   flow through unchanged.
//!
//! ## The §D.10.2 divisor is `2^4 = 16` — a spec typo, corrected
//!
//! The spec's p.255 text renders the §D.10.2 element divisor as `24`,
//! and rounds 408/9 pinned the *rendering* carefully (the `24` sits on
//! the text baseline, unlike §D.10.1's `2^13` whose exponent is a
//! raised superscript) and carried the literal reading. The staged
//! recovery record (`tables/dts-d10-2-hfreq-vq.meta.md`) contradicts
//! the literal reading three independent ways and settles the divisor
//! as **`2^4 = 16`** — a `2^4` whose superscript was lost in
//! typesetting, giving §D.10.2 exactly the same form as §D.10.1's
//! `2^13`. Using the literal 24 costs a constant `16/24 = 2/3` gain
//! error on every VQ-coded HF subband. The same record settles the
//! intra-entry element order the spec never pinned: element `2k` is
//! entry `k`'s **low** byte. Both corrections are confirmed end to end
//! by the black-box reference decode of the §D.10-bearing fixture
//! (`tests/black_box_d10.rs`).

use crate::Result;
use crate::bitreader::BitReader;

// ------------------------------------------------------------------
// §D.10.2 — High-Frequency Subband VQ (`HFreqVQ`)
// ------------------------------------------------------------------

/// §D.10.2 code-book size: `2^10 = 1024` vectors.
pub const HFREQ_VQ_BOOK_SIZE: usize = 1024;

/// Width of the §5.5 phase-1 `nVQIndex` bitstream field
/// (`nVQIndex = ExtractBits(10)`, Table 5-29).
pub const HFREQ_VQ_INDEX_BITS: u32 = 10;

/// Elements per §D.10.2 vector: 32 subband samples (one subband
/// analysis window).
pub const HFREQ_VQ_VECTOR_LEN: usize = 32;

/// The §D.10.2 element divisor: each 8-bit signed integer unpacked
/// from a 16-bit entry is divided by `2^4 = 16` to give a vector
/// element. The spec's p.255 text renders this as `24` — a `2^4` with
/// a lost superscript; see the module docs and
/// `docs/audio/dts/tables/dts-d10-2-hfreq-vq.meta.md` for the
/// three-way evidence that corrected the earlier literal-24 reading.
pub const HFREQ_VQ_ELEMENT_DIVISOR: f64 = 16.0;

// ------------------------------------------------------------------
// §D.10.1 — ADPCM Coefficient VQ (`ADPCMCoeffVQ`)
// ------------------------------------------------------------------

/// §D.10.1 code-book size: `2^12 = 4096` vectors.
pub const ADPCM_VQ_BOOK_SIZE: usize = 4096;

/// Elements per §D.10.1 vector: the 4 ADPCM subband-prediction
/// coefficients (`PVQ[ch][n]`, consumed by the §C.2.2 predictor).
pub const ADPCM_VQ_VECTOR_LEN: usize = 4;

/// The §D.10.1 stored-entry scaling divisor: the actual coefficient
/// is the stored signed integer divided by `2^13 = 8192`.
pub const ADPCM_VQ_COEFF_DIVISOR: f64 = 8192.0;

/// Scale a §D.10.1 `ADPCMCoeffVQ` stored integer entry to the actual
/// prediction coefficient: `entry / 2^13`.
///
/// The spec's single published anchor: entry `9928` →
/// `9928 / 2^13 = 1.2119140625` (§D.10.1, PDF p.255).
#[must_use]
pub fn adpcm_vq_coeff(entry: i32) -> f64 {
    f64::from(entry) / ADPCM_VQ_COEFF_DIVISOR
}

// ------------------------------------------------------------------
// Drop-in containers for recovered §D.10 books
// ------------------------------------------------------------------

/// A decoded §D.10.2 `HFreqVQ` high-frequency-subband code book:
/// [`HFREQ_VQ_BOOK_SIZE`] vectors of [`HFREQ_VQ_VECTOR_LEN`] scaled
/// elements, ready for the §5.5 phase-1
/// `HFreqVQ.LookUp(nVQIndex, HFREQ[ch][n])`.
///
/// [`HfVqCodebook::builtin`] returns the real book, transcribed from
/// the staged clean-room table
/// (`docs/audio/dts/tables/dts-d10-2-hfreq-vq.csv`); the caller-
/// supplied constructors remain for tests and experimentation.
/// Everything *around* the numbers — the 10-bit index, the 1024 × 32
/// dimensions, the two-int8-per-entry packing, the ÷ 2⁴ element
/// scaling — is spec-pinned and enforced here.
#[derive(Clone)]
pub struct HfVqCodebook {
    vectors: Vec<[f64; HFREQ_VQ_VECTOR_LEN]>,
}

impl core::fmt::Debug for HfVqCodebook {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HfVqCodebook")
            .field("vectors", &self.vectors.len())
            .field("vector_len", &HFREQ_VQ_VECTOR_LEN)
            .finish()
    }
}

impl HfVqCodebook {
    /// The real §D.10.2 `HFreqVQ` book, built once (and shared) from
    /// the in-crate transcription of the staged clean-room table
    /// `docs/audio/dts/tables/dts-d10-2-hfreq-vq.csv` (see
    /// [`crate::d10_tables`] for the provenance chain): 1024 × 32
    /// int8 elements, each ÷ 2⁴ ([`HFREQ_VQ_ELEMENT_DIVISOR`]).
    #[must_use]
    pub fn builtin() -> std::sync::Arc<Self> {
        static BOOK: std::sync::OnceLock<std::sync::Arc<HfVqCodebook>> = std::sync::OnceLock::new();
        BOOK.get_or_init(|| {
            let vectors = crate::d10_tables::HFREQ_VQ_TABLE
                .iter()
                .map(|row| row.map(|e| f64::from(e) / HFREQ_VQ_ELEMENT_DIVISOR))
                .collect();
            std::sync::Arc::new(Self { vectors })
        })
        .clone()
    }

    /// Look up one 32-element vector by its 10-bit `nVQIndex`. Every
    /// wire-representable index (`0..1024`) is in range, so the §5.5
    /// phase-1 lookup cannot fail on a well-shaped book.
    #[must_use]
    pub fn vector(&self, index: u16) -> &[f64; HFREQ_VQ_VECTOR_LEN] {
        &self.vectors[usize::from(index) % HFREQ_VQ_BOOK_SIZE]
    }
}

/// A decoded §D.10.1 `ADPCMCoeffVQ` prediction-coefficient code book:
/// [`ADPCM_VQ_BOOK_SIZE`] vectors of [`ADPCM_VQ_VECTOR_LEN`] (= 4)
/// scaled coefficients, ready for the §5.4.1
/// `ADPCMCoeffVQ.LookUp(nVQIndex, PVQ[ch][n])` that feeds the §C.2.2
/// inverse-ADPCM predictor.
///
/// Like [`HfVqCodebook`], the real book is built in
/// ([`AdpcmVqCodebook::builtin`], from
/// `docs/audio/dts/tables/dts-d10-1-adpcm-coeff-vq.csv`); the
/// caller-supplied constructors remain for tests.
#[derive(Clone)]
pub struct AdpcmVqCodebook {
    coeffs: Vec<[f64; ADPCM_VQ_VECTOR_LEN]>,
}

impl core::fmt::Debug for AdpcmVqCodebook {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("AdpcmVqCodebook")
            .field("vectors", &self.coeffs.len())
            .field("vector_len", &ADPCM_VQ_VECTOR_LEN)
            .finish()
    }
}

impl AdpcmVqCodebook {
    /// The real §D.10.1 `ADPCMCoeffVQ` book, built once (and shared)
    /// from the in-crate transcription of the staged clean-room table
    /// `docs/audio/dts/tables/dts-d10-1-adpcm-coeff-vq.csv` (see
    /// [`crate::d10_tables`]): 4096 × 4 signed Q13 integers, each
    /// ÷ 2¹³ ([`adpcm_vq_coeff`]; spec anchor `9928` →
    /// `1.2119140625` at index 0 element 0).
    #[must_use]
    pub fn builtin() -> std::sync::Arc<Self> {
        static BOOK: std::sync::OnceLock<std::sync::Arc<AdpcmVqCodebook>> =
            std::sync::OnceLock::new();
        BOOK.get_or_init(|| {
            let coeffs = crate::d10_tables::ADPCM_VQ_TABLE
                .iter()
                .map(|row| row.map(|e| adpcm_vq_coeff(i32::from(e))))
                .collect();
            std::sync::Arc::new(Self { coeffs })
        })
        .clone()
    }

    /// Look up the four §C.2.2 predictor coefficients by the 12-bit
    /// `PVQ` index. Every wire-representable index (`0..4096`) is in
    /// range, so the lookup cannot fail on a well-shaped book.
    #[must_use]
    pub fn coefficients(&self, index: u16) -> &[f64; ADPCM_VQ_VECTOR_LEN] {
        &self.coeffs[usize::from(index) % ADPCM_VQ_BOOK_SIZE]
    }
}

/// The (optional) pair of §D.10 code books a decoder carries.
///
/// [`VqCodebooks::builtin`] — **both real books** — is what every
/// decoder now starts with ([`crate::subframe_pcm::SubframePcmDecoder::new`]).
/// [`VqCodebooks::none`] (also `Default`, kept for source
/// compatibility with the drop-in era) strips them, restoring the
/// typed [`crate::audio_array::AudioArrayError::VqCodebookUnavailable`] refusal on
/// the affected sub-paths. The books are held behind
/// [`std::sync::Arc`] so a stream decoder clone (the all-or-nothing
/// decode pattern) does not copy the table data — and the built-in
/// books are additionally process-wide singletons.
#[derive(Debug, Clone, Default)]
pub struct VqCodebooks {
    /// The §D.10.2 high-frequency-subband book (`HFreqVQ`).
    pub hfreq: Option<std::sync::Arc<HfVqCodebook>>,
    /// The §D.10.1 ADPCM prediction-coefficient book
    /// (`ADPCMCoeffVQ`).
    pub adpcm: Option<std::sync::Arc<AdpcmVqCodebook>>,
}

impl VqCodebooks {
    /// Both real §D.10 books ([`HfVqCodebook::builtin`] +
    /// [`AdpcmVqCodebook::builtin`]) — the default state of every
    /// decoder.
    #[must_use]
    pub fn builtin() -> Self {
        Self {
            hfreq: Some(HfVqCodebook::builtin()),
            adpcm: Some(AdpcmVqCodebook::builtin()),
        }
    }
}

// ------------------------------------------------------------------
// §5.5 phase 1 — structural HF-VQ index scan
// ------------------------------------------------------------------

/// Walk the §5.5 Table 5-29 phase-1 high-frequency VQ region
/// structurally, capturing the 10-bit `nVQIndex` of every HF-VQ
/// subband without attempting the (gap-blocked) `HFreqVQ.LookUp`.
///
/// Per the corrected walker trace
/// (`docs/audio/dts/dts-lfe-interpolation-and-audio-walker.md` §2.1):
///
/// ```text
/// for (ch = 0; ch < nPCHS; ch++)
///     for (n = nVQSUB[ch]; n < nSUBS[ch]; n++)
///         nVQIndex = ExtractBits(10);   // then HFreqVQ.LookUp(...)
/// ```
///
/// * `bytes` / `bit_offset` — positioned at the first §5.5 bit (the
///   phase-1 region precedes the LFE phase and the audio-data
///   arrays).
/// * `n_vqsub` / `n_subs` — the per-channel loop bounds
///   ([`crate::audio_header::AudioCodingHeader::n_vqsub`] / `n_subs`); slices of
///   equal length, one entry per primary channel.
///
/// Returns `(indices, bits_consumed)` where `indices[ch]` holds the
/// captured 10-bit indices for channel `ch`'s subbands
/// `nVQSUB[ch]..nSUBS[ch]` in walk order (empty when the channel has
/// no HF-VQ subbands — the common Core case where
/// `nVQSUB == nSUBS`). `bits_consumed` is exactly
/// `10 · Σ (nSUBS[ch] − nVQSUB[ch])`, letting a caller advance its
/// cursor to the §5.5 LFE phase.
///
/// The full decode ([`crate::subframe_pcm::SubframePcmDecoder`]) looks the captured
/// indices up in the built-in §D.10.2 book; this structural scan
/// remains useful for stream inspection.
///
/// # Errors
///
/// [`crate::Error::UnexpectedEof`] on a truncated region.
pub fn scan_hf_vq_indices_at(
    bytes: &[u8],
    bit_offset: usize,
    n_vqsub: &[usize],
    n_subs: &[usize],
) -> Result<(Vec<Vec<u16>>, usize)> {
    debug_assert_eq!(n_vqsub.len(), n_subs.len());
    let byte_offset = bit_offset / 8;
    let intra_byte = bit_offset % 8;
    let mut br = BitReader::from_byte_offset(bytes, byte_offset);
    if intra_byte > 0 {
        br.read_bits(intra_byte as u32)?;
    }

    let mut indices = Vec::with_capacity(n_vqsub.len());
    for (&vqsub, &subs) in n_vqsub.iter().zip(n_subs) {
        let mut ch_indices = Vec::new();
        for _ in vqsub..subs {
            ch_indices.push(br.read_bits(HFREQ_VQ_INDEX_BITS)? as u16);
        }
        indices.push(ch_indices);
    }

    let bits_consumed = br.absolute_bit_position() - bit_offset;
    Ok((indices, bits_consumed))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Error;

    fn pack_fields(fields: &[(u32, u8)]) -> Vec<u8> {
        let total_bits: usize = fields.iter().map(|(_, w)| *w as usize).sum();
        let mut out = vec![0u8; total_bits.div_ceil(8)];
        let mut bit_pos = 0usize;
        for &(value, width) in fields {
            for i in (0..width).rev() {
                let bit = ((value >> i) & 1) as u8;
                out[bit_pos / 8] |= bit << (7 - (bit_pos % 8));
                bit_pos += 1;
            }
        }
        out
    }

    /// The §D.10.1 anchor printed in the spec: stored entry 9928 →
    /// coefficient 1.2119140625 (= 9928 / 2^13).
    #[test]
    fn adpcm_anchor_entry_9928() {
        assert_eq!(adpcm_vq_coeff(9928), 1.2119140625);
        assert_eq!(adpcm_vq_coeff(0), 0.0);
        assert_eq!(adpcm_vq_coeff(-8192), -1.0);
    }

    /// The structural scan reads exactly 10 bits per HF-VQ subband in
    /// (ch, n) walk order and reports the consumed bit count.
    #[test]
    fn scan_captures_indices_in_walk_order() {
        // ch0: nVQSUB=2, nSUBS=4 -> 2 indices; ch1: 3..3 -> none;
        // ch2: 0..2 -> 2 indices.
        let vals = [0x3FFu32, 0x001, 0x155, 0x2AA];
        let fields: Vec<(u32, u8)> = vals.iter().map(|&v| (v, 10u8)).collect();
        let stream = pack_fields(&fields);
        let (idx, bits) = scan_hf_vq_indices_at(&stream, 0, &[2, 3, 0], &[4, 3, 2]).unwrap();
        assert_eq!(bits, 40);
        assert_eq!(idx, vec![vec![0x3FF, 0x001], vec![], vec![0x155, 0x2AA]]);
    }

    /// A non-byte-aligned start cursor is honoured (the §5.5 region
    /// rarely begins on a byte boundary).
    #[test]
    fn scan_honours_bit_offset() {
        let fields = [(0b101u32, 3u8), (0x2AB, 10)];
        let stream = pack_fields(&fields);
        let (idx, bits) = scan_hf_vq_indices_at(&stream, 3, &[1], &[2]).unwrap();
        assert_eq!(bits, 10);
        assert_eq!(idx, vec![vec![0x2AB]]);
    }

    /// The common Core case (`nVQSUB == nSUBS` everywhere) consumes
    /// zero bits.
    #[test]
    fn scan_empty_when_no_hf_vq_subbands() {
        let (idx, bits) = scan_hf_vq_indices_at(&[0u8; 4], 0, &[2, 4], &[2, 4]).unwrap();
        assert_eq!(bits, 0);
        assert_eq!(idx, vec![Vec::<u16>::new(), Vec::new()]);
    }

    /// The built-in §D.10.1 book reproduces the spec's only printed
    /// anchor (index 0 element 0: entry `9928` → `1.2119140625`) and
    /// the staged table's pinned sample rows (`.meta.md` "Sample
    /// values").
    #[test]
    fn builtin_adpcm_book_matches_staged_table_anchors() {
        let book = AdpcmVqCodebook::builtin();
        assert_eq!(
            book.coefficients(0),
            &[9928, -2618, -1093, -1263].map(|e| f64::from(e) / 8192.0)
        );
        assert_eq!(book.coefficients(0)[0], 1.2119140625);
        assert_eq!(
            book.coefficients(1),
            &[11077, -2876, -1747, -308].map(|e| f64::from(e) / 8192.0)
        );
        assert_eq!(
            book.coefficients(4095),
            &[8538, -6997, 5309, 453].map(|e| f64::from(e) / 8192.0)
        );
    }

    /// The transcribed §D.10.1 table reproduces the staged `.meta.md`
    /// verification facts: stored range `-21806 … 21657`, and all
    /// 4096 vectors distinct.
    #[test]
    fn builtin_adpcm_table_range_and_distinctness() {
        let table = &crate::d10_tables::ADPCM_VQ_TABLE;
        let min = table.iter().flatten().min().unwrap();
        let max = table.iter().flatten().max().unwrap();
        assert_eq!((*min, *max), (-21806, 21657));
        let distinct: std::collections::HashSet<[i16; 4]> = table.iter().copied().collect();
        assert_eq!(distinct.len(), 4096, "all §D.10.1 vectors are distinct");
    }

    /// Whether every root of the degree-4 predictor polynomial
    /// `A(z) = 1 − Σ cₖ z^(−k−1)` lies strictly inside radius `m`,
    /// via the Schur–Cohn (step-down) recursion on `A(m·z)` — all
    /// reflection coefficients strictly inside the unit disc.
    fn predictor_roots_inside(coeffs: &[f64; 4], m: f64) -> bool {
        // A(m·z) in powers of z^{-1}: aᵢ = −c_{i−1} · m^{−i}, a₀ = 1.
        let mut a = [1.0, 0.0, 0.0, 0.0, 0.0];
        for (i, &c) in coeffs.iter().enumerate() {
            a[i + 1] = -c / m.powi(i as i32 + 1);
        }
        let mut n = 4;
        while n > 0 {
            let k = a[n];
            if k.abs() >= 1.0 {
                return false;
            }
            let denom = 1.0 - k * k;
            let prev = a;
            for (i, slot) in a.iter_mut().enumerate().take(n).skip(1) {
                *slot = (prev[i] - k * prev[n - i]) / denom;
            }
            n -= 1;
        }
        true
    }

    /// The staged `.meta.md` stability fact, re-proved on the
    /// transcription: every one of the 4096 §D.10.1 vectors is a
    /// strictly minimum-phase fourth-order predictor, with the
    /// largest root modulus bracketed around the recorded `0.98702`
    /// (all inside radius 0.988; not all inside 0.986). A mis-framed
    /// transcription (wrong stride/order/signedness) does not produce
    /// 4096 consecutive stable predictors with that exact margin.
    #[test]
    fn builtin_adpcm_predictors_all_minimum_phase_with_recorded_margin() {
        let book = AdpcmVqCodebook::builtin();
        let mut inside_0986 = 0usize;
        for idx in 0..ADPCM_VQ_BOOK_SIZE {
            let coeffs = book.coefficients(idx as u16);
            assert!(
                predictor_roots_inside(coeffs, 1.0),
                "vector {idx} is not minimum-phase"
            );
            assert!(
                predictor_roots_inside(coeffs, 0.988),
                "vector {idx} has a root beyond the recorded 0.98702 margin"
            );
            if predictor_roots_inside(coeffs, 0.986) {
                inside_0986 += 1;
            }
        }
        assert!(
            inside_0986 < ADPCM_VQ_BOOK_SIZE,
            "some vector must reach past 0.986 (recorded max modulus 0.98702)"
        );
    }

    /// The built-in §D.10.2 book reproduces the staged table's pinned
    /// sample rows, with the ÷ 2⁴ scaling applied.
    #[test]
    fn builtin_hf_book_matches_staged_table_anchors() {
        let book = HfVqCodebook::builtin();
        assert!(
            book.vector(0).iter().all(|&e| e == 0.0),
            "index 0 is the zero vector"
        );
        let v1 = book.vector(1);
        let want1 = [-4, -2, 2, 1, -16, -10, 1, 3].map(|e| f64::from(e) / 16.0);
        assert_eq!(&v1[..8], &want1);
        let v1023 = book.vector(1023);
        let want1023 = [5, 0, -6, 5, 6, 3, 3, -10].map(|e| f64::from(e) / 16.0);
        assert_eq!(&v1023[..8], &want1023);
    }

    /// The transcribed §D.10.2 table reproduces the staged `.meta.md`
    /// verification facts: element range `-87 … 89`, exactly one zero
    /// vector, and 996 distinct patterns (28 genuine duplicate code
    /// words, clustered in the recovered book).
    #[test]
    fn builtin_hf_table_range_zero_vector_and_duplicates() {
        let table = &crate::d10_tables::HFREQ_VQ_TABLE;
        let min = table.iter().flatten().min().unwrap();
        let max = table.iter().flatten().max().unwrap();
        assert_eq!((*min, *max), (-87, 89));
        let zero_vectors = table
            .iter()
            .filter(|row| row.iter().all(|&e| e == 0))
            .count();
        assert_eq!(zero_vectors, 1, "index 0 is the only zero vector");
        let distinct: std::collections::HashSet<[i8; 32]> = table.iter().copied().collect();
        assert_eq!(distinct.len(), 996, "996 distinct patterns / 28 duplicates");
    }

    /// A truncated region reports EOF rather than fabricating indices.
    #[test]
    fn scan_reports_eof_on_truncation() {
        let stream = [0u8; 1]; // 8 bits; a single index needs 10.
        assert_eq!(
            scan_hf_vq_indices_at(&stream, 0, &[0], &[1]).unwrap_err(),
            Error::UnexpectedEof
        );
    }
}
