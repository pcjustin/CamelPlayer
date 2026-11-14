//! DTS Coherent Acoustics — §5.5 + §C.2.5 end-to-end subframe→PCM
//! bridge (ETSI TS 102 114 V1.3.1).
//!
//! Round 346 (2026-06-20) composes the two already-landed halves of the
//! Core reconstruction chain into one per-subframe call:
//!
//! 1. the round-340 §5.5 [`crate::audio_array::decode_audio_data_subframe_at`] walk, which
//!    turns the §5.4.1 side information + the §5.5 `Audio Data` arrays
//!    into the per-channel subband-sample matrices
//!    `aPrmCh[ch].aSubband[n].aSample[m]`, and
//! 2. the round-330 §C.2.5 [`MultiChannelQmf`] driver, which runs the
//!    per-channel `aPrmCh[ch].QMFInterpolation(FILTS, nSUBS[ch])` 32-band
//!    synthesis filterbank over those matrices to produce PCM.
//!
//! The bridge is the missing composition step the crate README's "Not
//! yet implemented" tail named first: *"The §5.5 `Audio Data` walker
//! that composes the side-info, dispatch, dequantization, ADPCM, and QMF
//! primitives into reconstructed subband samples — and thus PCM
//! output."* The walker (#1) and the synthesis (#2) both landed in
//! prior rounds; this module is the one-call subframe driver that wires
//! the walker's output directly into the synthesis input.
//!
//! # The per-subframe loop (§5.4 + §5.5 + §C.2.5)
//!
//! For one audio subframe the spec runs (PDF p.28-33, then the §C.2.5
//! driver per channel):
//!
//! ```text
//! // §5.5 Audio Data: nSSC subsubframes of 8 samples each ->
//! //   aPrmCh[ch].aSubband[n].aSample[0 .. nSSC*8]
//! decode_audio_data_subframe_at(...);
//! // §C.2.5 Filter Bank Reconstruction, once per channel:
//! for (ch=0; ch<nPCHS; ch++)
//!     aPrmCh[ch].QMFInterpolation(FILTS, nSUBS[ch]);
//! ```
//!
//! Each channel's `nSSC*8` per-sample subband rows synthesise to
//! `nSSC*8*32` PCM samples (the §C.2.5 driver emits 32 PCM samples per
//! subband-sample row). A subframe therefore yields `nSSC * 256` PCM
//! samples per channel — except on a **termination frame** (§5.3.1
//! `FTYPE = 0`) whose subframe signals a §5.4.1 partial subsubframe
//! (`PSC > 0`): its last subsubframe carries `PSC < 8` samples per
//! subband, so the subframe yields `((nSSC-1)*8 + PSC) * 32` PCM
//! samples (see [`SubframePcmDecoder::decode_subframe_partial`]).
//!
//! # Persistence across subframes
//!
//! [`SubframePcmDecoder`] owns one persistent [`MultiChannelQmf`] so a
//! caller decoding a frame's subframes (or a stream's frames) in order
//! carries each channel's inter-subframe filter tail (`raX[]` / `raZ[]`)
//! exactly as the §C.2.5 driver requires. Construct it once for the
//! frame's channel count, then call [`SubframePcmDecoder::decode_subframe`]
//! for each subframe.
//!
//! # Scope
//!
//! The walker's §D.10.1 ADPCM-coefficient-VQ (`PMODE != 0`) and §D.10.2
//! high-frequency-VQ (`nVQSUB < nSUBS`) sub-paths are fully implemented
//! (round 434) and — since round 439 — **enabled by default**: every
//! decoder starts with the real §D.10 books
//! ([`VqCodebooks::builtin`], transcribed from the staged clean-room
//! tables `docs/audio/dts/tables/dts-d10-*.csv`), so those frames
//! reconstruct to PCM end-to-end out of the box (phase-1 HF-VQ fill,
//! §C.2.2 prediction with the persistent [`AdpcmHistory`] and the
//! §5.3.1 `HFLAG` frame gate). A caller may still swap or strip the
//! books ([`SubframePcmDecoder::set_vq_codebooks`]); with
//! [`VqCodebooks::none`] such frames surface the typed
//! [`AudioArrayError::VqCodebookUnavailable`] error before any §5.5
//! bit is read, exactly as in the pre-round-439 bookless state.
//!
//! Joint-intensity subband coding (`JOINX[ch] > 0`) is not applied here:
//! the §C.2.3 joint-subband decode is landed
//! ([`crate::joint_subband::joint_subband_decode_range_f64`]) but it needs the
//! `JOIN_SCALES[ch][n]` Huffman factors, whose §5.4.x bit-stream decode
//! is not yet wired. [`SubframePcmDecoder::decode_subframe`] therefore
//! surfaces [`SubframePcmError::JointSubbandUnsupported`] when any
//! channel carries `JOINX[ch] > 0`, rather than silently skipping the
//! joint step.

use crate::audio_array::{
    AdpcmContext, AdpcmHistory, AudioArrayDecodeError, AudioArrayError, HfVqFill,
    SubbandSampleMatrix, decode_audio_data_subframe_vq_at, decode_lfe_phase_at,
};
use crate::audio_header::AudioCodingHeader;
use crate::cos_mod::NUM_SUBBAND;
use crate::d10_vq::{VqCodebooks, scan_hf_vq_indices_at};
use crate::filter_bank::FilterBankSelection;
use crate::header::{AmodeArrangement, DtsFrameHeader};
use crate::qmf_multichannel::{MultiChannelQmf, MultiChannelQmfError};
use crate::step_size::StepSizeTable;
use crate::subframe::{ChannelSideInfo, SideInfoTail};

/// One subframe's reconstructed PCM, planar (one `Vec<i32>` per
/// channel). Every channel's vec has the same length — `nSSC * 256`
/// samples (`nSSC` subsubframes × 8 samples × 32 PCM samples per
/// subband-sample row), or `((nSSC-1)·8 + PSC) · 32` samples when a
/// termination frame's subframe ends in a §5.4.1 partial subsubframe.
pub type SubframePcm = Vec<Vec<i32>>;

/// PCM samples one §C.2.5 subband-sample row expands to (the driver
/// emits 32 PCM samples per row — the `NumSubband` bands of one
/// polyphase output block).
pub const PCM_PER_SUBBAND_ROW: usize = NUM_SUBBAND;

/// Implementation-defined output-level calibration applied on top of
/// [`DtsFrameHeader::output_r_scale`] by the §C.2.5 synthesis step of
/// this decoder.
///
/// Per `docs/audio/dts/dts-qmf-driver.md` §2, the §C.2.5 output
/// `rScale` is **not** a normative constant: it is whatever fixed gain
/// maps the implementation's internal `raZ` normalization to
/// full-scale integer PCM at the `PCMR` resolution. This crate's
/// `raZ` domain (fixed by the literal §C.2.5 cosine-modulation
/// scalers) sits exactly √2 below the level the black-box reference
/// decoder emits for the same streams — a constant ratio measured to
/// < 1e-4 across every bundled `ffmpeg`-reference fixture (stereo,
/// 5.1 and the three §D.10 fixture families). Round 453 folds that √2
/// into the decode chain so the reconstructed PCM matches the
/// reference (and the round trip through this crate's own encoder is
/// unity), instead of documenting the ratio test-side.
pub const OUTPUT_LEVEL_CALIBRATION: f64 = std::f64::consts::SQRT_2;

