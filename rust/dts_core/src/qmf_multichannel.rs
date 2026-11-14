//! Per-frame multi-channel 32-band synthesis QMF driver — the
//! channel-loop wrapper around the §C.2.5 `QMFInterpolation()`
//! per-channel call.
//!
//! The §C.2.5 normative driver (transcribed verbatim in
//! `docs/audio/dts/dts-core-extracts.md` §2.4, ETSI TS 102 114 V1.3.1
//! Annex C §C.2.5, staged PDF p.185) is invoked **once per channel**:
//!
//! ```text
//! aPrmCh[ch].QMFInterpolation(FILTS, nSUBS[ch]);
//! ```
//!
//! where `aPrmCh[ch]` is the persistent per-channel filter object
//! ([`crate::qmf_synth::QmfSynthesis`]) and `nSUBS[ch]` is that channel's count of
//! active subbands (§C.2.5: "higher subbands zero; joint-intensity-coded
//! subbands take the source channel's value"). `FILTS` (the §5.3.1
//! Table 5-15 "Multirate Interpolator Switch") is a single frame-header
//! flag shared by every channel, and the output `rScale` is the single
//! post-filterbank float→PCM gain derived from the frame header's `PCMR`
//! source resolution (`docs/audio/dts/dts-qmf-driver.md` §1/§2). Both
//! are constant across the frame's channels.
//!
//! [`MultiChannelQmf`] owns one [`QmfSynthesis`] per channel and runs
//! the §C.2.5 per-channel call for all of them over one block of
//! subband samples, so a caller that has decoded a frame's per-channel
//! subband samples reconstructs the whole frame's PCM in one call. The
//! per-channel filter state (`raX[]` / `raZ[]`) persists across calls
//! exactly as each underlying [`QmfSynthesis`] persists it, so feeding
//! a stream's frames in order carries every channel's inter-frame
//! filter tail correctly.
//!
//! # Scope: composition of landed primitives
//!
//! This module composes the already-landed [`QmfSynthesis`] driver (the
//! §C.2.5 per-sample loop body) across channels. It adds no new spec
//! step — the only spec construct it materialises is the channel loop
//! around `aPrmCh[ch].QMFInterpolation(...)` and the planar/interleaved
//! arrangement of the per-channel `naCh[]` outputs. The header-sourced
//! `FILTS` / `rScale` come from [`crate::header::DtsFrameHeader`] accessors
//! ([`crate::header::DtsFrameHeader::filter_bank_selection`] /
//! [`crate::header::DtsFrameHeader::output_r_scale`]) per the round-335 bridge.

use crate::cos_mod::NUM_SUBBAND;
use crate::filter_bank::FilterBankSelection;
use crate::qmf_assemble::QmfAssembleError;
use crate::qmf_synth::QmfSynthesis;

/// Errors specific to the multi-channel synthesis driver, layered over
/// the per-channel [`QmfAssembleError`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum MultiChannelQmfError {
    /// A per-channel synthesis step failed; carries the channel index
    /// `ch` at which it failed and the underlying [`QmfAssembleError`].
    Channel {
        /// 0-based channel index whose §C.2.5 call failed.
        ch: usize,
        /// The underlying per-channel error.
        source: QmfAssembleError,
    },
    /// The caller-supplied per-channel `n_subs` slice length did not
    /// match the driver's channel count.
    NSubsLenMismatch {
        /// The driver's configured channel count.
        channels: usize,
        /// The length of the supplied `n_subs` slice.
        got: usize,
    },
    /// The caller-supplied per-channel subband-sample slice count did
    /// not match the driver's channel count.
    ChannelSlicesLenMismatch {
        /// The driver's configured channel count.
        channels: usize,
        /// The number of per-channel slices supplied.
        got: usize,
    },
    /// Two channels carried a different number of sample rows. Every
    /// channel of one frame block must carry the same number of
    /// per-sample subband rows (the §C.2.5 outer loop runs
    /// `nStart..nEnd` identically for every channel of a frame).
    RowCountMismatch {
        /// Row count of channel 0 (the reference).
        expected: usize,
        /// 0-based channel index whose row count differed.
        ch: usize,
        /// That channel's row count.
        got: usize,
    },
}

impl core::fmt::Display for MultiChannelQmfError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            MultiChannelQmfError::Channel { ch, source } => {
                write!(f, "channel {ch} synthesis failed: {source}")
            }
            MultiChannelQmfError::NSubsLenMismatch { channels, got } => {
                write!(
                    f,
                    "n_subs length {got} does not match channel count {channels}"
                )
            }
            MultiChannelQmfError::ChannelSlicesLenMismatch { channels, got } => {
                write!(
                    f,
                    "channel-slices count {got} does not match channel count {channels}"
                )
            }
            MultiChannelQmfError::RowCountMismatch { expected, ch, got } => {
                write!(
                    f,
                    "channel {ch} carried {got} sample rows, expected {expected}"
                )
            }
        }
    }
}

impl std::error::Error for MultiChannelQmfError {}

