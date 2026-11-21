//! DTS Coherent Acoustics — §C.2.2 Inverse ADPCM.
//!
//! Round 228 (2026-06-04) lands the §C.2.2 inverse-ADPCM predictor,
//! the per-sample reconstruction step that turns a subband's
//! residual (error) sample stream into the reconstructed subband
//! sample stream when the subband's `PMODE == 1` flag indicates
//! ADPCM prediction is active.
//!
//! Source: ETSI TS 102 114 V1.3.1 (2011-08), Annex C (informative)
//! §C.2.2 "Inverse ADPCM" (staged PDF p.183) at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`. The
//! spec's normative pseudocode reads (reproduced as documented):
//!
//! ```text
//! void InverseADPCM(void) {
//! // NumADPCMCoeff =4, the number of ADPCM coefficients.
//! // raADPCMCoeff[] are the ADPCM coefficients extracted
//! // from the bit stream.
//! // raSample[NumADPCMCoeff], ..., raSample[-1] are the
//! // history from last subframe or subsubframe. It must
//! // updated each time before reverse ADPCM is run for a
//! // block of samples for each subband.
//! for (m=0; m<nNumSample; m++)
//!     for (n=0; n<NumADPCMCoeff; n++)
//!         raSample[m] += raADPCMCoeff[n]*raSample[m-n-1];
//! }
//! ```
//!
//! The predictor is a fixed-order (4-tap) FIR running over the
//! reconstructed signal: each output sample is the bit-stream
//! residual plus the dot product of the four ADPCM coefficients
//! with the four most recently reconstructed samples (the past
//! one, two, three, and four samples). The fourth-order
//! reconstructed signal forms the running history that the next
//! samples consume; the spec's comment "history from last subframe
//! or subsubframe" makes the per-subband, per-decode-block
//! initial-condition handoff explicit.
//!
//! # Scope
//!
//! This module exposes the predictor primitive itself, not the
//! dispatch. Its caller, the subframe decode in `subframe_pcm.rs`, is
//! responsible for:
//!
//! - Reading the per-subband `PMODE` flag and only invoking the
//!   predictor for subbands whose `PMODE == 1`.
//! - Extracting the four ADPCM coefficients (`raADPCMCoeff[0..4]`)
//!   from the bit stream and feeding them in.
//! - Maintaining the rolling four-sample history across decode
//!   blocks: at end-of-block the last four reconstructed samples
//!   become the next block's `raSample[-4]..raSample[-1]` priming
//!   values.
//!
//! Only the floating-point (f64) predictor is kept, as the decoder runs
//! in f64; the integer flavour was left out (`NOTICE.md`).
//!
//! # Implementation note
//!
//! The §C.2.2 pseudocode loop reads `raSample[m - n - 1]` for
//! `n ∈ [0, 4)`, i.e. the four samples *preceding* index `m` in the
//! same `raSample[]` array (with negative indices `[-4..0)` carrying
//! the priming history from the prior decode block). This is an
//! in-place accumulation: the output of step `m` is written to
//! `raSample[m]` and then *immediately* visible as `raSample[(m+1) - 1]`
//! when step `m + 1` consumes its `n = 0` history slot. The decoder
//! must therefore reconstruct samples strictly left-to-right; a
//! parallel / SIMD reformulation would have to materialise the
//! running history explicitly.
//!
//! The number of ADPCM coefficients is fixed at four by the spec's
//! `NumADPCMCoeff = 4`, so the predictor's per-sample inner loop is
//! a four-tap dot product. The crate exposes the constant
//! [`NUM_ADPCM_COEFF`] for callers that want to size buffers by the
//! same invariant the spec writes against.
//!
//! Integer arithmetic is wrapping (`i32::wrapping_add`,
//! `i32::wrapping_mul`) to mirror the spec's C `int` semantics for
//! the §C.2.2 pseudocode (the spec's `int` type wraps on overflow;
//! the decoder does not require saturation).

use crate::{Error, Result};

/// Number of ADPCM prediction coefficients per the §C.2.2 spec
/// invariant `NumADPCMCoeff = 4`. The predictor reads four history
/// samples per output sample; the bit stream carries four
/// coefficients per subband when `PMODE == 1`.
pub const NUM_ADPCM_COEFF: usize = 4;