/// [`DtsFrameHeader::output_r_scale`] of a 16-bit source, the gain the
/// synthesis applies whatever `PCMR` declares.
const SIXTEEN_BIT_R_SCALE: f64 = 32_768.0;

/// Errors from the §5.5 + §C.2.5 end-to-end subframe→PCM bridge.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum SubframePcmError {
    /// The §5.5 [`crate::audio_array::decode_audio_data_subframe_at`] walk failed: a
    /// bit-stream-level error, or an Annex D VQ-codebook blocker
    /// (`PMODE != 0` / `nVQSUB < nSUBS`). Carries the underlying
    /// [`AudioArrayDecodeError`].
    AudioData(AudioArrayDecodeError),
    /// The §C.2.5 [`MultiChannelQmf`] synthesis failed (a length or
    /// row-count mismatch between the walker's matrices and the driver's
    /// channel count, or a per-channel synthesis error).
    Synthesis(MultiChannelQmfError),
    /// A channel carried `JOINX[ch] > 0` (joint-intensity subband
    /// coding). The §C.2.3 joint-subband decode is landed but its
    /// `JOIN_SCALES` Huffman side-info decode is not yet wired, so the
    /// bridge declines rather than producing incorrect PCM. Carries the
    /// 0-based channel index and the one-based `JOINX` source.
    JointSubbandUnsupported {
        /// 0-based destination channel carrying `JOINX > 0`.
        ch: usize,
        /// The one-based `JOINX[ch]` source-channel selector.
        joinx: u8,
    },
    /// The frame header's `PCMR` source-PCM-resolution code is one of
    /// the two reserved values, so the §C.2.5 output `rScale` (the
    /// post-filterbank float→PCM full-scale gain derived from `PCMR`) is
    /// undefined and no PCM can be produced. Carries the raw `PCMR`
    /// index.
    ReservedPcmResolution {
        /// The raw §5.3.1 Table 5-17 `PCMR` index.
        pcmr: u8,
    },
    /// The frame header's `LFF` is the invalid code 3, which names no LFE decimation (ETSI TS 102 114 §5.3.1).
    InvalidLfe,
    /// The subframes' subsubframes hold more sample blocks than the header's `NBLKS` gives the frame. Carries the
    /// 0-based subframe index that goes past it.
    MoreBlocksThanHeader {
        /// 0-based subframe index.
        subframe: usize,
    },
    /// The caller-supplied per-channel side-info / loop-bound slices did
    /// not all agree on the channel count. Carries the channel count the
    /// driver expected (the [`SubframePcmDecoder`]'s configured count)
    /// and the mismatching slice length.
    ChannelCountMismatch {
        /// The driver's configured channel count.
        expected: usize,
        /// The mismatching supplied slice length.
        got: usize,
    },
    /// The §C.2.3 joint-intensity sub-band copy could not be applied
    /// because the supplied `JOIN_SCALES` factors, source-channel index,
    /// or `nSUBS` bounds were structurally inconsistent with the decoded
    /// sub-band matrices. Carries the 0-based destination channel.
    JointSubbandShape {
        /// 0-based destination channel whose joint import failed.
        ch: usize,
    },
    /// A subframe's §5.4.1 `PSC` (Partial Subsubframe Sample Count) was
    /// non-zero but the §5.3.1 frame header's `FTYPE` says **normal**
    /// frame. Per PDF p.30 a partial subsubframe "exists only in a
    /// termination frame", so a normal frame signalling one is
    /// structurally invalid and the decode declines rather than
    /// truncating a normal frame's audio. Carries the 0-based subframe
    /// index and the offending wire `PSC`.
    PartialSubsubframeInNormalFrame {
        /// 0-based subframe index whose side info carried `PSC > 0`.
        subframe: usize,
        /// The offending 3-bit wire `PSC` value (`1..=7`).
        psc: u8,
    },
}

impl core::fmt::Display for SubframePcmError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            SubframePcmError::AudioData(e) => write!(f, "audio-data walk failed: {e}"),
            SubframePcmError::Synthesis(e) => write!(f, "QMF synthesis failed: {e}"),
            SubframePcmError::JointSubbandUnsupported { ch, joinx } => write!(
                f,
                "channel {ch} carries JOINX={joinx} (joint-intensity subband \
                 coding); the JOIN_SCALES side-info decode is not yet wired"
            ),
            SubframePcmError::ReservedPcmResolution { pcmr } => write!(
                f,
                "frame header PCMR index {pcmr} is reserved; the output rScale \
                 is undefined"
            ),
            SubframePcmError::InvalidLfe => write!(f, "frame header LFF code 3 is invalid"),
            SubframePcmError::MoreBlocksThanHeader { subframe } => write!(
                f,
                "subframe {subframe} goes past the frame header's NBLKS sample blocks"
            ),
            SubframePcmError::ChannelCountMismatch { expected, got } => write!(
                f,
                "channel-count mismatch: driver expects {expected}, slice carries {got}"
            ),
            SubframePcmError::JointSubbandShape { ch } => write!(
                f,
                "channel {ch} joint-intensity sub-band copy failed: JOIN_SCALES / \
                 nSUBS bounds inconsistent with the decoded sub-band matrices"
            ),
            SubframePcmError::PartialSubsubframeInNormalFrame { subframe, psc } => write!(
                f,
                "subframe {subframe} signals a partial subsubframe (PSC={psc}) \
                 but the frame header's FTYPE is normal; a partial subsubframe \
                 exists only in a termination frame (§5.4.1, PDF p.30)"
            ),
        }
    }
}

impl std::error::Error for SubframePcmError {}

impl From<AudioArrayDecodeError> for SubframePcmError {
    fn from(e: AudioArrayDecodeError) -> Self {
        SubframePcmError::AudioData(e)
    }
}

impl From<MultiChannelQmfError> for SubframePcmError {
    fn from(e: MultiChannelQmfError) -> Self {
        SubframePcmError::Synthesis(e)
    }
}

/// Persistent per-frame §5.5 + §C.2.5 subframe→PCM decoder.
///
/// Owns one [`MultiChannelQmf`] for the frame's channel count, so the
/// per-channel filter state (`raX[]` / `raZ[]`) carries across
/// subframes (and across frames if the same decoder instance is reused
/// for a stream). Construct once with the channel count from the frame
/// header, then call [`SubframePcmDecoder::decode_subframe`] for each of
/// the `nSUBFS` subframes the §5.3.2 header declares.
#[derive(Debug, Clone)]
pub struct SubframePcmDecoder {
    qmf: MultiChannelQmf,
    /// The persistent §5.5/§C.2.6 LFE channel (the `LFECh` filter
    /// object). Drives the §5.5 LFE phase (§2.2) when the frame header's
    /// `LFF` is non-zero, carrying the §C.2.6 inter-subframe
    /// interpolation history. Idle (and never read from the bitstream)
    /// for LFF-absent frames.
    lfe: crate::lfe_synth::LfeChannel,
    /// The LFE PCM decoded from the most recent [`Self::decode_subframe`]
    /// call (empty when the frame had no LFE channel). Surfaced via
    /// [`Self::take_last_lfe_pcm`] so the primary-channel return tuple is
    /// unchanged.
    last_lfe_pcm: Vec<i32>,
    /// The §D.10 VQ code books. Default: the built-in real books
    /// ([`VqCodebooks::builtin`]); a caller may swap or strip them
    /// ([`Self::set_vq_codebooks`]).
    vq_codebooks: VqCodebooks,
    /// The persistent §C.2.2 per-subband reconstruction history that
    /// primes the inverse-ADPCM predictor across subframe (and, per
    /// the §5.3.1 `HFLAG` gate, frame) boundaries.
    adpcm_history: AdpcmHistory,
}

