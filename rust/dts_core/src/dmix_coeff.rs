//! §D.11 "Look-up Table for Downmix Scale Factors" (`DmixTable` /
//! `InvDmixTbl`) plus the §5.7.1 Table 5-31 downmix-coefficient code
//! resolver.
//!
//! Transcribed verbatim from ETSI TS 102 114 V1.3.1 (2011-08) Annex D
//! §D.11 (staged at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`, PDF
//! p.256-259). The printed table has six columns; this module keeps
//! the two normative integer columns:
//!
//! - `DmixTable` — unsigned 16-bit values, the `AbsValues` column
//!   "after multiplication by 2^15 and rounding to the nearest
//!   integer value" (§5.7.2.2), indexed by `DmixTblIndex` `0..=240`.
//! - `InvDmixTbl` — unsigned 24-bit values, the `InvAbsValues`
//!   column "after multiplication by 2^16 and rounding to the
//!   nearest integer value" (§8.5.x wording mirrored at PDF p.256),
//!   indexed by `InvDmixTblIndex` `0..=200`. Inverse entries exist
//!   only for `DmixTblIndex >= 40` (`-40 dB` and louder):
//!   `InvDmixTblIndex = DmixTblIndex - 40`.
//!
//! The informative `LogAbsValues (dB)` column is a piecewise-uniform
//! ramp: `0.5 dB` steps from `-60 dB` (index 0) to `-30 dB`
//! (index 60), `0.25 dB` steps to `-15 dB` (index 120), and
//! `0.125 dB` steps to `0 dB` (index 240) — except index 216, which
//! the spec prints as the exact half-power point `0.707107`
//! (`1/sqrt(2)`, i.e. `-3.0103 dB`) rather than `10^(-3/20)`.
//!
//! Two §5.7 consumers feed this table:
//!
//! - §5.7.1 Table 5-31 dynamic downmix coefficients: each 9-bit
//!   `panDwnMixCodeCoeffs[n]` code word carries a phase bit in the
//!   MSB (`1` → in phase `+1`, `0` → out of phase `-1`) and an 8-bit
//!   biased table index in the low bits (`0` → the coefficient is
//!   exactly `0.0` — "-Infinity is not part of the table" — else
//!   `DmixTable[index - 1]`). [`decode_dmix_code`] implements that
//!   resolution.
//! - §5.7.2 Table 5-33 `nEmbESDownMixScaleIndex`: a plain 8-bit
//!   `DmixTable[]` index whose encode-side range is limited to
//!   `40..=240` (`[-40 dB, 0 dB]`).
//!
//! This module is feature-independent (no `oxideav-core` dep), so it
//! is available under both the default and `--no-default-features`
//! build modes.

use crate::{Error, Result};

/// Number of entries in the §D.11 `DmixTable` (`DmixTblIndex`
/// `0..=240`).
pub const DMIX_TABLE_LEN: usize = 241;

/// §D.11 `DmixTable` column: unsigned 16-bit Q15 downmix scale
/// factors (`AbsValues * 2^15`, rounded to nearest), indexed by
/// `DmixTblIndex` `0..=240`. Entry 0 is `-60 dB` (`33`), entry 240
/// is unity (`32768`).
///
/// Transcribed from ETSI TS 102 114 V1.3.1 §D.11 (PDF p.256-259).
pub static DMIX_TABLE: [u16; DMIX_TABLE_LEN] = [
    33, 35, 37, 39, 41, 44, 46, 49, 52, 55, // 0..=9
    58, 62, 65, 69, 73, 78, 82, 87, 92, 98, // 10..=19
    104, 110, 116, 123, 130, 138, 146, 155, 164, 174, // 20..=29
    184, 195, 207, 219, 232, 246, 260, 276, 292, 309, // 30..=39
    328, 347, 368, 389, 413, 437, 463, 490, 519, 550, // 40..=49
    583, 617, 654, 693, 734, 777, 823, 872, 924, 978, // 50..=59
    1036, 1066, 1098, 1130, 1163, 1197, 1232, 1268, 1305, 1343, // 60..=69
    1382, 1422, 1464, 1506, 1550, 1596, 1642, 1690, 1740, 1790, // 70..=79
    1843, 1896, 1952, 2009, 2068, 2128, 2190, 2254, 2320, 2388, // 80..=89
    2457, 2529, 2603, 2679, 2757, 2838, 2920, 3006, 3093, 3184, // 90..=99
    3277, 3372, 3471, 3572, 3677, 3784, 3894, 4008, 4125, 4246, // 100..=109
    4370, 4497, 4629, 4764, 4903, 5046, 5193, 5345, 5501, 5662, // 110..=119
    5827, 5912, 5997, 6084, 6172, 6262, 6353, 6445, 6538, 6633, // 120..=129
    6729, 6827, 6925, 7026, 7128, 7231, 7336, 7442, 7550, 7659, // 130..=139
    7771, 7883, 7997, 8113, 8231, 8350, 8471, 8594, 8719, 8845, // 140..=149
    8973, 9103, 9235, 9369, 9505, 9643, 9783, 9924, 10068, 10214, // 150..=159
    10362, 10512, 10665, 10819, 10976, 11135, 11297, 11460, 11627, 11795, // 160..=169
    11966, 12139, 12315, 12494, 12675, 12859, 13045, 13234, 13426, 13621, // 170..=179
    13818, 14018, 14222, 14428, 14637, 14849, 15064, 15283, 15504, 15729, // 180..=189
    15957, 16188, 16423, 16661, 16902, 17147, 17396, 17648, 17904, 18164, // 190..=199
    18427, 18694, 18965, 19240, 19519, 19802, 20089, 20380, 20675, 20975, // 200..=209
    21279, 21587, 21900, 22218, 22540, 22867, 23170, 23534, 23875, 24221, // 210..=219
    24573, 24929, 25290, 25657, 26029, 26406, 26789, 27177, 27571, 27970, // 220..=229
    28376, 28787, 29205, 29628, 30057, 30493, 30935, 31383, 31838, 32300, // 230..=239
    32768, // 240
];