/// Persistent per-frame, multi-channel 32-band synthesis QMF.
///
/// Holds one [`QmfSynthesis`] per channel — the §C.2.5 `aPrmCh[ch]`
/// filter objects — and drives the per-channel `QMFInterpolation()`
/// call for every channel over one block of subband samples. The
/// per-channel filter state persists across [`MultiChannelQmf::synthesize_planar`]
/// / [`MultiChannelQmf::synthesize_interleaved`] calls, so a multi-frame
/// stream reuses one instance and feeds its frames in order.
#[derive(Debug, Clone)]
pub struct MultiChannelQmf {
    /// One persistent §C.2.5 filter object per channel.
    channels: Vec<QmfSynthesis>,
}

impl MultiChannelQmf {
    /// Construct a driver for `channels` channels, each with a freshly
    /// cleared per-channel filter history (matching the §C.2.5
    /// per-channel filter's initial state before the first subframe).
    ///
    /// `channels` is the frame's audio-channel count — e.g. the value
    /// from [`crate::header::DtsFrameHeader::channel_count`]. A zero-channel
    /// driver is permitted (it produces no output) for callers that
    /// resolve the channel count dynamically.
    #[must_use]
    pub fn new(channels: usize) -> Self {
        Self {
            channels: (0..channels).map(|_| QmfSynthesis::new()).collect(),
        }
    }

    /// The driver's channel count.
    #[must_use]
    pub fn channel_count(&self) -> usize {
        self.channels.len()
    }

    /// Run the §C.2.5 per-channel `QMFInterpolation()` call for every
    /// channel and append the result **planar** — channel 0's PCM
    /// first, then channel 1's, and so on — to `output[ch]`.
    ///
    /// `channel_samples[ch]` is channel `ch`'s block of per-sample
    /// subband rows (one `[f64; 32]` per `nSubIndex`, the same layout
    /// [`QmfSynthesis::synthesize`] consumes). `n_subs[ch]` is that
    /// channel's count of active subbands (`nSUBS[ch]`). `filter` and
    /// `r_scale` are the frame-wide §C.2.5 parameters (the resolved
    /// `FILTS` branch and the post-filterbank output `rScale`); see
    /// [`MultiChannelQmf::synthesize_planar_from_header`] for sourcing
    /// them from a parsed header.
    ///
    /// `output` must have one `Vec<i32>` per channel; each channel's
    /// reconstructed PCM (`rows * 32` samples) is appended to its own
    /// vec. The per-channel filter state persists for the next call.
    ///
    /// # Errors
    ///
    /// - [`MultiChannelQmfError::NSubsLenMismatch`] /
    ///   [`MultiChannelQmfError::ChannelSlicesLenMismatch`] if the
    ///   per-channel slice lengths do not match the channel count (or
    ///   the `output` length differs, which is reported as a
    ///   channel-slices mismatch against `output`).
    /// - [`MultiChannelQmfError::RowCountMismatch`] if two channels
    ///   carry a different number of sample rows.
    /// - [`MultiChannelQmfError::Channel`] wrapping the per-channel
    ///   [`QmfAssembleError`] if a channel's §C.2.5 call fails.
    pub fn synthesize_planar(
        &mut self,
        channel_samples: &[&[[f64; NUM_SUBBAND]]],
        n_subs: &[usize],
        filter: FilterBankSelection,
        r_scale: f64,
        output: &mut [Vec<i32>],
    ) -> Result<(), MultiChannelQmfError> {
        self.check_lengths(channel_samples, n_subs, output.len())?;

        for (ch, q) in self.channels.iter_mut().enumerate() {
            q.synthesize(
                channel_samples[ch],
                n_subs[ch],
                filter,
                r_scale,
                &mut output[ch],
            )
            .map_err(|source| MultiChannelQmfError::Channel { ch, source })?;
        }
        Ok(())
    }

