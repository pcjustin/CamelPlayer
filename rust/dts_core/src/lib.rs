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
mod aux_data;
mod bitreader;
mod block_code;
mod cos_mod;
mod crc16;
#[doc(hidden)]
pub mod d10_tables;
mod d10_vq;
mod d6_block_book;
mod dmix_coeff;
mod drc_range;
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
mod optional_info;
mod qmf_assemble;
mod qmf_multichannel;
mod qmf_synth;
mod rev2_aux;
mod side_info;
mod step_size;
mod stream;
mod subframe;
mod subframe_pcm;
mod sum_diff;
#[cfg(test)]
mod test_util;
mod unpack14;

// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::audio_array::{
    AdpcmContext, AudioArrayDecodeError, AudioArrayError, HfVqFill, SubbandSampleMatrix,
    decode_audio_data_subframe_at, decode_audio_data_subframe_partial_at,
    decode_audio_data_subframe_vq_at, decode_lfe_phase_at,
};
// Stable: the persistent §C.2.2 reconstruction history a caller may
// inspect/reset around the recovered-§D.10 ADPCM decode path.
pub use crate::audio_array::AdpcmHistory;
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::audio_data::{
    ABITS_MAX_BLOCK_CODE, ABITS_MAX_SEL, ABITS_TABLE_LEN, AudioQuantType, CODEBOOK_GROUP_SIZE,
    QUANT_LEVELS, audio_quant_type, terminal_sel_index,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::audio_header::{AudioCodingHeader, SEL_PLANE_LEN, decode_audio_coding_header_at};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::audio_huff::{AudioHuffCodebook, decode_audio_huff_at};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::aux_data::{
    AUX_SYNC_WORD, AUX_TIME_STAMP_MARKER, AuxData, DownmixType, DynamicDownmix, find_aux_data,
    parse_aux_data, parse_aux_data_at,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::block_code::{block_code_max_code, block_code_offset, decode_block_code};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::cos_mod::{
    COS_MOD_BLOCK1_START, COS_MOD_BLOCK2_START, COS_MOD_BLOCK3_START, COS_MOD_BLOCK4_START,
    COS_MOD_LEN, NUM_SUBBAND, cos_mod_stage, precal_cos_mod,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::crc16::{
    DTS_CRC16_INIT, DTS_CRC16_POLY, DTS_CRC16_TABLE, dts_crc16, dts_crc16_update,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::d10_vq::{
    ADPCM_VQ_BOOK_SIZE, ADPCM_VQ_COEFF_DIVISOR, ADPCM_VQ_INDEX_BITS, ADPCM_VQ_VECTOR_LEN,
    HFREQ_VQ_BOOK_SIZE, HFREQ_VQ_ELEMENT_DIVISOR, HFREQ_VQ_ENTRIES_PER_VECTOR, HFREQ_VQ_INDEX_BITS,
    HFREQ_VQ_VECTOR_LEN, adpcm_vq_coeff, scan_hf_vq_indices_at, unpack_hfreq_vq_entry,
};
// Stable: the §D.10 VQ code books. `VqCodebooks::builtin()` (the
// decoder default) carries the real books, transcribed from the
// staged clean-room tables; the caller-supplied constructors remain.
pub use crate::d10_vq::{AdpcmVqCodebook, HfVqCodebook, VqCodebookShapeError, VqCodebooks};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::d6_block_book::{
    D6_BLOCK_ELEMENTS, D6_BOOK_3, D6_BOOK_5, D6_BOOK_7, D6_BOOK_9, D6_BOOK_13, D6_BOOK_17,
    D6_BOOK_25, D6BlockBook, d6_book_for_levels, decode_block_code_table,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::dmix_coeff::{
    DMIX_TABLE, DMIX_TABLE_LEN, DMIX_TABLE_UNITY_INDEX, INV_DMIX_INDEX_OFFSET, INV_DMIX_TABLE,
    INV_DMIX_TABLE_LEN, decode_dmix_code, dmix_scale, inv_dmix_scale,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::drc_range::{
    DRC_RANGE_LEN, DRC_RANGE_MULTIPLIER, DRC_RANGE_UNITY_INDEX, drc_range, dts_dynrng_to_db,
    dts_dynrng_to_linear,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::dsync::{DSYNC_WIRE_BITS, DSYNC_WORD, decode_dsync_at, dsync_present};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::filter_bank::FilterBankSelection;
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::fir_coeff::{FIR_COEFF_LEN, RA_COEFF_LOSSLESS, RA_COEFF_LOSSY};
pub use crate::header::{
    AmodeArrangement, DialogNormalization, DtsFrameHeader, FrameType, LfeMode, SampleFrequency,
    SourcePcmResolution, SyncWordEncoding, TargetedBitRate, encode_frame_header_14bit_be,
    encode_frame_header_14bit_le, encode_frame_header_be, encode_frame_header_le,
    parse_frame_header, parse_frame_header_14bit,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::inverse_adpcm::{
    NUM_ADPCM_COEFF, inverse_adpcm_decode_f64, inverse_adpcm_decode_i32, inverse_adpcm_required,
    update_history_f64, update_history_i32,
};
pub use crate::iter::{
    FrameIterator, FrameIterator14, FrameIteratorResync, FrameView, FrameView14, ResyncCause,
    ResyncEvent, SyncIterator, SyncMatch, find_all_syncs, find_next_sync, iter_frames,
    iter_frames_14bit, iter_frames_resync, iter_syncs,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::join_scale::{
    JOIN_SCALE_FACTOR, JOIN_SCALE_LEN, JOIN_SCALE_UNITY_INDEX, join_scale,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::joint_subband::{
    joint_source_channel, joint_subband_decode_range_f64, joint_subband_decode_range_i32,
    joint_subband_required,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::lfe_fir_coeff::{LFE_FIR_COEFF_LEN, RA_COEFF_LFE64, RA_COEFF_LFE128};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::lfe_interp::LfeInterpolationSelection;
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::lfe_synth::{
    LFE_HISTORY_LEN, LFE_OUTPUT_CALIBRATION, LFE_SCALE_STEP, LfeChannel, LfeChannelError,
    LfeInterpError, LfeInterpolator,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::optional_info::{MAX_AUX_BYTE_COUNT, OptionalInfo, decode_optional_info_at};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::qmf_assemble::{
    PCM_OUTPUT_PER_SAMPLE, QmfAssembleError, X_HISTORY_LEN, Z_OUTPUT_LEN, assemble_xin, fir_step,
    shift_x_history, shift_z_output, write_pcm_output,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::qmf_multichannel::{MultiChannelQmf, MultiChannelQmfError};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::qmf_synth::QmfSynthesis;
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::rev2_aux::{
    REV2_AUX_SYNC_WORD, REV2_DRC_VERSION_SINGLE_BAND, Rev2AuxChunk, Rev2Drc, find_rev2_aux,
    parse_rev2_aux, parse_rev2_aux_at,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::side_info::{
    AbitsCodebook, RMS_6BIT, RMS_7BIT, ScaleFactorAdjustment, ScalesCodebook, SubsubframeCount,
    TmodeCodebook, decode_abits_at, decode_adj_at, decode_join_scale_at, decode_scales_at,
    decode_subsubframe_count_at, decode_tmode_at, scales_code,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::step_size::{
    RATE_LOSSLESS, SAMPLES_PER_SUBSUBFRAME, STEP_SIZE_FIRST_INVALID, STEP_SIZE_LOSSLESS,
    STEP_SIZE_LOSSY, STEP_SIZE_SCALE_SHIFT, STEP_SIZE_TABLE_LEN, StepSizeTable, dequant_scale,
    dequant_subsubframe, scale_subsubframe_samples, transient_scale_index,
};
// internal — exposed for tests/fuzz; not part of the stable API
#[doc(hidden)]
pub use crate::subframe::{
    ChannelSideInfo, ChannelSideInfoParams, MAX_PRIMARY_CHANNELS, PrimarySideInfo, SideInfoTail,
    decode_primary_side_info_at, decode_primary_side_info_tail_at,
};
pub use crate::subframe_pcm::{
    CoreFrameDecodeError, CoreStreamDecoder, OUTPUT_LEVEL_CALIBRATION, PCM_PER_SUBBAND_ROW,
    Subframe, SubframePcm, SubframePcmDecoder, SubframePcmError, decode_core_frame,
    decode_core_frame_with_info,
};
// internal — exposed for tests/fuzz; not part of the stable API
pub use crate::stream::{
    DecodeError, Decoded, Decoder, FrameInfo, Speaker, Sync, find_sync, frame_info,
};
#[doc(hidden)]
pub use crate::sum_diff::{
    front_sum_difference_required, sum_difference_decode_f64, sum_difference_decode_i32,
    sum_difference_decode_subband_pair_f64, sum_difference_decode_subband_pair_i32,
    surround_sum_difference_required,
};
pub use crate::unpack14::{FourteenBitByteOrder, pack_16bit_to_14bit, unpack_14bit_to_16bit};

/// Crate-local error type. Round 1 surfaces only the parser-related
/// variants; future rounds will extend this enum as decoding stages
/// land.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum Error {
    /// The input buffer was too short for the field being read.
    UnexpectedEof,
    /// None of the four documented DTS sync words matched the first
    /// 4–5 bytes of the input.
    NoSync,
    /// A 14-bit DTS sync was detected at the 16-bit-input entry
    /// point [`parse_frame_header`]. Round 2 added a dedicated
    /// [`parse_frame_header_14bit`] entry point plus
    /// [`unpack_14bit_to_16bit`] for callers that want to convert
    /// 14-bit-packed bytes into the raw-BE form. This variant
    /// remains for callers that route by sync up-front.
    UnsupportedFourteenBit,
    /// A raw 16-bit DTS sync was detected at the 14-bit-input entry
    /// point [`crate::iter_frames_14bit`]. Symmetric counterpart to
    /// [`Self::UnsupportedFourteenBit`]: the 14-bit iterator only
    /// walks 14-bit-packed container streams, so a raw-16-bit sync at
    /// the cursor is out-of-domain. Callers walking raw 16-bit input
    /// should switch to [`crate::iter_frames`].
    UnsupportedRaw16Bit,
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
    /// A field passed to [`crate::encode_frame_header_be`] does not
    /// fit the bit-width the wiki bit-table documents (e.g. AMODE > 63
    /// for a 6-bit field, VERSION > 15 for a 4-bit field, or a
    /// `header_crc: Some(_)` paired with `crc_present == false`).
    /// Only the encoder returns this variant; the parser cannot
    /// produce out-of-range values because every field is read from
    /// the bit-vector through a width-bounded read.
    FieldOutOfRange {
        /// Static name of the offending [`crate::DtsFrameHeader`]
        /// field.
        field: &'static str,
        /// Caller-supplied value.
        value: u32,
        /// Maximum value the field's documented bit-width can hold.
        /// For the `header_crc` mismatch variant this is set to `0`
        /// (the value being out-of-range is the `Some` vs `None`
        /// disagreement with `crc_present`, not the integer payload).
        max: u32,
    },
    /// A Primary Audio Coding Side Information field
    /// (§5.4.1 Table 5-28) carried a value the ETSI spec marks as
    /// reserved/invalid: a `BHUFF` / `SHUFF` selector equal to `7`,
    /// a `SCALES` accumulator that walked outside the documented
    /// range of the §D.1.1 / §D.1.2 RMS square-root table (e.g.
    /// index 63 in the 6-bit table, or indices 125..=127 in the
    /// 7-bit table, both written as "invalid" in the staged PDF),
    /// or a [`crate::decode_primary_side_info_at`] loop bound
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
    /// The left and right slice arguments to a §C.2.4 sum/difference
    /// decoder ([`crate::sum_difference_decode_i32`],
    /// [`crate::sum_difference_decode_f64`], or one of their
    /// subband-pair counterparts) had different lengths. The §C.2.4
    /// pseudocode requires a one-to-one pairing of left- and right-
    /// channel samples.
    SumDiffLengthMismatch {
        /// Length of the left-channel slice.
        left_len: usize,
        /// Length of the right-channel slice.
        right_len: usize,
    },
    /// The slice arguments to a §C.2.3 joint-subband decoder
    /// ([`crate::joint_subband_decode_range_i32`] or
    /// [`crate::joint_subband_decode_range_f64`]) violated one of the
    /// §C.2.3 pseudocode's structural invariants: `n_subs_dst >
    /// n_subs_src` (the imported range would run backwards), a
    /// per-channel subband array shorter than `n_subs_src` (no
    /// storage for the imported range), a `scales` slice whose length
    /// disagrees with `n_subs_src - n_subs_dst`, or a per-subband
    /// destination/source sample-length disagreement. `dst_len` /
    /// `src_len` carry the lengths that disagreed (the meaning is
    /// context-dependent: see the documented invariants on each
    /// constructor site).
    JointSubbandShapeMismatch {
        /// Length-like value on the destination side of the disagreement.
        dst_len: usize,
        /// Length-like value on the source side of the disagreement.
        src_len: usize,
    },
    /// A slice argument to a §C.2.2 inverse-ADPCM decoder
    /// ([`crate::inverse_adpcm_decode_i32`] or
    /// [`crate::inverse_adpcm_decode_f64`]) had a length that disagrees
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
    /// The `n_levels` argument to [`crate::decode_block_code`] was
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
    /// quantization-step-size lookup ([`crate::StepSizeTable::step_size`]
    /// or a `dequant_*` composer) was one of the `27..=31` indices the
    /// staged §D.2.1 / §D.2.2 tables write "invalid" (PDF p.193-194),
    /// or was `>= 32`. A defined `ABITS` index is `0..=26`.
    InvalidStepSize {
        /// The reserved / out-of-range `ABITS` index.
        abits: u8,
    },
    /// A slice argument to a §5.5 subsubframe dequantizer
    /// ([`crate::scale_subsubframe_samples`] /
    /// [`crate::dequant_subsubframe`]) was not exactly
    /// [`crate::SAMPLES_PER_SUBSUBFRAME`] (= 8) samples long. The §5.5
    /// `Audio Data` block scales one subband analysis subwindow of
    /// eight samples per call (PDF p.31-32).
    SampleCountMismatch {
        /// The number of samples the §5.5 loop requires (always 8).
        expected: usize,
        /// The slice length the caller actually supplied.
        found: usize,
    },
    /// The bytes at the offset handed to
    /// [`crate::parse_aux_data_at`] were not the §5.7.1 DWORD-aligned
    /// auxiliary-data sync word `0x9A1105A0` (`nSYNCAUX`).
    AuxSyncMismatch {
        /// The 32-bit word actually read at the offset.
        found: u32,
    },
    /// One of the two 4-bit markers bracketing the §5.7.1 Table 5-31
    /// 36-bit `nAUXTimeStamp` halves was not the documented `0b1011`
    /// (`nMaker==1011`), indicating a false-positive sync match or a
    /// corrupt auxiliary-data chunk.
    AuxTimeStampMarkerMismatch {
        /// The 4-bit value actually read.
        found: u8,
    },
    /// The §5.7.1 `DeriveNumDwnMixCodeCoeffs()` input-channel count
    /// (`nPriCh = anNumCh[AMODE]`) could not be resolved because the
    /// frame's `AMODE` is a user-defined code (`16..=63`) whose
    /// channel count Table 5-4 does not define.
    AuxChannelCountUnresolved {
        /// The unresolvable 6-bit `AMODE` code.
        amode: u8,
    },
    /// The bytes at the offset handed to
    /// [`crate::parse_rev2_aux_at`] were not the §5.7.2 DWORD-aligned
    /// Rev2 auxiliary sync word `0x7004C070` (`nSYNCRev2AUX`).
    Rev2AuxSyncMismatch {
        /// The 32-bit word actually read at the offset.
        found: u32,
    },
    /// The §5.7.2 `nRev2AUXDataByteSize` field was outside its valid
    /// `3..=128` range ("Error: Invalid range of Rev 2 Auxiliary Data
    /// Chunk Size"), or the chunk's declared fields overran the
    /// size-located `nRev2AUXCRC16` position.
    Rev2AuxSizeOutOfRange {
        /// The decoded chunk byte size (after the `+1` increment).
        size: u8,
    },
    /// The §5.7.2 `nEmbESDownMixScaleIndex` was outside its valid
    /// `40..=240` range (the encode side limits the `ESDmixScale`
    /// parameters to `[-40 dB, 0 dB]`).
    Rev2AuxEsScaleIndexOutOfRange {
        /// The out-of-range 8-bit index.
        index: u8,
    },
    /// The §5.7.2 per-subsubframe DRC value count could not be
    /// derived: one 8-bit value is transmitted per 256-sample
    /// subsubframe (Table 5-34), but the frame's block count is not a
    /// whole number of subsubframes (`32·(NBLKS+1)` not a multiple of
    /// 256).
    Rev2AuxDrcCountUnresolved {
        /// The frame's `NBLKS + 1` block count.
        blocks: u8,
    },
    /// A §5.7.1 Table 5-31 dynamic-downmix coefficient code word
    /// passed to [`crate::decode_dmix_code`] was wider than the
    /// documented 9-bit field, or its one-biased low byte resolved
    /// past the end of the 241-entry §D.11 `DmixTable` (the
    /// pseudocode's `if (nTmp > nTblSize) return false;` arm).
    DownmixCodeOutOfRange {
        /// The offending 9-bit (or wider) code word.
        code: u16,
    },
    /// The planar PCM handed to
    /// [`crate::DynamicDownmix::apply_planar`] did not match the
    /// coefficient table's shape: the plane count must equal the
    /// §5.7.1 `nPriCh` input channel count, and every plane must have
    /// the same sample count.
    DownmixInputShapeMismatch {
        /// The expected plane count (or, for the unequal-length case,
        /// the first plane's sample count).
        expected: usize,
        /// The offending count actually supplied.
        found: usize,
    },
    /// The §5.5 Table 5-29 `DSYNC` subsubframe synchronization check
    /// word ([`crate::decode_dsync_at`]) read a 16-bit value other than
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
            Error::UnsupportedRaw16Bit => write!(
                f,
                "oxideav-dts: raw 16-bit DTS sync detected at the \
                 14-bit-input entry point; call iter_frames (or \
                 parse_frame_header) instead"
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
            Error::FieldOutOfRange { field, value, max } => write!(
                f,
                "oxideav-dts: field `{field}` value {value} exceeds the wiki \
                 bit-table maximum {max}"
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
            Error::SumDiffLengthMismatch {
                left_len,
                right_len,
            } => write!(
                f,
                "oxideav-dts: §C.2.4 sum/difference decode requires matched \
                 left/right slice lengths; got left={left_len} right={right_len}"
            ),
            Error::JointSubbandShapeMismatch { dst_len, src_len } => write!(
                f,
                "oxideav-dts: §C.2.3 joint-subband decode shape mismatch; got \
                 dst-side={dst_len} src-side={src_len} (see ETSI TS 102 114 \
                 §C.2.3 for the structural invariants)"
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
            Error::SampleCountMismatch { expected, found } => write!(
                f,
                "oxideav-dts: §5.5 subsubframe dequantization expects {expected} \
                 samples per subband analysis subwindow, got {found}"
            ),
            Error::AuxSyncMismatch { found } => write!(
                f,
                "oxideav-dts: §5.7.1 auxiliary-data parse expected the \
                 DWORD-aligned sync word 0x9a1105a0, read 0x{found:08x}"
            ),
            Error::AuxTimeStampMarkerMismatch { found } => write!(
                f,
                "oxideav-dts: §5.7.1 auxiliary time-stamp marker mismatch: \
                 read 0b{found:04b}, expected 0b1011"
            ),
            Error::AuxChannelCountUnresolved { amode } => write!(
                f,
                "oxideav-dts: §5.7.1 dynamic-downmix coefficient count cannot \
                 be derived: AMODE {amode} is a user-defined channel \
                 arrangement with no Table 5-4 channel count"
            ),
            Error::Rev2AuxSyncMismatch { found } => write!(
                f,
                "oxideav-dts: §5.7.2 Rev2 auxiliary-data parse expected the \
                 DWORD-aligned sync word 0x7004c070, read 0x{found:08x}"
            ),
            Error::Rev2AuxSizeOutOfRange { size } => write!(
                f,
                "oxideav-dts: §5.7.2 Rev2 auxiliary chunk size {size} B is \
                 invalid (valid range 3..=128, and the declared fields must \
                 fit in front of the size-located CRC)"
            ),
            Error::Rev2AuxEsScaleIndexOutOfRange { index } => write!(
                f,
                "oxideav-dts: §5.7.2 embedded-ES downmix scale index {index} \
                 is outside the valid 40..=240 range ([-40 dB, 0 dB])"
            ),
            Error::Rev2AuxDrcCountUnresolved { blocks } => write!(
                f,
                "oxideav-dts: §5.7.2 Rev2AUX DRC value count cannot be \
                 derived: {blocks} blocks per frame is not a whole number of \
                 256-sample subsubframes"
            ),
            Error::DownmixCodeOutOfRange { code } => write!(
                f,
                "oxideav-dts: §5.7.1 dynamic-downmix coefficient code 0x{code:03x} \
                 is out of domain (must be a 9-bit word whose one-biased low byte \
                 indexes the 241-entry §D.11 DmixTable)"
            ),
            Error::DownmixInputShapeMismatch { expected, found } => write!(
                f,
                "oxideav-dts: §5.7.1 downmix fold shape mismatch: expected \
                 {expected}, got {found} (plane count must equal the nPriCh \
                 input channel count and all planes must be equal-length)"
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
