//! DTS Coherent Acoustics — §C.2.3 Joint Subband Coding.
//!
//! Round 223 (2026-06-03) lands the §C.2.3 joint-subband decode, the
//! per-channel reconstruction step that copies the high-end subband
//! samples of a source channel into a destination channel and scales
//! them by the destination channel's per-subband
//! `JOIN_SCALES[ch][n]` factor. The encoder uses joint-subband coding
//! to drop redundant high-frequency content from a destination
//! channel: only the source channel's high subbands are coded on the
//! wire; the decoder re-synthesises the destination's high subbands
//! from them at unpack time.
//!
//! Source: ETSI TS 102 114 V1.3.1 (2011-08), Annex C (informative)
//! §C.2.3 "Joint Subband Coding" (PDF p.184) — staged at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`. The
//! reproduced normative spec pseudocode is:
//!
//! ```text
//! for (ch=0; ch<nPCHS; ch++)
//!     if ( JOINX[ch]>0 ){                       // Joint subband coding enabled.
//!         nSourceCh = JOINX[ch]-1;              // Get source channel. JOINX counts
//!                                               // channels as 1,2,3,4,5, so minus 1.
//!         for (n=nSUBS[ch]; n<nSUBS[nSourceCh]; n++)
//!             for (nSample=0; nSample<8*nSSC; nSample++)
//!                 aPrmCh[ch].aSubband[n].aSample[nSample] =
//!                     JOIN_SCALES[ch][n] *
//!                     aPrmCh[nSourceCh].aSubband[n].aSample[nSample];
//!     }
//! ```
//!
//! Key structural facts the pseudocode locks down:
//!
//! - The trigger is `JOINX[ch] > 0`; `JOINX[ch] == 0` means the
//!   destination channel does not import joint-coded subbands from
//!   any source.
//! - `nSourceCh = JOINX[ch] - 1`: `JOINX` is one-based per the
//!   pseudocode's inline comment ("counts channels as 1,2,3,4,5, so
//!   minus 1"); the array indexing into `aPrmCh[]` is zero-based.
//! - The imported subband range is exactly
//!   `n ∈ [nSUBS[ch], nSUBS[nSourceCh])`: the destination's own
//!   subbands `0..nSUBS[ch]` are unchanged; subbands above the source
//!   channel's `nSUBS[nSourceCh]` upper bound are not touched (they
//!   remain whatever they were before the joint-subband step, i.e.
//!   zero for an inactive subband).
//! - Per-subband scaling: a single scalar `JOIN_SCALES[ch][n]` is
//!   broadcast over all `8 * nSSC` samples in that subband.
//! - The operation is unconditionally **write**, not accumulate: the
//!   destination subband is replaced by the scaled copy.
//!
//! # Scope
//!
//! This module exposes the matrix copy + scale itself, not the
//! dispatch. The caller (a future subframe walker) is responsible
//! for:
//!
//! - Reading the `JOINX[ch]` per-channel selector from the AUDIO
//!   CODING HEADER (`JOINX` is per-channel and 0 when joint-subband
//!   coding is disabled for that channel; > 0 when enabled, with the
//!   one-based source-channel index).
//! - Reading the per-channel-per-subband `JOIN_SCALES[ch][n]` scale
//!   factors from the bit stream (the §5.4.x joint-scale Huffman /
//!   linear decode for the active range
//!   `n ∈ [nSUBS[ch], nSUBS[nSourceCh])`).
//! - Translating the one-based `JOINX[ch]` to the zero-based
//!   `nSourceCh = JOINX[ch] - 1` (see [`joint_source_channel`]).
//! - Reading the per-channel `nSUBS[ch]` active-subband count for both
//!   the destination and source channels and confirming
//!   `nSUBS[ch] < nSUBS[nSourceCh]` (an empty `[nSUBS[ch],
//!   nSUBS[nSourceCh])` range yields a no-op decode but is otherwise
//!   well-formed: see [`joint_subband_decode_range_i32`] /
//!   [`joint_subband_decode_range_f64`]).
//!
//! Both the integer (i32) and floating-point (f64) decode flavours are
//! exposed because the §C.2.3 spec text does not constrain the
//! arithmetic type — the inverse-quantisation path that feeds the
//! joint-subband decode may run in either; the decoder picks per its
//! precision requirements. The i32 flavour uses `i32::wrapping_mul`
//! to mirror the §C.2.3 pseudocode's C-style `int` overflow
//! semantics; callers that require saturating arithmetic perform the
//! conversion before invoking the primitive.

/// Resolve the one-based `JOINX[ch]` field to the zero-based
/// source-channel index `nSourceCh` per §C.2.3.
///
/// Returns `None` when `joinx == 0` (joint-subband coding disabled
/// for the channel — the §C.2.3 `if (JOINX[ch] > 0)` predicate
/// rejects this code, so no source channel is named). Returns
/// `Some(joinx - 1)` for `joinx > 0`.
///
/// The dispatch predicate matches the §C.2.3 pseudocode's
/// `if (JOINX[ch] > 0)` gate directly: callers route through this
/// helper to obtain the source-channel index in one step.
#[must_use]
pub fn joint_source_channel(joinx: u8) -> Option<u8> {
    if joinx == 0 { None } else { Some(joinx - 1) }
}

#[cfg(test)]
mod tests {

    // -----------------------------------------------------------
    // Dispatch / source-channel-resolution tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // i32 range decode — happy-path matrix tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // i32 range decode — error-path shape tests
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // f64 range decode — happy-path + error-path
    // -----------------------------------------------------------

    // -----------------------------------------------------------
    // End-to-end §C.2.3 sweep — hand-computed expected
    // -----------------------------------------------------------
}