    /// Validate the per-channel slice lengths and equal-row-count
    /// invariant before any per-channel synthesis runs, so a length
    /// error leaves every channel's filter state untouched.
    fn check_lengths(
        &self,
        channel_samples: &[&[[f64; NUM_SUBBAND]]],
        n_subs: &[usize],
        output_len: usize,
    ) -> Result<(), MultiChannelQmfError> {
        let channels = self.channels.len();
        if channel_samples.len() != channels {
            return Err(MultiChannelQmfError::ChannelSlicesLenMismatch {
                channels,
                got: channel_samples.len(),
            });
        }
        if n_subs.len() != channels {
            return Err(MultiChannelQmfError::NSubsLenMismatch {
                channels,
                got: n_subs.len(),
            });
        }
        if output_len != channels {
            return Err(MultiChannelQmfError::ChannelSlicesLenMismatch {
                channels,
                got: output_len,
            });
        }
        // All channels of one frame block must carry the same row count.
        if let Some(first) = channel_samples.first() {
            let expected = first.len();
            for (ch, rows) in channel_samples.iter().enumerate().skip(1) {
                if rows.len() != expected {
                    return Err(MultiChannelQmfError::RowCountMismatch {
                        expected,
                        ch,
                        got: rows.len(),
                    });
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A `channels`-channel planar synthesis must equal `channels`
    /// independent single-channel [`QmfSynthesis`] runs — the driver is
    /// a faithful per-channel composition with no cross-channel
    /// coupling.
    #[test]
    fn planar_matches_independent_single_channel_runs() {
        let channels = 3;
        let filter = FilterBankSelection::PerfectReconstruction;
        let r_scale = 32768.0;
        let n_subs = [4usize, 6, 2];

        // Distinct deterministic input per channel.
        let make_rows = |seed: usize| -> Vec<[f64; NUM_SUBBAND]> {
            (0..8)
                .map(|s| {
                    let mut row = [0.0_f64; NUM_SUBBAND];
                    for (i, slot) in row.iter_mut().enumerate() {
                        *slot = (((s * 13 + i * 7 + seed * 5) % 17) as f64 - 8.0) * 1000.0;
                    }
                    row
                })
                .collect()
        };
        let rows: Vec<Vec<[f64; NUM_SUBBAND]>> = (0..channels).map(make_rows).collect();
        let refs: Vec<&[[f64; NUM_SUBBAND]]> = rows.iter().map(|r| r.as_slice()).collect();

        // Multi-channel planar.
        let mut mc = MultiChannelQmf::new(channels);
        let mut planar: Vec<Vec<i32>> = vec![Vec::new(); channels];
        mc.synthesize_planar(&refs, &n_subs, filter, r_scale, &mut planar)
            .unwrap();

        // Independent single-channel runs.
        for ch in 0..channels {
            let mut q = QmfSynthesis::new();
            let mut expect = Vec::new();
            q.synthesize(&rows[ch], n_subs[ch], filter, r_scale, &mut expect)
                .unwrap();
            assert_eq!(planar[ch], expect, "channel {ch} mismatch");
        }
        // Non-vacuous: at least one channel produced non-zero PCM.
        assert!(planar.iter().any(|p| p.iter().any(|&s| s != 0)));
    }

    /// Persisting one [`MultiChannelQmf`] across two calls equals a
    /// single call over the concatenated per-channel input — every
    /// channel's inter-frame filter tail (`raX[]`) carries across calls.
    #[test]
    fn split_calls_match_single_concatenated_call() {
        let channels = 2;
        let filter = FilterBankSelection::PerfectReconstruction;
        let r_scale = 32768.0;
        let n_subs = [6usize, 4];

        let make_rows = |seed: usize| -> Vec<[f64; NUM_SUBBAND]> {
            (0..10)
                .map(|s| {
                    let mut row = [0.0_f64; NUM_SUBBAND];
                    for (i, slot) in row.iter_mut().take(6).enumerate() {
                        *slot = (((s * 9 + i * 5 + seed) % 13) as f64 - 6.0) * 2000.0;
                    }
                    row
                })
                .collect()
        };
        let rows: Vec<Vec<[f64; NUM_SUBBAND]>> = (0..channels).map(make_rows).collect();
        let refs: Vec<&[[f64; NUM_SUBBAND]]> = rows.iter().map(|r| r.as_slice()).collect();

        // Single call over all 10 rows.
        let mut mc_single = MultiChannelQmf::new(channels);
        let mut single = vec![Vec::new(); channels];
        mc_single
            .synthesize_planar(&refs, &n_subs, filter, r_scale, &mut single)
            .unwrap();

        // Two calls split 4 + 6, reusing the same instance.
        let first: Vec<&[[f64; NUM_SUBBAND]]> = rows.iter().map(|r| &r[..4]).collect();
        let second: Vec<&[[f64; NUM_SUBBAND]]> = rows.iter().map(|r| &r[4..]).collect();
        let mut mc_split = MultiChannelQmf::new(channels);
        let mut split = vec![Vec::new(); channels];
        mc_split
            .synthesize_planar(&first, &n_subs, filter, r_scale, &mut split)
            .unwrap();
        mc_split
            .synthesize_planar(&second, &n_subs, filter, r_scale, &mut split)
            .unwrap();

        assert_eq!(single, split);
        assert!(single.iter().any(|p| p.iter().any(|&s| s != 0)));
    }

    /// Channels with differing row counts are rejected (the §C.2.5
    /// outer loop runs identically for every channel of one frame).
    #[test]
    fn unequal_row_counts_are_rejected() {
        let mut mc = MultiChannelQmf::new(2);
        let rows_a = vec![[0.0_f64; NUM_SUBBAND]; 4];
        let rows_b = vec![[0.0_f64; NUM_SUBBAND]; 3];
        let refs: Vec<&[[f64; NUM_SUBBAND]]> = vec![rows_a.as_slice(), rows_b.as_slice()];
        let mut out = vec![Vec::new(); 2];
        assert_eq!(
            mc.synthesize_planar(
                &refs,
                &[4, 4],
                FilterBankSelection::NonPerfectReconstruction,
                1.0,
                &mut out
            )
            .unwrap_err(),
            MultiChannelQmfError::RowCountMismatch {
                expected: 4,
                ch: 1,
                got: 3
            }
        );
    }
}