/// Look up the §D.11 `DmixTable` scale factor for a `DmixTblIndex`,
/// returned as the real-valued gain (`DmixTable[index] / 2^15`).
///
/// Returns `None` when `index > 240` (outside the printed table).
#[must_use]
pub fn dmix_scale(index: usize) -> Option<f64> {
    DMIX_TABLE.get(index).map(|&q15| f64::from(q15) / 32768.0)
}

/// Resolve one §5.7.1 Table 5-31 9-bit dynamic-downmix coefficient
/// code word (`panDwnMixCodeCoeffs[n]`) to its real-valued
/// coefficient.
///
/// Per the Table 5-31 pseudocode:
///
/// ```text
/// nSign = ( nTmp & nMask1 ) ? 1 : -1;   // MSB: 1 -> in phase (+1)
/// nTmp  = (nTmp & nMask2);              // low 8 bits: biased index
/// if (nTmp > 0) {
///   nTmp--;              // -Infinity is not part of the table
///   if (nTmp > nTblSize)
///     return false;
///   m_panCoreDwnMixCoeffs[n] = (nSign * DmixCoeffTable[nTmp]);
/// } else
///   m_panCoreDwnMixCoeffs[n] = 0.0;
/// ```
///
/// A zero low-byte therefore encodes an exact `0.0` (the muted
/// channel pairing), and any other value is a one-biased
/// [`DMIX_TABLE`] index carrying the phase in bit 8.
///
/// # Errors
///
/// Returns [`Error::DownmixCodeOutOfRange`] when the code is wider
/// than 9 bits or its unbiased index walks past the end of the
/// 241-entry table (the pseudocode's `return false` arm).
pub fn decode_dmix_code(code: u16) -> Result<f64> {
    if code >= 1 << 9 {
        return Err(Error::DownmixCodeOutOfRange { code });
    }
    let sign = if code & 0x100 != 0 { 1.0 } else { -1.0 };
    let biased = (code & 0xFF) as usize;
    if biased == 0 {
        return Ok(0.0);
    }
    let index = biased - 1;
    if index >= DMIX_TABLE_LEN {
        return Err(Error::DownmixCodeOutOfRange { code });
    }
    Ok(sign * f64::from(DMIX_TABLE[index]) / 32768.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The informative §D.11 `LogAbsValues (dB)` ramp: `0.5 dB` steps
    /// to index 60, `0.25 dB` steps to index 120, `0.125 dB` steps to
    /// index 240.
    fn spec_db(index: usize) -> f64 {
        if index <= 60 {
            -60.0 + 0.5 * index as f64
        } else if index <= 120 {
            -30.0 + 0.25 * (index - 60) as f64
        } else {
            -15.0 + 0.125 * (index - 120) as f64
        }
    }

    #[test]
    fn dmix_table_matches_db_ramp_closed_form() {
        // Every DmixTable entry is round(10^(dB/20) * 2^15) on the
        // piecewise dB ramp — except index 216, which the spec prints
        // as the exact half-power point 1/sqrt(2) (-3.0103 dB) rather
        // than 10^(-3/20).
        for (i, &q15) in DMIX_TABLE.iter().enumerate() {
            let abs = if i == 216 {
                std::f64::consts::FRAC_1_SQRT_2
            } else {
                10f64.powf(spec_db(i) / 20.0)
            };
            let predicted = (abs * 32768.0).round() as u16;
            assert_eq!(q15, predicted, "DmixTable[{i}]");
        }
    }

    #[test]
    fn decode_dmix_code_phase_and_bias() {
        // Low byte 0 -> exact 0.0 regardless of the phase bit.
        assert_eq!(decode_dmix_code(0x000).unwrap(), 0.0);
        assert_eq!(decode_dmix_code(0x100).unwrap(), 0.0);
        // Biased index 241 -> DmixTable[240] = unity; MSB set -> +1.
        assert_eq!(decode_dmix_code(0x100 | 241).unwrap(), 1.0);
        // MSB clear -> out of phase (-1).
        assert_eq!(decode_dmix_code(241).unwrap(), -1.0);
        // Biased index 1 -> DmixTable[0] = 33 (Q15).
        let quietest = decode_dmix_code(0x100 | 1).unwrap();
        assert!((quietest - 33.0 / 32768.0).abs() < 1e-12);
    }

    #[test]
    fn decode_dmix_code_rejects_out_of_domain() {
        // Biased index 242 -> unbiased 241, past the table end.
        assert_eq!(
            decode_dmix_code(242),
            Err(Error::DownmixCodeOutOfRange { code: 242 })
        );
        assert_eq!(
            decode_dmix_code(0x100 | 255),
            Err(Error::DownmixCodeOutOfRange { code: 0x1FF })
        );
        // Wider than the 9-bit field.
        assert_eq!(
            decode_dmix_code(0x200),
            Err(Error::DownmixCodeOutOfRange { code: 0x200 })
        );
    }
}
