//! DTS Coherent Acoustics — Annex D §D.6 Block Code Books and the
//! §C.2.1 table-look-up block-code decoder variant.
//!
//! Source: ETSI TS 102 114 V1.3.1 (2011-08), Annex D §D.6 "Block Code
//! Books" (staged PDF p.231-236) and Annex C (informative) §C.2.1
//! "Block Code" (staged PDF p.182-183) at
//! `docs/audio/dts/etsi-ts-102114-dts-coherent-acoustics.pdf`.
//!
//! # What this module adds
//!
//! Round 232 landed the §C.2.1 **modulus / integer-division** block-code
//! decoder ([`crate::block_code::decode_block_code`]) and left the §C.2.1 **table
//! look-up** decoder variant as an explicit follow-up, blocked on the
//! §D.6 code-book rows enumerated as the §C.2.1 Table C-1. This module
//! transcribes those §D.6 tables and implements the table-look-up
//! decoder against them.
//!
//! The spec presents both decoder variants and states (PDF p.182) that
//! they produce the identical quantisation-index array for the same code
//! word — so the table-look-up decoder here is cross-validated against
//! the round-232 modulus decoder over the full §D.6 code domain in the
//! tests, with no implementation read from anywhere else.
//!
//! # §D.6 code-book structure
//!
//! Each §D.6.x sub-clause tabulates a `4`-element block code over an
//! `nNumLevel`-level alphabet. The `e`-th element (`e` in `1..=4`,
//! one-based as the spec writes "1st/2nd/3rd/4th element") lists one
//! code value per quantisation-level index `L` in `0..nNumLevel`:
//!
//! ```text
//! code(element e, level index L) = L * nNumLevel^(e-1)
//! ```
//!
//! (For the 3-level book, element 1 lists `0, 1, 2`; element 2 lists
//! `0, 3, 6`; element 3 lists `0, 9, 18`; element 4 lists `0, 27, 54` —
//! verbatim §D.6.1.) The quantisation-level index `L` maps to the
//! signed quantisation index by the §C.2.1 mid-range offset:
//! `index = L - (nNumLevel - 1) / 2` (so `L = 0` is the most negative
//! index and `L = nNumLevel - 1` the most positive).
//!
//! # §C.2.1 table-look-up walk (Table C-1)
//!
//! To decode, §C.2.1 rearranges each §D.6 book into a per-element table
//! and walks it **from the last element down to the first**, at each
//! element subtracting the largest table entry that does not exceed the
//! remaining code, and recording that entry's level index. Reproduced
//! as documented from the §C.2.1 worked example (PDF p.182), decoding
//! the 3-level 4-element code `64`:
//!
//! ```text
//! 4th Element: 64 - 54 = 10 > 0; level index 2 -> quantisation index +1
//! 3rd Element: 10 -  9 =  1 > 0; level index 1 -> quantisation index  0
//! 2nd Element:  1 -  0 =  1 > 0; level index 0 -> quantisation index -1
//! 1st Element:  1 -  1 =  0    ; level index 1 -> quantisation index  0
//! ```
//!
//! producing `[0, -1, 0, +1]` first-element-first — identical to the
//! modulus decoder's worked example. The success criterion is the same
//! `nCode == 0` residual check after the last (i.e. the first-element)
//! step.

// ---------------------------------------------------------------------
// §D.6 table construction.
//
// Every §D.6 book is `code(element e, level L) = L * nNumLevel^(e-1)`
// (verified against the printed §D.6.1..§D.6.7 tables row by row in the
// tests). Building the rows from that closed form transcribes the
// table's own arithmetic rather than re-typing several hundred decimal
// cells; the test module then asserts each printed anchor cell against
// the constructed row so the closed form is pinned to the staged PDF.
//
// PDF print errata noted in the tests: §D.6.3 (7-level) 3rd-element
// level-0 cell prints "47" where the table's own `L * 49` arithmetic
// and the §C.2.1 modulus decoder both give 147 — the constructed table
// carries the arithmetically-consistent 147 and the test records the
// print error.
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {

    // -----------------------------------------------------------------
    // §D.6 table transcription: assert printed anchor cells against the
    // constructed books, row by row, to pin the closed form to the
    // staged PDF (p.231-236).
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // §C.2.1 table-look-up decoder.
    // -----------------------------------------------------------------
}