impl SubframePcmDecoder {
    /// Construct a decoder for `channels` primary audio channels — the
    /// §5.3.2 `nPCHS` (e.g. [`AudioCodingHeader::n_pchs`]). Each
    /// channel's §C.2.5 filter starts with cleared history.
    #[must_use]
    pub fn new(channels: usize) -> Self {
        Self {
            qmf: MultiChannelQmf::new(channels),
            lfe: crate::lfe_synth::LfeChannel::new(),
            last_lfe_pcm: Vec::new(),
            vq_codebooks: VqCodebooks::builtin(),
            adpcm_history: AdpcmHistory::new(channels),
        }
    }

    /// Take the LFE PCM decoded by the most recent
    /// [`Self::decode_subframe`] / [`Self::decode_frame`] call, leaving
    /// the decoder's buffer empty. Returns an empty `Vec` when the last
    /// frame carried no LFE channel (`LFF == 0`). The samples are the
    /// §5.5 LFE phase (§2.2) output: `2·LFF·nSSC·(64 | 128)` interpolated
    /// PCM samples per decoded subframe.
    #[must_use]
    pub fn take_last_lfe_pcm(&mut self) -> Vec<i32> {
        core::mem::take(&mut self.last_lfe_pcm)
    }

    /// The configured channel count (`nPCHS`).
    #[must_use]
    pub fn channel_count(&self) -> usize {
        self.qmf.channel_count()
    }

