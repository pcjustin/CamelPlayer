//! §5.6 "Unpack Optional Information" (Table 5-30): the flag-gated
//! region that follows the last audio-data array of a Core frame.
//!
//! Transcribed from ETSI TS 102 114 V1.3.1 (2011-08) §5.6
//! (Table 5-30 + field descriptions, PDF p.33-34), staged at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`:
//!
//! ```text
//! if ( TIMEF==1 )               // Present only when TIMEF=1.
//!   TIMES = ExtractBits(32);
//! if ( AUXF==1 )                // Present only if AUXF=1.
//!   AUXCT = ExtractBits(6);
//! else
//!   AUXCT = 0;                  // Clear it.
//! ByteAlign = ExtractBits(0 ... 7);
//! for (int n=0; n<AUXCT; n++)
//!   AUXD[n] = ExtractBits(8);
//! if ( (CPF==1) && (DYNF!=0) )
//!   OCRC = ExtractBits(16);
//! ```
//!
//! The walker starts at the bit cursor left by the last §5.5
//! audio-data array — [`crate::subframe_pcm::SubframePcmDecoder`] exposes it through
//! the `*_with_info` decode entry points, which run this walk after
//! the reconstruction loop.
//!
//! Two Table 5-30 caveats, kept as documented in the staged spec:
//!
//! - The pseudocode's `ByteAlign` consumes `0..=7` bits (to the next
//!   byte boundary), while the `ZeroPadAux` field description says
//!   the auxiliary data bytes begin "on the 32-bit boundary from the
//!   beginning of the core stream". The two disagree whenever the
//!   byte-aligned cursor is not also DWORD-aligned; §5.7.1 resolves
//!   the tension by recommending navigation *into* the aux content
//!   via the DWORD-aligned sync-word search
//!   ([`crate::aux_data::find_aux_data`] / [`crate::aux_data::parse_aux_data`]) instead
//!   of the `AUXCT` cursor. This walker follows the pseudocode
//!   literally (byte alignment) and surfaces the raw `AUXD` bytes;
//!   use the §5.7.1 parser for their content.
//! - `OCRC`: "The CRC value test shall not be applied" — the Annex B
//!   algorithm is documented ([`crate::crc16::dts_crc16`],
//!   `docs/audio/dts/dts-crc16.md`) but the core check words are
//!   normatively untested placeholders, so the word is surfaced raw.

#[cfg(test)]
mod tests {}
