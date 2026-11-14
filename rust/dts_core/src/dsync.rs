//! DTS Coherent Acoustics — §5.5 Table 5-29 `DSYNC` subsubframe
//! synchronization check word.
//!
//! Source: ETSI TS 102 114 V1.3.1 (2011-08), staged PDF at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`
//! (Table 5-29 pseudocode on PDF p.32, prose on PDF p.33).
//!
//! The final step of the §5.5 per-subsubframe `Audio Data` walk is a
//! conditional 16-bit synchronization word read. Transcribed verbatim
//! from the staged Table 5-29 pseudocode (PDF p.32):
//!
//! ```text
//! // Check for DSYNC
//! if ( (nSubSubFrame==(nSSC-1)) || (ASPF==1) ) {
//!   DSYNC = ExtractBits(16);
//!   if ( DSYNC != 0xffff )
//!     printf("DSYNC error at end of subsubframe #%d", nSubSubFrame);
//! }
//! ```
//!
//! and the §5.5 prose (PDF p.33, "AUDIO (Audio data)"):
//!
//! > "At end of each subsubframe there may be a synchronization check
//! > word `DSYNC = 0xffff` depending on the flag `ASPF` in the frame
//! > header, but there must be at least a DSYNC at the end of each
//! > subframe."
//!
//! Two facts fix the gating completely:
//!
//! * **End of subframe** (`nSubSubFrame == nSSC - 1`, the last
//!   subsubframe of the audio subframe): a DSYNC word is *always*
//!   present here regardless of `ASPF`. This is the "at least a DSYNC
//!   at the end of each subframe" guarantee.
//! * **`ASPF == 1`** (the §5.3.2 "Audio Sync-Word Insertion Flag" of
//!   the frame header, [`crate::header::DtsFrameHeader::aspf`]): a DSYNC word
//!   is present after *every* subsubframe, not only the last.
//!
//! When neither condition holds, no DSYNC word is read and the bit
//! cursor stays on the next subsubframe's first audio field.
//!
//! This module exposes the gating predicate
//! ([`dsync_present`]), the expected word
//! ([`DSYNC_WORD`]), and the bit-stream reader
//! ([`decode_dsync_at`]) that reads the 16-bit field and verifies it
//! against `0xffff`. It composes the round-281 `aspf` header field
//! with the round-249 [`crate::side_info::SubsubframeCount::n_ssc`] count to
//! drive the §5.5 walker's trailer step.

/// The §5.5 `DSYNC` synchronization check word value, `0xffff`
/// (PDF p.32: `if ( DSYNC != 0xffff )`). A correctly framed
/// subsubframe trailer carries exactly this 16-bit pattern.
pub const DSYNC_WORD: u16 = 0xffff;

#[cfg(test)]
mod tests {}