    /// Like [`Self::decode_subframe_with_joint`] but with the §5.4.1
    /// `PSC` (Partial Subsubframe Sample Count) of a **termination
    /// frame** applied: when `psc ∈ 1..=7`, the last of this
    /// subframe's `n_ssc` subsubframes is *partial* — it carries `psc`
    /// subband samples per active subband instead of 8, so the
    /// subframe reconstructs to `((n_ssc - 1) * 8 + psc) * 32` PCM
    /// samples per channel and the §5.5 bit budget shrinks exactly by
    /// the untransmitted samples (see
    /// [`crate::audio_array::decode_audio_data_subframe_partial_at`] for the
    /// spec-clause derivation). `psc = 0` is the normal-frame case and
    /// reproduces [`Self::decode_subframe_with_joint`] verbatim.
    ///
    /// The spec ties `PSC > 0` to termination frames only ("It exists
    /// only in a termination frame", PDF p.30); this per-subframe
    /// entry point trusts the caller on that frame-level gate (the
    /// frame walk [`decode_core_frame`] enforces it, surfacing
    /// [`SubframePcmError::PartialSubsubframeInNormalFrame`]).
    ///
    /// When the frame carries an LFE channel, the §5.5 LFE phase is
    /// extracted and interpolated at its spec-literal size (Table
    /// 5-29: `2·LFF·nSSC` decimated samples — the count has no `PSC`
    /// term, so the LFE always covers whole subsubframes) and the
    /// interpolated plane is then truncated to the primary channels'
    /// PCM length, keeping every output plane aligned on the valid
    /// prefix of the terminated subframe.
    ///
    /// # Errors
    ///
    /// See [`Self::decode_subframe_with_joint`].
    pub fn decode_subframe_partial(
        &mut self,
        bytes: &[u8],
        bit_offset: usize,
        header: &DtsFrameHeader,
        coding: &AudioCodingHeader,
        side: &[ChannelSideInfo],
        n_ssc: usize,
        psc: u8,
        aspf: bool,
        join_scales: &[Vec<f64>],
    ) -> Result<(SubframePcm, usize), SubframePcmError> {
        let channels = self.qmf.channel_count();
        if side.len() != channels {
            return Err(SubframePcmError::ChannelCountMismatch {
                expected: channels,
                got: side.len(),
            });
        }
        if coding.n_pchs != channels {
            return Err(SubframePcmError::ChannelCountMismatch {
                expected: channels,
                got: coding.n_pchs,
            });
        }

        // The §C.2.5 output rScale must be defined (PCMR not reserved)
        // before any decode work runs, so a reserved-PCMR frame fails
        // cleanly without disturbing the persistent filter state.
        if header.output_r_scale().is_none() {
            return Err(SubframePcmError::ReservedPcmResolution {
                pcmr: header.source_pcm_resolution_index,
            });
        }
        if header.lfe == crate::header::LfeMode::Mode3 {
            return Err(SubframePcmError::InvalidLfe);
        }
        // PCMR tells what the source was, not how the core's samples
        // are scaled: every resolution decodes at the 16-bit one's
        // level, as the reference decoder does and as the LFE plane's
        // fixed calibration assumes.
        let r_scale = SIXTEEN_BIT_R_SCALE * OUTPUT_LEVEL_CALIBRATION;
        let filter: FilterBankSelection = header.filter_bank_selection();

        // Joint-intensity subband coding (JOINX > 0) is applied below
        // once the §5.5 walk has filled every channel's sub-band matrix,
        // but only when the caller supplied the JOIN_SCALES factors (via
        // decode_subframe_with_joint). A JOINX > 0 channel with no
        // supplied factors is declined rather than silently dropped.
        if join_scales.is_empty() {
            for (ch, &joinx) in coding.joinx.iter().enumerate().take(channels) {
                if joinx > 0 {
                    return Err(SubframePcmError::JointSubbandUnsupported { ch, joinx });
                }
            }
        }

        // Per-channel loop bounds for the §5.5 walk and the §C.2.5
        // driver come straight off the §5.3.2 header.
        let n_subs = coding.n_subs();
        let n_vqsub = coding.n_vqsub();

        let table = StepSizeTable::for_rate(header.rate_index);

        // Subband-sample rows this subframe reconstructs: the last
        // subsubframe is partial (psc rows) on a termination-frame
        // subframe, full (8 rows) otherwise.
        let rows = if psc > 0 {
            (n_ssc - 1) * 8 + usize::from(psc)
        } else {
            n_ssc * 8
        };

        // (0a) §D.10 book-availability gates, checked BEFORE any bit
        // is read so a blocked frame fails cleanly without disturbing
        // the persistent LFE / filter / history state. Both books are
        // present by default (`VqCodebooks::builtin`); the gates fire
        // only when a caller stripped them (`VqCodebooks::none`).
        let has_hf_vq = (0..channels).any(|ch| n_vqsub[ch] < n_subs[ch]);
        if has_hf_vq && self.vq_codebooks.hfreq.is_none() {
            let ch = (0..channels)
                .find(|&ch| n_vqsub[ch] < n_subs[ch])
                .unwrap_or(0);
            return Err(SubframePcmError::AudioData(
                AudioArrayError::VqCodebookUnavailable {
                    ch,
                    n: n_vqsub[ch],
                    high_frequency_vq: true,
                }
                .into(),
            ));
        }
        if self.vq_codebooks.adpcm.is_none() {
            for (ch, ch_side) in side.iter().enumerate() {
                if let Some(n) = ch_side.pmode[..n_vqsub[ch]].iter().position(|&p| p != 0) {
                    return Err(SubframePcmError::AudioData(
                        AudioArrayError::VqCodebookUnavailable {
                            ch,
                            n,
                            high_frequency_vq: false,
                        }
                        .into(),
                    ));
                }
            }
        }

        // (0b) §5.5 phase 1 — high-frequency VQ subbands: the 10-bit
        // `nVQIndex` fields precede the LFE phase (Table 5-29). Empty
        // for the common Core case (`nVQSUB == nSUBS` everywhere).
        let mut cursor = bit_offset;
        let hf_indices: Option<Vec<Vec<u16>>> = if has_hf_vq {
            let (indices, hf_bits) = scan_hf_vq_indices_at(bytes, cursor, &n_vqsub, &n_subs)
                .map_err(|e| SubframePcmError::AudioData(e.into()))?;
            cursor += hf_bits;
            Some(indices)
        } else {
            None
        };

        // (0c) §5.5 LFE phase (§2.2): present only when the header's
        // `LFF` is non-zero; it follows the phase-1 HF-VQ region and
        // precedes the audio-data phase. Its bits count toward the
        // subframe's total so the caller advances correctly. The Table
        // 5-29 sample count (`2·LFF·nSSC`) has no PSC term — the LFE
        // plane always covers whole subsubframes — so on a partial
        // (termination) subframe the interpolated plane is truncated
        // below to the primary channels' PCM length.
        let lff = header.lfe.code();
        if lff != 0 {
            let (mut lfe_pcm, lfe_bits) =
                decode_lfe_phase_at(bytes, cursor, lff, n_ssc, &mut self.lfe)?;
            lfe_pcm.truncate(rows * PCM_PER_SUBBAND_ROW);
            self.last_lfe_pcm = lfe_pcm;
            cursor += lfe_bits;
        } else {
            self.last_lfe_pcm = Vec::new();
        }

        // (1) §5.5 Audio Data -> per-channel subband-sample matrices
        // (`rows` per channel; the §5.4.1 PSC truncation of the last
        // subsubframe is applied inside the walk, bit-exactly), with
        // the recovered-book sub-paths enabled where supplied: the
        // phase-1 HF-VQ fill and the §C.2.2 inverse-ADPCM prediction
        // (whose reconstruction history persists across subframes; the
        // §5.3.1 HFLAG frame gate is applied by the frame-level walk).
        let hf_fill = match (&self.vq_codebooks.hfreq, &hf_indices) {
            (Some(book), Some(indices)) => Some(HfVqFill {
                book: book.as_ref(),
                indices: indices.as_slice(),
            }),
            _ => None,
        };
        let adpcm_ctx = self.vq_codebooks.adpcm.as_ref().map(|book| AdpcmContext {
            book: book.as_ref(),
            history: &mut self.adpcm_history,
        });
        let (matrices, audio_bits): (Vec<SubbandSampleMatrix>, usize) =
            decode_audio_data_subframe_vq_at(
                bytes,
                cursor,
                side,
                |ch, abits| coding.sel(ch, abits),
                |ch, abits| coding.adj(ch, abits),
                &n_vqsub,
                &n_subs,
                n_ssc,
                psc,
                table,
                aspf,
                hf_fill,
                adpcm_ctx,
            )?;
        let bits_consumed = (cursor - bit_offset) + audio_bits;

        // (1b) §C.2.3 joint-intensity sub-band copy. For each
        // destination channel with JOINX[ch] > 0 and supplied
        // JOIN_SCALES factors, overwrite its imported sub-band columns
        // [nSUBS[ch], nSUBS[src]) with the source channel's sub-band
        // samples scaled by the matching JOIN_SCALES factor. This runs
        // on the sub-band matrices before QMF synthesis.
        let mut matrices = matrices;
        if !join_scales.is_empty() {
            apply_joint_subband(&mut matrices, coding, &n_subs, join_scales)?;
        }

        // (1b') Effective per-channel active-subband counts after the
        // joint import. The §C.2.5 driving-call comment is explicit
        // (staged PDF p.184): "nSUBS[ch] indicates the number of active
        // subbands. Subbands above it are all zeros. For joint intensity
        // coded subbands, it must be set to that of the source channel,
        // in order to reflect the true subband activity." A jointly-
        // coded destination channel therefore synthesizes (and, below,
        // sum/difference-matrixes) over the source channel's count —
        // otherwise the §C.2.3 import into [nSUBS[ch], nSUBS[src]) would
        // be zero-filled away by the QMF's inactive-subband clear. The
        // degenerate empty-range joint (source not wider than the
        // destination) keeps the destination's own count.
        let mut eff_n_subs = n_subs.clone();
        for (ch, &joinx) in coding.joinx.iter().enumerate().take(channels) {
            if let Some(src) = crate::joint_subband::joint_source_channel(joinx) {
                let src = usize::from(src);
                if src < n_subs.len() && n_subs[src] > eff_n_subs[ch] {
                    eff_n_subs[ch] = n_subs[src];
                }
            }
        }

        // (1c) §C.2.4 sum/difference decoding. When the front-sum flag
        // (`SUMF`) is set — or unconditionally for AMODE == 3, per the
        // spec's "This decoding is also required when AMODE = 3" — the
        // front L/R channels are stored as (L+R, L-R) and must be matrixed
        // back on the reconstructed sub-band samples (all active subbands,
        // all sub-subframe samples) before QMF synthesis. `SUMS` does the
        // same for the surround L/R pair. This runs after §C.2.3 joint
        // subband and before §C.2.5, matching the Annex C ordering.
        let arrangement = header.amode_arrangement();
        let apply_front =
            header.front_sum || matches!(arrangement, AmodeArrangement::SumDifference);
        if apply_front && let Some((l, r)) = arrangement.front_lr_channels() {
            apply_sum_difference(&mut matrices, l, r, &mut eff_n_subs)?;
        }
        if header.surround_sum
            && let Some((l, r)) = arrangement.surround_lr_channels()
        {
            apply_sum_difference(&mut matrices, l, r, &mut eff_n_subs)?;
        }

        // (2) §C.2.5 per-channel 32-band synthesis -> planar PCM.
        let channel_samples: Vec<&[[f64; NUM_SUBBAND]]> =
            matrices.iter().map(|m| m.as_slice()).collect();
        let mut pcm: SubframePcm = vec![Vec::new(); channels];
        self.qmf
            .synthesize_planar(&channel_samples, &eff_n_subs, filter, r_scale, &mut pcm)?;

        Ok((pcm, bits_consumed))
    }
}

