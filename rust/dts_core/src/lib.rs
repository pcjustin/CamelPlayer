//! The DTS Coherent Acoustics core decoder of CamelPlayer, for DTS-CD WAV files. No I/O: [`Decoder`] takes one frame's
//! bytes and returns float PCM, with the filter state kept across frames.
//!
//! Except for `stream.rs`, these modules are a pinned copy of `oxideav-dts` at `ec55712`, with its encoder and framework
//! glue left out; see `NOTICE.md` and `LICENSE-oxideav-dts`.

#![forbid(unsafe_code)]
#![warn(missing_debug_implementations)]
#![warn(missing_docs)]

mod audio_array;
mod audio_data;
mod audio_header;
mod audio_huff;
mod bitreader;
mod block_code;
mod cos_mod;
mod d10_tables;
mod d10_vq;
mod d6_block_book;
mod dsync;
mod filter_bank;
mod fir_coeff;
mod header;
mod inverse_adpcm;
mod iter;
mod join_scale;
mod joint_subband;
mod lfe_fir_coeff;
mod lfe_interp;
mod lfe_synth;
mod qmf_assemble;
mod qmf_multichannel;
mod qmf_synth;
mod side_info;
mod step_size;
mod stream;
mod subframe;
mod subframe_pcm;
mod unpack14;

// What the Engine, the tests and the fuzz target use.
pub use crate::header::{SyncWordEncoding, parse_frame_header};
pub use crate::stream::{
    DecodeError, Decoded, Decoder, FrameInfo, Speaker, Sync, find_sync, frame_info,
};
pub use crate::unpack14::{FourteenBitByteOrder, pack_16bit_to_14bit};

