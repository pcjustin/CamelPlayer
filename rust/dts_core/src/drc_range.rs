//! DTS Dynamic Range Control: the signed-Q2 `dts_dynrng_to_db()`
//! code→dB mapping (§5.4.1 / §5.7.2) plus the §D.4 "Dynamic Range
//! Control" presentation table.
//!
//! ## The wire format: an 8-bit signed Q2 code (round 408)
//!
//! Per §5.4.1 (`RANGE` field description, recovered as a closed-form
//! function in the freshly staged `docs/audio/dts/dts-drc-dynrng.md`):
//!
//! > "Each coefficient is **8-bit signed fractional Q2 binary** and
//! > represents a logarithmic gain value as shown in clause D.4
//! > giving a range of **±31,75 dB in steps of 0,25 dB**. Dynamic
//! > range compression is affected by multiplying the decoded audio
//! > samples by the linear coefficient."
//!
//! So the byte the bitstream carries (`ExtractBits(8)` in the §5.4.1
//! `DYNF != 0` tail, and each `subsubFrameDRC_Rev2AUX[]` byte of the
//! §5.7.2 Rev2AUX chunk) is a **two's-complement signed Q2** value:
//!
//! ```text
//! dB     = (int8_t)code × 0.25          // dts_dynrng_to_db()
//! linear = 10^(dB / 20)                 // multiplies each sample
//! ```
//!
//! Code `0` is therefore **unity** (0 dB), and the applied gain is
//! `10^(dB/20)`, post-QMF ([`dts_dynrng_to_db`] /
//! [`dts_dynrng_to_linear`]).
//!
//! ## The §D.4 table is an offset-binary *presentation* — do not
//! index it with the raw code
//!
//! Annex D §D.4 (PDF p.195-197) prints the same mapping as a 256-row
//! `Index | Q18 binary | Multiplier | Log Multiplier (dB)` table,
//! where `dB(Index) = (Index − 127) × 0.25` — i.e. the printed
//! `Index` column is **offset-binary** (`Index = signed_code + 127`;
//! Index `127` = code `0` = 0 dB). [`DRC_RANGE_MULTIPLIER`] preserves
//! the printed `Multiplier` column keyed by that printed Index.
//! Indexing it directly with a raw wire code (`table[code]`) is off
//! by 127 steps — code `0` would wrongly yield −31.75 dB — which is
//! exactly the correctness trap `docs/audio/dts/dts-drc-dynrng.md`
//! documents ("Why the §D.4 table was reverted"). Decoders must use
//! the signed-Q2 function; the table stays available as the §D.4
//! reference data and is cross-checked against the function in the
//! tests below.
//!
//! §5.4.1 Table 5-28 application pseudocode (the `RANGE` multiply
//! runs **after** the §C.2.5 QMF synthesis):
//!
//! ```text
//! if ( DYNF != 0 ) {
//!   nIndex = ExtractBits(8);
//!   RANGEtbl.LookUp(nIndex, RANGE);
//!   for (ch=0; ch<nPCHS; ch++)
//!     for (n=0; n<nNumSamples; n++)
//!       AudioCh[ch].ReconstructedSamples[n] *= RANGE;
//! }
//! ```
//!
//! This module is feature-independent (no `oxideav-core` dep), so it
//! is available under both the default and `--no-default-features`
//! build modes.

/// The §5.4.1 / §5.7.2 `dts_dynrng_to_db()` function: interpret the
/// 8-bit DRC code from the bitstream as **two's-complement signed Q2**
/// and return its dB gain (`0.25` dB per LSB).
///
/// Per `docs/audio/dts/dts-drc-dynrng.md`: input spans the full byte
/// (`0x00..=0xFF` → signed `−128..=+127`), output spans
/// `[−32.00 dB, +31.75 dB]` in 0.25 dB steps (the spec's nominal
/// usable range is ±31.75 dB; the extreme `0x80` = −32.0 dB sits just
/// outside it). Code `0` is unity (0 dB).
#[must_use]
pub fn dts_dynrng_to_db(code: u8) -> f64 {
    // Sign-extend: two's complement, then Q2 -> 0.25 dB / LSB.
    f64::from(code as i8) * 0.25
}

/// The linear gain a §5.4.1 `RANGE` / §5.7.2 Rev2AUX DRC code applies
/// to every reconstructed PCM sample: `10^(dts_dynrng_to_db(code)/20)`
/// (§5.4.1: "Dynamic range compression is affected by multiplying the
/// decoded audio samples by the linear coefficient").
///
/// Code `0` returns exactly `1.0`.
#[must_use]
pub fn dts_dynrng_to_linear(code: u8) -> f64 {
    if code == 0 {
        return 1.0;
    }
    10f64.powf(dts_dynrng_to_db(code) / 20.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    // -----------------------------------------------------------
    // dts_dynrng_to_db / dts_dynrng_to_linear (signed Q2, round 408)
    // -----------------------------------------------------------

    /// The signed-Q2 anchors from `docs/audio/dts/dts-drc-dynrng.md`:
    /// code 0 = 0 dB (unity), 0.25 dB per LSB in both directions, and
    /// the two's-complement extremes.
    #[test]
    fn dynrng_signed_q2_anchors() {
        assert_eq!(dts_dynrng_to_db(0), 0.0);
        assert_eq!(dts_dynrng_to_linear(0), 1.0);
        assert_eq!(dts_dynrng_to_db(1), 0.25);
        assert_eq!(dts_dynrng_to_db(0xFF), -0.25); // signed -1
        assert_eq!(dts_dynrng_to_db(0x7F), 31.75); // +127
        assert_eq!(dts_dynrng_to_db(0x80), -32.0); // -128 (outside nominal)
        assert_eq!(dts_dynrng_to_db(0x81), -31.75); // -127
        // +20 dB = 80 quarter-dB steps -> linear 10.0.
        assert_eq!(dts_dynrng_to_db(80), 20.0);
        assert!((dts_dynrng_to_linear(80) - 10.0).abs() < 1e-12);
        // -20 dB -> linear 0.1.
        let minus_20 = 0u8.wrapping_sub(80);
        assert_eq!(dts_dynrng_to_db(minus_20), -20.0);
        assert!((dts_dynrng_to_linear(minus_20) - 0.1).abs() < 1e-12);
    }
}
