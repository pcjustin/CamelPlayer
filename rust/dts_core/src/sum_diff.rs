//! DTS Coherent Acoustics — §C.2.4 Sum/Difference Decoding.
//!
//! Round 214 (2026-06-03) lands the §C.2.4 sum/difference matrix
//! decoder, the inverse of the encoder-side joint sum/difference
//! coding that the `FRONT_SUM` (`SUMF`) and `SURROUND_SUM` (`SUMS`)
//! header flags signal (and that the `AMODE == 3` Sum/Difference
//! channel-arrangement code implies for the front pair).
//!
//! Source: ETSI TS 102 114 V1.3.1 (2011-08), Annex C (informative)
//! §C.2.4 "Sum/Difference Decoding" (PDF p.184) — staged at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`. The
//! reproduced normative spec pseudocode is:
//!
//! ```text
//! // SUMF — front L/R (also when AMODE == 3, Sum/Difference)
//! for (n=0; n<nSUBS; n++)               // All active subbands.
//!     for (nSample=0; nSample<8*nSSC; nSample++) {  // Samples in all subsubframes
//!         FrontLeft[nSample]   = Lfeft[nSample] + Fright[nSample];
//!         Frontright[nSample]  = Lfeft[nSample] - Fright[nSample];
//!     }
//!
//! // SUMS — surround L/R
//! for (n=0; n<nSUBS; n++)
//!     for (nSample=0; nSample<8*nSSC; nSample++) {
//!         SurroundLeft[nSample]   = Sleft[nSample] + Sright[nSample];
//!         Surroundright[nSample]  = Sleft[nSample] - Sright[nSample];
//!     }
//! ```
//!
//! The spec works in the subband-sample domain (reconstructed
//! subband samples, before the §C.2.5 32-band synthesis QMF) so the
//! decode is purely a per-sample (`L', R') = (L+R, L-R)` matrix
//! multiplication. The matrix is symmetric and self-inverse up to
//! a factor of 2: applying it twice produces `(2 L, 2 R)`. The
//! decoder applies it once, undoing a single encoder-side application
//! (the encoder produces `(L+R, L-R)` from `(L, R)`; the decoder
//! recovers something proportional to the original `(L, R)` pair).
//!
//! # Scope
//!
//! This module exposes the matrix decode itself, not the dispatch.
//! The caller (a future subframe walker) is responsible for:
//!
//! - Reading the `FRONT_SUM` / `SURROUND_SUM` header flags from the
//!   frame header (already surfaced by [`crate::header::DtsFrameHeader`] as
//!   [`crate::header::DtsFrameHeader::front_sum`] / [`crate::header::DtsFrameHeader::surround_sum`]).
//! - Recognising that `AMODE == 3` ([`crate::header::AmodeArrangement::SumDifference`])
//!   forces the front-channel decode regardless of the `FRONT_SUM` bit
//!   (per the §C.2.4 narrative: "This decoding is also required when
//!   AMODE = 3.").
//! - Identifying which pair of channels carries the encoder's
//!   `(L+R, L-R)` (front L / front R for `SUMF`; surround L / surround R
//!   for `SUMS`).
//! - Determining the number of active subbands (`nSUBS`) and the
//!   number of sub-sub-frame samples (`8*nSSC`), and arranging the
//!   slice geometry so each `(left, right)` pair the decoder consumes
//!   is "all subbands × all samples per subsubframe" for that channel.
//!
//! Both the integer (i32) and floating-point (f64) decode flavours are
//! exposed because the §C.2.4 spec text does not constrain the
//! arithmetic type — the inverse-quantisation path that feeds the
//! sum/difference decode may run in either; the decoder picks per
//! its precision requirements.
//!
//! # Implementation note
//!
//! The decode is an **in-place** sample-by-sample operation: the
//! caller passes mutable slices for the left and right channels of
//! the pair, and on return each slice has been overwritten with its
//! decoded value (`left` ← `left + right`, `right` ← `left - right`,
//! using the **pre-update** value of `left` for the right channel —
//! exactly the §C.2.4 pseudocode's read-old / write-new ordering).
//! Integer arithmetic is wrapping (`i32::wrapping_add` /
//! `i32::wrapping_sub`) to mirror C semantics for the §C.2.4
//! pseudocode (the spec's `int` type wraps on overflow; the decoder
//! does not require saturation).

#[cfg(test)]
mod tests {

    // -----------------------------------------------------------
    // i32 single-pair decode — matrix property tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // f64 single-pair decode tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // Subband-pair (slice-of-slices) decode tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // Dispatch predicate tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // End-to-end full §C.2.4 sweep
    // -----------------------------------------------------------
}