/// Why a Core frame could not be decoded straight from its bytes by
/// [`decode_core_frame`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum CoreFrameDecodeError {
    /// The frame carries a Table 5-28 joint-intensity side-info tail
    /// this crate does not yet decode: some channel has `JOINX > 0`,
    /// so a variable-length `JOIN_SHUFF` / `JOIN_SCALES` block (gated on
    /// the unstaged joint-scale table) sits between a subframe's side
    /// info and its §5.5 `Audio Data` region, and the audio-data bit
    /// offset cannot be located. The `DYNF` (`RANGE`) and `CPF`
    /// (`SICRC`) tail fields are decoded (see [`decode_core_frame`]);
    /// only joint-intensity surfaces here.
    UnsupportedSideInfoTail {
        /// `DYNF != 0` — embedded dynamic-range `RANGE` field present.
        /// Retained for source compatibility; no longer a decline
        /// reason (the `RANGE` field is decoded and applied post-QMF).
        dynamic_range: bool,
        /// `CPF != 0` — a 16-bit `SICRC` side-info CRC trailer present.
        /// Retained for source compatibility; no longer a decline
        /// reason (the `SICRC` word is consumed for framing).
        side_info_crc: bool,
        /// Some channel carries `JOINX > 0` — a `JOIN_SHUFF`/`JOIN_SCALES`
        /// block present. This is the sole remaining decline reason.
        joint_intensity: bool,
    },
    /// A §5.3.2 / §5.4.1 / §5.5 decode step failed. Carries the
    /// underlying [`SubframePcmError`] (or a wrapped bit-stream
    /// [`crate::Error`] for the header/side-info walks).
    Decode(SubframePcmError),
    /// A structural bit-stream error in the §5.3.2 audio-coding-header or
    /// §5.4.1 side-info walk (EOF, reserved selector, …).
    Bitstream(crate::Error),
}