/// What parsing or decoding a frame can report.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The input buffer was too short for the field being read.
    UnexpectedEof,
    /// None of the four documented DTS sync words matched the first
    /// 4–5 bytes of the input.
    NoSync,
    /// A 14-bit DTS sync was detected at the 16-bit-input entry
    /// point [`parse_frame_header`]; 14-bit-packed frames are read
    /// through `parse_frame_header_14bit`, which unpacks them first.
    UnsupportedFourteenBit,
    /// The decoded `NBLKS` field reported fewer than 5 blocks per
    /// frame — the wiki/spec disallow this.
    BlockCountOutOfRange {
        /// Decoded number of blocks (after the +1 increment).
        blocks: u8,
    },
    /// The decoded frame-size field reported fewer than 95 bytes —
    /// the wiki/spec disallow this.
    FrameSizeOutOfRange {
        /// Decoded frame size in bytes (after the +1 increment).
        frame_size: u16,
    },
    /// A Primary Audio Coding Side Information field
    /// (§5.4.1 Table 5-28) carried a value the ETSI spec marks as
    /// reserved/invalid: a `BHUFF` / `SHUFF` selector equal to `7`,
    /// a `SCALES` accumulator that walked outside the documented
    /// range of the §D.1.1 / §D.1.2 RMS square-root table (e.g.
    /// index 63 in the 6-bit table, or indices 125..=127 in the
    /// 7-bit table, both written as "invalid" in the staged PDF),
    /// or a `decode_primary_side_info_at` loop bound
    /// outside its §5.3.2 range (`"nPCHS"` above 5 per PDF p.25,
    /// `"nSUBS"` above the `NumSubband = 32` cap, or `"VQSUB"`
    /// above the channel's `nSUBS`).
    InvalidSideInfo {
        /// Static name of the offending side-info field: `"BHUFF"`,
        /// `"SHUFF"`, `"SCALES"`, `"nPCHS"`, `"nSUBS"`, or
        /// `"VQSUB"`.
        field: &'static str,
        /// The reserved value the bit stream carried.
        value: u32,
    },
    /// The bit stream's prefix did not match any entry in the named
    /// Annex D Huffman codebook (`A12`, `B12`, `C12`, `D12`, `E12`,
    /// `A5`, `B5`, `C5`, `A7`, `B7`, `A4`, `B4`, `C4`, `D4`) within
    /// the maximum documented code length. Surfaced by the ABITS /
    /// SCALES / TMODE side-info decoders when the input is
    /// structurally corrupt or the wrong codebook was selected
    /// upstream.
    HuffmanDecodeFailed {
        /// Static name of the codebook that failed to match.
        table: &'static str,
    },
    /// A slice argument to the §C.2.2 inverse-ADPCM predictor
    /// (`inverse_adpcm_decode_f64`) had a length that disagrees
    /// with the spec's fixed `NumADPCMCoeff = 4` invariant. The
    /// predictor requires a four-sample history (carrying
    /// `raSample[-4..0]`) and four ADPCM coefficients
    /// (`raADPCMCoeff[0..4]`); either slice having any other length is
    /// out-of-domain.
    InverseAdpcmShapeMismatch {
        /// Caller-supplied history-buffer length (spec requires 4).
        history_len: usize,
        /// Caller-supplied coefficient-array length (spec requires 4).
        coeffs_len: usize,
    },
    /// The `n_levels` argument to `decode_block_code` was
    /// less than 2. A one-level alphabet has only the index `0` and
    /// the §C.2.1 mixed-radix recurrence is undefined for
    /// `n_levels < 2` (division by zero / one would never advance).
    BlockCodeLevelsOutOfRange {
        /// Caller-supplied `n_levels` value (spec requires `>= 2`).
        n_levels: u32,
    },
    /// A §C.2.1 block-code word produced a non-zero residual after
    /// consuming every output element via the spec's
    /// `nCode % nNumLevel` / `nCode /= nNumLevel` recurrence. The
    /// spec text treats this as a fatal "ERROR: block code look-up
    /// fail" condition (PDF p.183); the Rust API surfaces it as a
    /// recoverable error carrying the residual + block dimensions
    /// so the caller can distinguish bit-stream corruption from a
    /// structural decoder bug.
    BlockCodeResidual {
        /// The residual code-word value after the last extraction.
        residual: u32,
        /// Element count passed to the decoder (`output.len()`).
        n_elements: usize,
        /// Quantisation-level count passed to the decoder.
        n_levels: u32,
    },
    /// An `ABITS` bit-allocation index passed to the §D.2
    /// quantization-step-size lookup (`StepSizeTable::step_size`) was
    /// one of the `27..=31` indices the
    /// staged §D.2.1 / §D.2.2 tables write "invalid" (PDF p.193-194),
    /// or was `>= 32`. A defined `ABITS` index is `0..=26`.
    InvalidStepSize {
        /// The reserved / out-of-range `ABITS` index.
        abits: u8,
    },
    /// The §5.5 Table 5-29 `DSYNC` subsubframe synchronization check
    /// word, read with the subsubframe's audio data, was a 16-bit value other than
    /// `0xffff` (PDF p.32: `if ( DSYNC != 0xffff )`). The spec text only
    /// emits a diagnostic and keeps decoding; this crate surfaces it as
    /// a recoverable typed error carrying the bad word and the
    /// zero-based subsubframe index it trailed, mirroring the spec's
    /// `"DSYNC error at end of subsubframe #%d"` message — it is the
    /// only in-band integrity check the Core profile provides for the
    /// audio-data array.
    DsyncMismatch {
        /// The 16-bit value read where `0xffff` was expected.
        found: u16,
        /// The zero-based `nSubSubFrame` index the trailer followed.
        n_subsubframe: u8,
    },
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Error::UnexpectedEof => write!(f, "oxideav-dts: unexpected end of input"),
            Error::NoSync => {
                write!(f, "oxideav-dts: no DTS sync word found at offset 0")
            }
            Error::UnsupportedFourteenBit => write!(
                f,
                "oxideav-dts: 14-bit DTS sync detected at the 16-bit-input \
                 entry point; call parse_frame_header_14bit (or \
                 unpack_14bit_to_16bit + parse_frame_header) instead"
            ),
            Error::BlockCountOutOfRange { blocks } => write!(
                f,
                "oxideav-dts: NBLKS={blocks} is out of the documented 5..=128 \
                 range (spec mandates >=5)"
            ),
            Error::FrameSizeOutOfRange { frame_size } => write!(
                f,
                "oxideav-dts: frame size {frame_size} B is out of the documented \
                 95..=16384 range (spec mandates >=95)"
            ),
            Error::InvalidSideInfo { field, value } => write!(
                f,
                "oxideav-dts: side-info field `{field}` value {value} is \
                 reserved/invalid per ETSI TS 102 114 §5.4.1 (Table 5-24/5-25 \
                 selector 7, or §D.1.1/§D.1.2 RMS table index marked invalid)"
            ),
            Error::HuffmanDecodeFailed { table } => write!(
                f,
                "oxideav-dts: bit stream did not match any entry in Annex D \
                 Huffman codebook `{table}` within the documented maximum \
                 code length"
            ),
            Error::InverseAdpcmShapeMismatch {
                history_len,
                coeffs_len,
            } => write!(
                f,
                "oxideav-dts: §C.2.2 inverse-ADPCM decode requires a 4-sample \
                 history and 4 ADPCM coefficients (NumADPCMCoeff = 4); got \
                 history_len={history_len} coeffs_len={coeffs_len}"
            ),
            Error::BlockCodeLevelsOutOfRange { n_levels } => write!(
                f,
                "oxideav-dts: §C.2.1 block-code decode requires n_levels >= 2; \
                 got n_levels={n_levels}"
            ),
            Error::BlockCodeResidual {
                residual,
                n_elements,
                n_levels,
            } => write!(
                f,
                "oxideav-dts: §C.2.1 block-code decode residual {residual} != 0 \
                 after extracting {n_elements} element(s) from a base-{n_levels} \
                 block (code-word out of range for the declared block dimensions)"
            ),
            Error::InvalidStepSize { abits } => write!(
                f,
                "oxideav-dts: §D.2 quantization step-size lookup: ABITS index \
                 {abits} is reserved/invalid (defined range is 0..=26)"
            ),
            Error::DsyncMismatch {
                found,
                n_subsubframe,
            } => write!(
                f,
                "oxideav-dts: §5.5 DSYNC error at end of subsubframe #{n_subsubframe}: \
                 read 0x{found:04x}, expected 0xffff"
            ),
        }
    }
}

impl std::error::Error for Error {}

/// Convenience alias for [`Result`] specialised to this crate's
/// [`Error`].
pub type Result<T> = core::result::Result<T, Error>;