/// Apply the §C.2.2 inverse-ADPCM predictor to a single subband's
/// residual sample stream in place, in floating-point (f64)
/// arithmetic. Same predictor structure as
/// [`inverse_adpcm_decode_i32`]; chosen by callers that consume the
/// reconstructed subband samples in floating-point.
///
/// # Errors
///
/// Returns [`Error::InverseAdpcmShapeMismatch`] under the same
/// conditions as [`inverse_adpcm_decode_i32`].
pub fn inverse_adpcm_decode_f64(
    history: &[f64],
    coeffs: &[f64],
    samples: &mut [f64],
) -> Result<()> {
    if history.len() != NUM_ADPCM_COEFF || coeffs.len() != NUM_ADPCM_COEFF {
        return Err(Error::InverseAdpcmShapeMismatch {
            history_len: history.len(),
            coeffs_len: coeffs.len(),
        });
    }
    for m in 0..samples.len() {
        let mut acc = samples[m];
        for n in 0..NUM_ADPCM_COEFF {
            let past = if (n + 1) <= m {
                samples[m - n - 1]
            } else {
                history[NUM_ADPCM_COEFF + m - n - 1]
            };
            acc += coeffs[n] * past;
        }
        samples[m] = acc;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // ----------------------------------------------------------------
    // i32 single-block decode — predictor property tests
    // ----------------------------------------------------------------

    // ----------------------------------------------------------------
    // f64 single-block decode tests
    // ----------------------------------------------------------------

    #[test]
    fn f64_zero_coeffs_make_predictor_identity() {
        let history = [1.0_f64, 2.0, 3.0, 4.0];
        let coeffs = [0.0_f64; NUM_ADPCM_COEFF];
        let mut samples = [10.0_f64, 20.0, 30.0];
        inverse_adpcm_decode_f64(&history, &coeffs, &mut samples).unwrap();
        assert_eq!(samples, [10.0, 20.0, 30.0]);
    }

    #[test]
    fn f64_each_coeff_taps_the_right_history_slot() {
        let history = [1.0_f64, 2.0, 3.0, 4.0];
        let coeffs = [1.0_f64, 10.0, 100.0, 1000.0];
        let mut samples = [0.0_f64];
        inverse_adpcm_decode_f64(&history, &coeffs, &mut samples).unwrap();
        // 1*4 + 10*3 + 100*2 + 1000*1 = 4 + 30 + 200 + 1000 = 1234
        assert!((samples[0] - 1234.0).abs() < 1e-9);
    }

    #[test]
    fn f64_uses_just_reconstructed_sample_immediately() {
        let history = [0.0_f64; NUM_ADPCM_COEFF];
        let coeffs = [0.5_f64, 0.0, 0.0, 0.0];
        let mut samples = [1.0_f64, 0.0, 0.0, 0.0];
        inverse_adpcm_decode_f64(&history, &coeffs, &mut samples).unwrap();
        // 1.0, 0.5, 0.25, 0.125
        assert!((samples[0] - 1.0).abs() < 1e-12);
        assert!((samples[1] - 0.5).abs() < 1e-12);
        assert!((samples[2] - 0.25).abs() < 1e-12);
        assert!((samples[3] - 0.125).abs() < 1e-12);
    }

    #[test]
    fn f64_history_length_mismatch() {
        let short_history = [0.0_f64; 2];
        let coeffs = [0.0_f64; NUM_ADPCM_COEFF];
        let mut samples = [0.0_f64; 1];
        let err = inverse_adpcm_decode_f64(&short_history, &coeffs, &mut samples).unwrap_err();
        assert!(matches!(
            err,
            Error::InverseAdpcmShapeMismatch {
                history_len: 2,
                coeffs_len: 4,
            }
        ));
    }

    #[test]
    fn f64_coeffs_length_mismatch() {
        let history = [0.0_f64; NUM_ADPCM_COEFF];
        let long_coeffs = [0.0_f64; 5];
        let mut samples = [0.0_f64; 1];
        let err = inverse_adpcm_decode_f64(&history, &long_coeffs, &mut samples).unwrap_err();
        assert!(matches!(
            err,
            Error::InverseAdpcmShapeMismatch {
                history_len: 4,
                coeffs_len: 5,
            }
        ));
    }

    #[test]
    fn f64_empty_block_is_no_op() {
        let history = [1.0_f64, 2.0, 3.0, 4.0];
        let coeffs = [5.0_f64, 6.0, 7.0, 8.0];
        let mut samples: [f64; 0] = [];
        inverse_adpcm_decode_f64(&history, &coeffs, &mut samples).unwrap();
    }

    // ----------------------------------------------------------------
    // History-update helper tests
    // ----------------------------------------------------------------

    // ----------------------------------------------------------------
    // Dispatch predicate tests
    // ----------------------------------------------------------------

    // ----------------------------------------------------------------
    // End-to-end two-block continuation sweep
    // ----------------------------------------------------------------
}