impl core::fmt::Display for CoreFrameDecodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            CoreFrameDecodeError::UnsupportedSideInfoTail {
                dynamic_range,
                side_info_crc,
                joint_intensity,
            } => write!(
                f,
                "frame carries an undecoded §5.4.x side-info tail \
                 (DYNF={dynamic_range}, CPF/SICRC={side_info_crc}, \
                 JOINX>0={joint_intensity}); only the empty-tail common \
                 Core case is decoded to PCM"
            ),
            CoreFrameDecodeError::Decode(e) => write!(f, "{e}"),
            CoreFrameDecodeError::Bitstream(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for CoreFrameDecodeError {}

impl From<SubframePcmError> for CoreFrameDecodeError {
    fn from(e: SubframePcmError) -> Self {
        CoreFrameDecodeError::Decode(e)
    }
}

impl From<crate::Error> for CoreFrameDecodeError {
    fn from(e: crate::Error) -> Self {
        CoreFrameDecodeError::Bitstream(e)
    }
}

/// Persistent §5.3/§5.4/§5.5 + §C.2.5 Core-stream decoder.
///
/// The §C.2.5 `aPrmCh[ch]` synthesis filter is a **continuous**
/// per-channel object whose 512-tap history (`raX[]`) and output
/// accumulator (`raZ[]`) carry across subframe **and frame**
/// boundaries of a contiguous elementary stream — the decoder does not
/// reset the filter at each frame. [`decode_core_frame`] (a fresh
/// per-call decoder) therefore reconstructs each frame as if it were
/// the first frame of a stream, which produces a filter-warmup
/// transient at every frame boundary instead of only the stream's true
/// start. For multi-frame decode use this type: construct it once for
/// the stream's channel count and feed every frame in order through
/// [`CoreStreamDecoder::decode_frame`], so each channel's inter-frame
/// filter tail carries correctly.
///
/// Validated against a black-box `ffmpeg -c:a dca` reference decode of
/// the bundled 5-frame fixture: carrying the filter state across frames
/// makes our channel-0 PCM **shape-identical** to the reference
/// (Pearson correlation 1.0 over the whole stream), versus 0.73 when
/// the filter is reset per frame. (The two differ only by the
/// implementation-defined output `rScale` constant — see
/// [`DtsFrameHeader::output_r_scale`] and the round-356 report.)
#[derive(Debug, Clone)]
pub struct CoreStreamDecoder {
    decoder: SubframePcmDecoder,
}

impl CoreStreamDecoder {
    /// Construct a stream decoder for `channels` primary audio channels
    /// (the §5.3.2 `nPCHS`). Every channel's §C.2.5 filter starts with a
    /// cleared history; that history then carries across every
    /// [`CoreStreamDecoder::decode_frame`] call.
    #[must_use]
    pub fn new(channels: usize) -> Self {
        Self {
            decoder: SubframePcmDecoder::new(channels),
        }
    }

    /// The configured channel count (`nPCHS`).
    #[must_use]
    pub fn channel_count(&self) -> usize {
        self.decoder.channel_count()
    }

    /// Take the LFE PCM decoded by the most recent
    /// [`Self::decode_frame`] call (empty when the frame carried no LFE
    /// channel, `LFF == 0`). See
    /// [`SubframePcmDecoder::take_last_lfe_pcm`]. The LFE PCM is at the
    /// same per-frame sample rate as the primary channels (the §C.2.6
    /// interpolation expands each decimated sample by exactly the factor
    /// that matches the primary `nSSC·256` per-subframe length).
    #[must_use]
    pub fn take_last_lfe_pcm(&mut self) -> Vec<i32> {
        self.decoder.take_last_lfe_pcm()
    }

    /// Decode one whole Core frame to planar PCM, carrying the
    /// per-channel §C.2.5 filter tail into the next call.
    ///
    /// Identical reconstruction to [`decode_core_frame`] except the
    /// filter state is **not** reset: a frame's first output samples see
    /// the previous frame's filter tail, exactly as the §C.2.5
    /// continuous per-channel filter requires for a contiguous stream.
    ///
    /// `bytes` is one frame's bit-stream buffer; `header` its parsed
    /// §5.3.1 header. The frame's §5.3.2 audio-coding-header channel
    /// count must equal this decoder's configured channel count.
    ///
    /// # Errors
    ///
    /// The same errors as [`decode_core_frame`], plus
    /// [`CoreFrameDecodeError::Decode`] wrapping a
    /// [`SubframePcmError::ChannelCountMismatch`] if the frame's
    /// `nPCHS` disagrees with the configured channel count.
    pub fn decode_frame(
        &mut self,
        bytes: &[u8],
        header: &DtsFrameHeader,
    ) -> Result<SubframePcm, CoreFrameDecodeError> {
        self.decoder.decode_core_frame_into(bytes, header)
    }
}

impl SubframePcmDecoder {
    /// Decode one whole Core frame to planar PCM using this persistent
    /// decoder's per-channel §C.2.5 filter state (carried across calls).
    ///
    /// This is the per-frame body shared by [`decode_core_frame`] (which
    /// calls it on a fresh decoder, giving single-frame semantics) and
    /// [`CoreStreamDecoder::decode_frame`] (which calls it on a
    /// stream-lifetime decoder, carrying the inter-frame filter tail).
    ///
    /// # Errors
    ///
    /// See [`decode_core_frame`].
    pub fn decode_core_frame_into(
        &mut self,
        bytes: &[u8],
        header: &DtsFrameHeader,
    ) -> Result<SubframePcm, CoreFrameDecodeError> {
        self.decode_core_frame_cursor(bytes, header)
            .map(|(pcm, _)| pcm)
    }

    /// Shared §5.3.2 → §5.4.1 → §5.5 + §C.2.5 frame walk, returning
    /// the planar PCM plus the bit cursor at the end of the last
    /// audio-data array (where the §5.6 Table 5-30 region begins).
    fn decode_core_frame_cursor(
        &mut self,
        bytes: &[u8],
        header: &DtsFrameHeader,
    ) -> Result<(SubframePcm, usize), CoreFrameDecodeError> {
        // §5.3.2 Primary Audio Coding Header begins right after the
        // §5.3.1 frame header. The §5.3.1 CRC-present flag
        // (CPF == `crc_present`) controls the optional 16-bit SICRC
        // trailer of every subframe's §5.4.1 side info.
        let header_bits = header.header_bit_length() as usize;
        let cpf = header.crc_present;
        let (coding, ach_bits) =
            crate::audio_header::decode_audio_coding_header_at(bytes, header_bits, cpf)?;

        let channels = coding.n_pchs;
        if channels != self.channel_count() {
            return Err(CoreFrameDecodeError::Decode(
                SubframePcmError::ChannelCountMismatch {
                    expected: self.channel_count(),
                    got: channels,
                },
            ));
        }
        let mut pcm: SubframePcm = vec![Vec::new(); channels];

        // The §5.4.1 side-info walk needs the per-channel
        // ChannelSideInfoParams.
        let params: Vec<_> = coding.channel_params.clone();

        // The encoder's dynamic-range control, the per-subframe RANGE gain (DYNF) and a Rev2AUX chunk's broadcast DRC
        // values (§5.7.2), is read for framing but not applied: the frame decodes to the uncompressed mix, as the
        // reference decoder does, on every channel and the LFE alike.

        // §5.3.1 HFLAG (Predictor History Flag Switch): "When
        // generating ADPCM predictions for current frame, the decoder
        // will use reconstruction history of the previous frame if
        // HFLAG = 1. Otherwise, the history will be ignored" — an
        // entry-point frame is coded without the previous frame's
        // predictor history, so the persistent §C.2.2 history is
        // zeroed before this frame's first subframe. (Within the
        // frame the history always carries across subframes.)
        if !header.predictor_history {
            self.adpcm_history.clear();
        }

        let n_subs = coding.n_subs();
        let mut bit = header_bits + ach_bits;
        // Each subframe leaves only its own LFE PCM; take_last_lfe_pcm() gives the frame's.
        let mut frame_lfe: Vec<i32> = Vec::new();
        // As the reference decoder does, more blocks than NBLKS+1 fail the frame; fewer leave a gap the caller fills.
        let header_blocks = usize::from(header.blocks_per_frame) + 1;
        let mut blocks = 0;
        for subframe_index in 0..coding.n_subframes {
            // §5.4.1 side info (Table 5-28) through the end of the
            // SCALES block.
            let (side, side_bits) =
                crate::subframe::decode_primary_side_info_at(bytes, bit, &params)?;
            bit += side_bits;

            // The Table 5-28 JOIN_SHUFF / JOIN_SCALES / RANGE (DYNF) /
            // SICRC (CPF) tail sits between the SCALES block and the §5.5
            // region. The joint-intensity JOIN_SCALES factors (if any)
            // feed the §C.2.3 sub-band copy below.
            let (tail, tail_bits): (SideInfoTail, usize) =
                crate::subframe::decode_primary_side_info_tail_at(
                    bytes,
                    bit,
                    &coding.joinx,
                    &n_subs,
                    header.dynamic_range,
                    cpf,
                )?;
            bit += tail_bits;

            let n_ssc = side.subsubframe_count.n_ssc() as usize;
            // §5.4.1 PSC: a partial (fewer-than-8-sample) trailing
            // subsubframe "exists only in a termination frame" (PDF
            // p.30) — a normal frame signalling one is structurally
            // invalid and declines rather than truncating its audio.
            let psc = side.subsubframe_count.psc;
            if psc > 0 && header.frame_type != crate::header::FrameType::Termination {
                return Err(CoreFrameDecodeError::Decode(
                    SubframePcmError::PartialSubsubframeInNormalFrame {
                        subframe: subframe_index,
                        psc,
                    },
                ));
            }
            blocks += n_ssc * crate::step_size::SAMPLES_PER_SUBSUBFRAME;
            if blocks > header_blocks {
                return Err(CoreFrameDecodeError::Decode(
                    SubframePcmError::MoreBlocksThanHeader {
                        subframe: subframe_index,
                    },
                ));
            }
            let (block, audio_bits) = self.decode_subframe_partial(
                bytes,
                bit,
                header,
                &coding,
                &side.channels,
                n_ssc,
                psc,
                header.aspf,
                &tail.join_scales,
            )?;
            frame_lfe.append(&mut self.last_lfe_pcm);

            for (ch, samples) in block.into_iter().enumerate() {
                pcm[ch].extend(samples);
            }
            bit += audio_bits;
        }

        self.last_lfe_pcm = frame_lfe;
        Ok((pcm, bit))
    }
}

/// Apply the §C.2.3 joint-intensity sub-band copy to the per-channel
/// sub-band matrices, in place, before QMF synthesis.
///
/// For every destination channel `ch` with `JOINX[ch] > 0` and a
/// non-empty `join_scales[ch]`, each imported sub-band column `n ∈
/// [nSUBS[ch], nSUBS[nSourceCh])` of every sample row is overwritten
/// with the source channel's (`nSourceCh = JOINX[ch] - 1`) sub-band
/// sample scaled by the matching `JOIN_SCALES[ch][n]` factor.
///
/// The `join_scales[ch]` vector supplies one factor per imported
/// sub-band, ordered from `nSUBS[ch]`; its length must equal
/// `nSUBS[nSourceCh] - nSUBS[ch]`. Structural inconsistencies (source
/// channel out of range, mismatched factor count, matrices shorter than
/// `nSUBS[nSourceCh]`) surface as
/// [`SubframePcmError::JointSubbandShape`].
fn apply_joint_subband(
    matrices: &mut [SubbandSampleMatrix],
    coding: &AudioCodingHeader,
    n_subs: &[usize],
    join_scales: &[Vec<f64>],
) -> Result<(), SubframePcmError> {
    let channels = matrices.len();
    for ch in 0..channels {
        let factors = join_scales.get(ch).map(Vec::as_slice).unwrap_or(&[]);
        if factors.is_empty() {
            continue;
        }
        let joinx = coding.joinx.get(ch).copied().unwrap_or(0);
        let Some(source_ch) = crate::joint_subband::joint_source_channel(joinx).map(usize::from)
        else {
            return Err(SubframePcmError::JointSubbandShape { ch });
        };
        if source_ch >= channels || ch >= n_subs.len() || source_ch >= n_subs.len() {
            return Err(SubframePcmError::JointSubbandShape { ch });
        }
        let n_subs_dst = n_subs[ch];
        let n_subs_src = n_subs[source_ch];
        // The import range must run forward and match the factor count.
        if n_subs_src < n_subs_dst || factors.len() != n_subs_src - n_subs_dst {
            return Err(SubframePcmError::JointSubbandShape { ch });
        }
        if n_subs_src > NUM_SUBBAND {
            return Err(SubframePcmError::JointSubbandShape { ch });
        }
        // Destination and source are distinct channels of one Vec here
        // (a self-referential joint has an empty import range and never
        // reaches this point). Split the slice so the source (immutable)
        // and destination (mutable) borrows do not overlap.
        let rows = matrices[ch].len();
        if matrices[source_ch].len() != rows {
            return Err(SubframePcmError::JointSubbandShape { ch });
        }
        let (lo, hi) = if ch < source_ch {
            (ch, source_ch)
        } else {
            (source_ch, ch)
        };
        let (left, right) = matrices.split_at_mut(hi);
        let (dst_ch, src_ch): (&mut SubbandSampleMatrix, &SubbandSampleMatrix) = if ch == lo {
            (&mut left[ch], &right[0])
        } else {
            (&mut right[0], &left[source_ch])
        };
        for (dst_row, src_row) in dst_ch.iter_mut().zip(src_ch.iter()) {
            for (k, &factor) in factors.iter().enumerate() {
                let n = n_subs_dst + k;
                dst_row[n] = factor * src_row[n];
            }
        }
    }
    Ok(())
}

/// Apply the §C.2.4 sum/difference matrix to one channel pair's
/// reconstructed sub-band samples, in place, before QMF synthesis.
///
/// For each active sub-band (`n ∈ [0, nSUBS)`) and each sub-subframe
/// sample row, the pair `(l, r)` — with `l` the front/surround **left**
/// channel and `r` the **right** — is matrixed as
/// `(L', R') = (L + R, L - R)`, reading the pre-update value of the left
/// sample for both outputs (the §C.2.4 pseudocode's read-old/write-new
/// ordering). The active sub-band bound is the larger of the two
/// channels' `nSUBS` clamped to the 32-band matrix width, each channel's
/// sub-bands above its own count being zero, so the result is the
/// PCM-domain butterfly; both channels' counts in `n_subs` are raised to
/// it, so the QMF synthesizes the bands the other channel brought.
///
/// Structural inconsistencies (channel index out of range, `l == r`, or
/// the two channels' matrices carrying a different sample-row count)
/// surface as [`SubframePcmError::JointSubbandShape`] — the same
/// caller-side sub-band-matrix shape-violation variant the §C.2.3 copy
/// uses.
fn apply_sum_difference(
    matrices: &mut [SubbandSampleMatrix],
    l: usize,
    r: usize,
    n_subs: &mut [usize],
) -> Result<(), SubframePcmError> {
    let channels = matrices.len();
    if l >= channels || r >= channels || l == r || l >= n_subs.len() || r >= n_subs.len() {
        return Err(SubframePcmError::JointSubbandShape { ch: l.min(r) });
    }
    let n_active = n_subs[l].max(n_subs[r]).min(NUM_SUBBAND);
    n_subs[l] = n_active;
    n_subs[r] = n_active;
    if n_active == 0 {
        return Ok(());
    }
    if matrices[l].len() != matrices[r].len() {
        return Err(SubframePcmError::JointSubbandShape { ch: l.min(r) });
    }
    // Split the backing Vec so the two distinct channel matrices can be
    // borrowed mutably at once (mirrors the §C.2.3 copy's split pattern).
    let (lo, hi) = if l < r { (l, r) } else { (r, l) };
    let (lower, upper) = matrices.split_at_mut(hi);
    // `left_m` is the front/surround-left channel `l`, `right_m` is `r`.
    let (left_m, right_m): (&mut SubbandSampleMatrix, &mut SubbandSampleMatrix) = if l == lo {
        (&mut lower[l], &mut upper[0])
    } else {
        (&mut upper[0], &mut lower[r])
    };
    for (left_row, right_row) in left_m.iter_mut().zip(right_m.iter_mut()) {
        for n in 0..n_active {
            let lv = left_row[n];
            let rv = right_row[n];
            left_row[n] = lv + rv;
            right_row[n] = lv - rv;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The §C.2.3 joint copy overwrites the destination channel's
    /// imported sub-band columns with the source channel's samples,
    /// scaled by the matching JOIN_SCALES factor, and leaves the
    /// non-imported columns untouched.
    #[test]
    fn apply_joint_subband_scales_imported_columns() {
        // 2 channels, 2 sample rows. ch0 (source) nSUBS=4, ch1 (dst)
        // nSUBS=2 -> import subbands 2 and 3 from ch0 into ch1.
        let mut ch0 = vec![[0.0f64; NUM_SUBBAND]; 2];
        let mut ch1 = vec![[0.0f64; NUM_SUBBAND]; 2];
        // Source subband samples in columns 2 and 3.
        ch0[0][2] = 10.0;
        ch0[0][3] = -4.0;
        ch0[1][2] = 5.0;
        ch0[1][3] = 8.0;
        // Destination pre-existing values in columns 0/1 (kept) and a
        // stray in column 2 (must be overwritten).
        ch1[0][0] = 99.0;
        ch1[0][2] = 7.0;
        let mut matrices = vec![ch0, ch1];

        let mut coding = AudioCodingHeader::two_channel_for_test((4, 4), (2, 2), 0);
        coding.set_joinx_for_test(1, 1); // ch1 sources ch0 (JOINX=1)

        // JOIN_SCALES[1] = [2.0, 3.0] for imported subbands 2 and 3.
        let join_scales = vec![Vec::new(), vec![2.0, 3.0]];
        let n_subs = [4usize, 2usize];

        apply_joint_subband(&mut matrices, &coding, &n_subs, &join_scales).unwrap();

        // Imported columns are source * factor.
        assert_eq!(matrices[1][0][2], 20.0); // 10 * 2
        assert_eq!(matrices[1][0][3], -12.0); // -4 * 3
        assert_eq!(matrices[1][1][2], 10.0); // 5 * 2
        assert_eq!(matrices[1][1][3], 24.0); // 8 * 3
        // Non-imported destination columns untouched.
        assert_eq!(matrices[1][0][0], 99.0);
        // Source channel untouched.
        assert_eq!(matrices[0][0][2], 10.0);
    }

    /// A JOIN_SCALES vector whose length disagrees with the import range
    /// is rejected as a shape error.
    #[test]
    fn apply_joint_subband_rejects_wrong_factor_count() {
        let mut matrices = vec![
            vec![[0.0f64; NUM_SUBBAND]; 1],
            vec![[0.0f64; NUM_SUBBAND]; 1],
        ];
        let mut coding = AudioCodingHeader::two_channel_for_test((4, 4), (2, 2), 0);
        coding.set_joinx_for_test(1, 1);
        // Import range is [2, 4) = 2 subbands, but only 1 factor given.
        let join_scales = vec![Vec::new(), vec![2.0]];
        let n_subs = [4usize, 2usize];
        let err = apply_joint_subband(&mut matrices, &coding, &n_subs, &join_scales).unwrap_err();
        assert!(matches!(err, SubframePcmError::JointSubbandShape { ch: 1 }));
    }

    /// The first frame of the 5.1 fixture, decoded on a fresh stream decoder: its primary channels and LFE.
    fn surround_frame_pcm(frame: &[u8]) -> (SubframePcm, Vec<i32>) {
        let header = crate::header::parse_frame_header(frame).unwrap();
        let (coding, _) = crate::audio_header::decode_audio_coding_header_at(
            frame,
            header.header_bit_length() as usize,
            header.crc_present,
        )
        .unwrap();
        let mut stream = CoreStreamDecoder::new(coding.n_pchs);
        let pcm = stream.decode_frame(frame, &header).unwrap();
        (pcm, stream.take_last_lfe_pcm())
    }

    fn bits_of(bytes: &[u8]) -> Vec<u8> {
        bytes
            .iter()
            .flat_map(|b| (0..8).rev().map(move |i| (b >> i) & 1))
            .collect()
    }

    fn bytes_of(bits: &[u8]) -> Vec<u8> {
        bits.chunks(8)
            .map(|c| {
                c.iter()
                    .enumerate()
                    .fold(0u8, |b, (i, &bit)| b | (bit << (7 - i)))
            })
            .collect()
    }

    fn put_bits(bits: &mut [u8], at: usize, width: usize, value: u32) {
        for i in 0..width {
            bits[at + i] = ((value >> (width - 1 - i)) & 1) as u8;
        }
    }

    const SURROUND: &[u8] = include_bytes!("../tests/fixtures/dts_51_lfe.bin");
    // 14-bit FSIZE (frame bytes - 1) at bit 46 and the DYNF flag at bit 76 of the core frame header.
    const FSIZE_BIT: usize = 46;
    const DYNF_BIT: usize = 76;

    /// The encoder's per-subframe RANGE gain (DYNF) is left out, as the reference decoder leaves it: the frame decodes
    /// to the uncompressed mix, the same samples as without it.
    #[test]
    fn the_legacy_dynamic_range_gain_is_not_applied() {
        let frame = &SURROUND[..1024];
        let header = crate::header::parse_frame_header(frame).unwrap();
        assert!(!header.dynamic_range && !header.crc_present);
        let header_bits = header.header_bit_length() as usize;
        let (coding, ach_bits) =
            crate::audio_header::decode_audio_coding_header_at(frame, header_bits, false).unwrap();
        assert_eq!(coding.n_subframes, 1);
        assert!(coding.joinx.iter().all(|&j| j == 0));
        let at = header_bits + ach_bits;
        let (_, side_bits) =
            crate::subframe::decode_primary_side_info_at(frame, at, &coding.channel_params)
                .unwrap();

        // The 8-bit RANGE code goes where the side-info tail starts: -20 dB.
        let mut bits = bits_of(frame);
        bits.splice(at + side_bits..at + side_bits, bits_of(&[0xB0]));
        bits[DYNF_BIT] = 1;
        put_bits(&mut bits, FSIZE_BIT, 14, 1024);
        let compressed = bytes_of(&bits);
        assert!(
            crate::header::parse_frame_header(&compressed)
                .unwrap()
                .dynamic_range
        );

        assert!(
            surround_frame_pcm(&compressed) == surround_frame_pcm(frame),
            "the gain was applied"
        );
    }

    /// Nor are the broadcast DRC values of a Rev2AUX chunk.
    #[test]
    fn the_rev2aux_dynamic_range_gain_is_not_applied() {
        let frame = &SURROUND[..1024];
        // Sync, 7-bit size - 1, the ES, broadcast, DRC and dialnorm flags, the DRC version, then one code per
        // subsubframe (two: -20 dB each), zero-padded to the CRC.
        let size = 8usize;
        let mut chunk = crate::rev2_aux::REV2_AUX_SYNC_WORD.to_be_bytes().to_vec();
        let mut fields = vec![0u8; 7 + 4 + 4];
        let size_code = size as u32 - 1;
        put_bits(&mut fields, 0, 7, size_code);
        put_bits(&mut fields, 7, 4, 0b0110);
        put_bits(
            &mut fields,
            11,
            4,
            u32::from(crate::rev2_aux::REV2_DRC_VERSION_SINGLE_BAND),
        );
        fields.resize(16, 0); // nByteAlign0
        fields.extend(bits_of(&[0xB0, 0xB0]));
        fields.resize((size - 2) * 8, 0);
        chunk.extend(bytes_of(&fields));
        let crc = crate::crc16::dts_crc16(&chunk[4..]);
        chunk.extend(crc.to_be_bytes());

        let mut bits = bits_of(frame);
        put_bits(&mut bits, FSIZE_BIT, 14, (1024 + chunk.len() - 1) as u32);
        let mut compressed = bytes_of(&bits);
        compressed.extend(&chunk);
        let found = crate::rev2_aux::parse_rev2_aux(
            &compressed,
            &crate::header::parse_frame_header(&compressed).unwrap(),
        )
        .unwrap()
        .unwrap();
        assert!(found.crc_valid && found.drc.is_some());

        assert!(
            surround_frame_pcm(&compressed) == surround_frame_pcm(frame),
            "the gain was applied"
        );
    }

    /// A pure unit test of the §C.2.4 matrix: feeding a two-channel pair
    /// whose sub-band samples are `(L+R, L-R)` back through
    /// `apply_sum_difference` recovers `(2L, 2R)` — the matrix is
    /// self-inverse up to the factor of two the encoder's scale factors
    /// absorb. Only the active sub-band columns are touched.
    #[test]
    fn apply_sum_difference_recovers_double_original() {
        // Two "original" channels, 3 sample rows, 2 active sub-bands.
        let l = [[1.0_f64, 2.0], [3.0, 4.0], [5.0, 6.0]];
        let r = [[10.0_f64, 20.0], [30.0, 40.0], [50.0, 60.0]];
        let mut matrices: Vec<SubbandSampleMatrix> = vec![vec![[0.0; NUM_SUBBAND]; 3]; 2];
        for row in 0..3 {
            for n in 0..2 {
                // Encoder side: store (L+R) in ch0, (L-R) in ch1.
                matrices[0][row][n] = l[row][n] + r[row][n];
                matrices[1][row][n] = l[row][n] - r[row][n];
            }
            // A high (inactive) sub-band that must be left untouched.
            matrices[0][row][20] = 7.0;
            matrices[1][row][20] = 9.0;
        }
        apply_sum_difference(&mut matrices, 0, 1, &mut [2, 2]).unwrap();
        for row in 0..3 {
            for n in 0..2 {
                assert_eq!(
                    matrices[0][row][n],
                    2.0 * l[row][n],
                    "ch0 row {row} sub {n}"
                );
                assert_eq!(
                    matrices[1][row][n],
                    2.0 * r[row][n],
                    "ch1 row {row} sub {n}"
                );
            }
            // Inactive sub-band 20 (>= nSUBS) untouched.
            assert_eq!(matrices[0][row][20], 7.0);
            assert_eq!(matrices[1][row][20], 9.0);
        }
    }

    /// A pair with unequal `nSUBS` matrixes up to the larger, as the PCM-domain butterfly would, and both channels
    /// are then synthesized that far.
    #[test]
    fn apply_sum_difference_reaches_the_larger_subband_count() {
        let mut sum = [0.0; NUM_SUBBAND];
        sum[0] = 3.0;
        sum[27] = 5.0;
        let mut difference = [0.0; NUM_SUBBAND];
        difference[0] = 1.0;
        let mut matrices: Vec<SubbandSampleMatrix> = vec![vec![sum; 2], vec![difference; 2]];
        let mut n_subs = [28, 20];
        apply_sum_difference(&mut matrices, 0, 1, &mut n_subs).unwrap();
        for (left, right) in matrices[0].iter().zip(&matrices[1]) {
            assert_eq!((left[0], right[0]), (4.0, 2.0));
            assert_eq!((left[27], right[27]), (5.0, 5.0));
        }
        assert_eq!(n_subs, [28, 28]);
    }
}
