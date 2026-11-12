// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Converts one decoded source to the device format (docs/Plan.md §5.2, §5.3).
//!
//! Integer sources the device takes as they are only repack (zero padding) and never touch float. Mono is
//! duplicated to both channels bit for bit. A float-only device gets integer samples widened exactly (at most 24
//! bits). Everything else — resampling, ITU-R BS.775 downmix, bit reduction and the decoded output of lossy sources —
//! is processed in 64-bit float and quantized once at the end with TPDF dither. Raw DSD reaches a native DSD or DoP
//! stream bit for bit (mono duplicated), or with the same permission as DSD to PCM at a lower DSD rate, modulated
//! again (crate::dsd_remod). DSD becomes PCM only with explicit permission: it is decimated to
//! 352.8 kHz, mixed and resampled in float like any processed source, and quantized once. While the DSP chain changes
//! the signal, every PCM path runs in float through it and is quantized once at the device depth; raw DSD bypasses it.

use crate::decoder_host::{CamelSourceInfoV1, SAMPLE_ENCODING_DSD, SAMPLE_ENCODING_FLOAT32};
use crate::dsd::{put_dop_sample, DOP_MARKERS, DSD_SILENCE};
use crate::dsd2pcm::{DsdDecimator, PCM_RATE};
use crate::dsd_remod::DsdRemodulator;
use crate::dsp::Dsp;
use crate::packing::pack_pcm;
use crate::resample::Resampler;
use crate::{
    AudioFormat, AUDIO_DOP, AUDIO_DSD, AUDIO_PCM, ENGINE_E_UNSUPPORTED_FORMAT, SAMPLE_F32_LE,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Mode {
    Bypass,
    Duplicate,
    Widen,
    Process,
    /// DSD byte frames to a native DSD stream.
    Dsd,
    /// Two DSD byte frames to one DoP frame.
    Dop,
    /// DSD decimated to PCM, then processed like `Process`.
    DsdPcm,
}

pub(crate) struct Converter {
    mode: Mode,
    in_channels: usize,
    in_bits: u32,
    in_float: bool,
    out: AudioFormat,
    out_channels: usize,
    /// Depth of the final quantization: the source depth, or less when the device takes less.
    quantize_bits: u32,
    /// The track's ReplayGain as a factor, applied before the DSP chain; 1 leaves the samples untouched.
    gain: f64,
    /// Depth of the final quantization while the DSP chain acts: the device's, 24 at most (0 for float).
    dsp_bits: u32,
    matrix: Option<[Vec<f64>; 2]>,
    resampler: Option<Resampler>,
    dither: Tpdf,
    pending: Vec<u8>,
    pending_frames: u64,
    finished: bool,
    bytes_per_frame: usize,
    /// DoP: a byte frame waiting for the one that completes its DoP frame.
    carry: Vec<u8>,
    decimator: Option<DsdDecimator>,
    /// Raw DSD to a lower DSD rate.
    remodulator: Option<DsdRemodulator>,
    in_rate: u32,
    out_rate: u32,
    scratch: Scratch,
}

// Working buffers, sized once so converting needs no heap.
#[derive(Default)]
struct Scratch {
    words: Vec<i32>,
    // Source frames as float, on the source's channels.
    values: Vec<f64>,
    // Mixed to the device's channels, before resampling.
    mixed: Vec<f64>,
    // Device-rate frames on the device's channels.
    output: Vec<f64>,
}

// A resampler's flushed tail, beyond what one block produces.
const RESAMPLER_FLUSH_FRAMES: usize = 8192;
// The most channels a DSD stream carries to the device.
const MAX_DSD_CHANNELS: usize = 8;

/// Stereo downmix coefficients per source channel count, in WAVE/FLAC default order (ITU-R BS.775: centre and
/// surrounds at -3 dB, LFE dropped). 7: L R C LFE Cs Ls Rs; 8: L R C LFE Lb Rb Ls Rs. With a `mask`, by the speaker
/// each channel names instead.
fn downmix_matrix(channels: usize, mask: u32) -> Option<[Vec<f64>; 2]> {
    if mask != 0 {
        return downmix_by_speaker(channels, mask);
    }
    let a = std::f64::consts::FRAC_1_SQRT_2;
    Some(match channels {
        1 => [vec![1.0], vec![1.0]],
        2 => [vec![1.0, 0.0], vec![0.0, 1.0]],
        3 => [vec![1.0, 0.0, a], vec![0.0, 1.0, a]],
        4 => [vec![1.0, 0.0, a, 0.0], vec![0.0, 1.0, 0.0, a]],
        5 => [vec![1.0, 0.0, a, a, 0.0], vec![0.0, 1.0, a, 0.0, a]],
        6 => [
            vec![1.0, 0.0, a, 0.0, a, 0.0],
            vec![0.0, 1.0, a, 0.0, 0.0, a],
        ],
        7 => [
            vec![1.0, 0.0, a, 0.0, 0.5, a, 0.0],
            vec![0.0, 1.0, a, 0.0, 0.5, 0.0, a],
        ],
        8 => [
            vec![1.0, 0.0, a, 0.0, a, 0.0, a, 0.0],
            vec![0.0, 1.0, a, 0.0, 0.0, a, 0.0, a],
        ],
        _ => return None,
    })
}

/// The downmix of channels on the WAVE speaker positions of `mask`, one bit per channel in their order: a centre at
/// -3 dB, or whole on both sides when there is no front pair, surrounds at -3 dB, a back centre at -6 dB, LFE dropped.
fn downmix_by_speaker(channels: usize, mask: u32) -> Option<[Vec<f64>; 2]> {
    const FRONT: u32 = 0x1 | 0x2;
    let a = std::f64::consts::FRAC_1_SQRT_2;
    if mask.count_ones() as usize != channels {
        return None;
    }
    let mut out = [Vec::with_capacity(channels), Vec::with_capacity(channels)];
    for bit in (0..32).map(|i| 1u32 << i).filter(|bit| mask & bit != 0) {
        let (left, right) = match bit {
            0x1 => (1.0, 0.0),
            0x2 => (0.0, 1.0),
            0x4 if mask & FRONT == 0 => (1.0, 1.0),
            0x4 => (a, a),
            0x8 => (0.0, 0.0),
            0x10 | 0x200 => (a, 0.0),
            0x20 | 0x400 => (0.0, a),
            0x100 => (0.5, 0.5),
            _ => return None,
        };
        out[0].push(left);
        out[1].push(right);
    }
    Some(out)
}

/// Triangular dither of +-1 LSB from two uniform values. Deterministic, so conversions are reproducible.
struct Tpdf(u64);

impl Tpdf {
    fn uniform(&mut self) -> f64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 11) as f64 / (1u64 << 53) as f64
    }
    fn next(&mut self) -> f64 {
        self.uniform() - self.uniform()
    }
}

impl Converter {
    /// `allow` is the "convert when unsupported" setting; a lossy source may still be quantized to its configured
    /// depth without it. `allow_dsd` permits DSD to PCM and DSD to a lower DSD rate; downmixing DSD also needs `allow`.
    pub(crate) fn new(
        info: &CamelSourceInfoV1,
        out: &AudioFormat,
        out_rate: u32,
        allow: bool,
        allow_dsd: bool,
        quality: u32,
    ) -> Result<Self, i32> {
        let mut converter = Self::build(info, out, out_rate, allow, allow_dsd, quality)?;
        converter.reserve();
        Ok(converter)
    }

    fn build(
        info: &CamelSourceInfoV1,
        out: &AudioFormat,
        out_rate: u32,
        allow: bool,
        allow_dsd: bool,
        quality: u32,
    ) -> Result<Self, i32> {
        let in_channels = info.channel_count as usize;
        let out_channels = out.channel_count as usize;
        let in_dsd = info.sample_encoding == SAMPLE_ENCODING_DSD;
        if in_dsd && out.stream_kind == AUDIO_PCM {
            if !allow_dsd || out_channels != 2 || (in_channels > 2 && !allow) {
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            }
            let decimator = DsdDecimator::new(info.sample_rate, in_channels)
                .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?;
            let resampler = if out_rate == PCM_RATE {
                None
            } else {
                Some(
                    Resampler::new(PCM_RATE, out_rate, out_channels, quality)
                        .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                )
            };
            return Ok(Self {
                mode: Mode::DsdPcm,
                in_channels,
                in_bits: info.valid_bits,
                in_float: false,
                out: *out,
                out_channels,
                // Like any processed source: 24-bit at most, MSB-aligned in a 32-bit container.
                quantize_bits: if out.sample_format == SAMPLE_F32_LE {
                    0
                } else {
                    out.valid_bits.min(24)
                },
                gain: 1.0,
                dsp_bits: Self::device_bits(out),
                matrix: Some(
                    downmix_matrix(in_channels, info.channel_mask)
                        .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                ),
                resampler,
                dither: Tpdf(0x9E37_79B9_7F4A_7C15),
                pending: Vec::new(),
                pending_frames: 0,
                finished: false,
                bytes_per_frame: crate::packing::bytes_per_frame(out)
                    .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                carry: Vec::new(),
                decimator: Some(decimator),
                remodulator: None,
                in_rate: info.sample_rate,
                out_rate,
                scratch: Scratch::default(),
            });
        }
        if in_dsd || out.stream_kind != AUDIO_PCM {
            // DSD to PCM is a separate, explicitly enabled path (docs/Plan.md §5.8).
            let out_byte_rate = match out.stream_kind {
                AUDIO_DSD if out_rate.is_multiple_of(8) => out_rate / 8,
                AUDIO_DOP => out_rate.saturating_mul(2),
                _ => 0,
            };
            let channels_kept =
                in_channels == out_channels || (in_channels == 1 && out_channels == 2);
            if !in_dsd || out_byte_rate == 0 || !channels_kept {
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            }
            let remodulator = if out_byte_rate == info.sample_rate {
                None
            } else if allow_dsd {
                Some(
                    DsdRemodulator::new(info.sample_rate, out_byte_rate, in_channels)
                        .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                )
            } else {
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            };
            return Ok(Self {
                mode: if out.stream_kind == AUDIO_DSD {
                    Mode::Dsd
                } else {
                    Mode::Dop
                },
                in_channels,
                in_bits: info.valid_bits,
                in_float: false,
                out: *out,
                out_channels,
                quantize_bits: 0,
                gain: 1.0,
                dsp_bits: 0,
                matrix: None,
                resampler: None,
                dither: Tpdf(0),
                pending: Vec::new(),
                pending_frames: 0,
                finished: false,
                bytes_per_frame: crate::packing::bytes_per_frame(out)
                    .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                carry: Vec::new(),
                decimator: None,
                remodulator,
                in_rate: info.sample_rate,
                out_rate,
                scratch: Scratch::default(),
            });
        }
        let in_float = info.sample_encoding == SAMPLE_ENCODING_FLOAT32;
        let same_rate = info.sample_rate == out_rate;
        let float_out = out.sample_format == SAMPLE_F32_LE;
        let channels_kept = in_channels == out_channels || (in_channels == 1 && out_channels == 2);
        let repackable = pack_pcm(&[], info.valid_bits, out, &mut []).is_ok();
        let mode = if !in_float && same_rate && !float_out && repackable && channels_kept {
            if in_channels == out_channels {
                Mode::Bypass
            } else {
                Mode::Duplicate
            }
        } else if !in_float && same_rate && float_out && info.valid_bits <= 24 && channels_kept {
            Mode::Widen
        } else {
            Mode::Process
        };
        let quantize_bits = if float_out {
            0
        } else {
            info.valid_bits.min(out.valid_bits)
        };
        if mode == Mode::Process {
            let lossy_decode_only = in_float
                && same_rate
                && channels_kept
                && (float_out || quantize_bits == info.valid_bits);
            if !(allow || lossy_decode_only) || out_channels != 2 {
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            }
        }
        let matrix = if mode == Mode::Process {
            Some(
                downmix_matrix(in_channels, info.channel_mask)
                    .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
            )
        } else {
            None
        };
        let resampler = if mode == Mode::Process && !same_rate {
            Some(
                Resampler::new(info.sample_rate, out_rate, out_channels, quality)
                    .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
            )
        } else {
            None
        };
        let bytes_per_frame =
            crate::packing::bytes_per_frame(out).ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?;
        Ok(Self {
            mode,
            in_channels,
            in_bits: info.valid_bits,
            in_float,
            out: *out,
            out_channels,
            quantize_bits,
            gain: 1.0,
            dsp_bits: Self::device_bits(out),
            matrix,
            resampler,
            dither: Tpdf(0x9E37_79B9_7F4A_7C15),
            pending: Vec::new(),
            pending_frames: 0,
            finished: false,
            bytes_per_frame,
            carry: Vec::new(),
            decimator: None,
            remodulator: None,
            in_rate: info.sample_rate,
            out_rate,
            scratch: Scratch::default(),
        })
    }

    /// Source frames to decode for about `output_frames` of output; any excess waits in the pending output.
    pub(crate) fn input_frames_for(&self, output_frames: u64) -> u64 {
        if self.mode == Mode::DsdPcm {
            let frames =
                u128::from(output_frames) * u128::from(self.in_rate) / u128::from(self.out_rate);
            return (frames as u64).max(1);
        }
        let raw = if self.mode == Mode::Dop {
            output_frames * 2
        } else {
            output_frames
        };
        match &self.resampler {
            Some(r) => ((output_frames as f64 / r.ratio()).floor() as u64).max(1),
            None if self.remodulator.is_some() => {
                let out_byte_rate = if self.mode == Mode::Dop {
                    self.out_rate * 2
                } else {
                    self.out_rate / 8
                };
                raw * u64::from(self.in_rate / out_byte_rate)
            }
            None => raw,
        }
    }

    pub(crate) fn pending(&self) -> &[u8] {
        &self.pending
    }

    pub(crate) fn pending_frames(&self) -> u64 {
        self.pending_frames
    }

    /// Drops the first `frames` pending output frames once they are in the ring.
    pub(crate) fn consume(&mut self, frames: u64) {
        let bytes = frames as usize * self.bytes_per_frame;
        self.pending.drain(..bytes);
        self.pending_frames -= frames;
    }

    pub(crate) fn finished(&self) -> bool {
        self.finished
    }

    /// Converts `frames` decoded frames (`samples` interleaved: right-aligned integers, or float32 bit patterns for a
    /// float source) and appends the device bytes to the pending output. `dsp` processes PCM while it acts.
    pub(crate) fn convert(
        &mut self,
        samples: &[i32],
        frames: usize,
        dsp: &mut Dsp,
    ) -> Result<(), i32> {
        let input = &samples[..frames * self.in_channels];
        let processes = self.processes(dsp);
        match self.mode {
            Mode::Bypass if !processes => self.pack(input, self.in_bits),
            Mode::Duplicate if !processes => {
                let mut stereo = std::mem::take(&mut self.scratch.words);
                stereo.clear();
                stereo.extend(input.iter().flat_map(|&s| [s, s]));
                let result = self.pack(&stereo, self.in_bits);
                self.scratch.words = stereo;
                result
            }
            Mode::Dsd | Mode::Dop => {
                let Some(remodulator) = self.remodulator.as_mut() else {
                    self.push_raw(input);
                    return Ok(());
                };
                let mut words = std::mem::take(&mut self.scratch.words);
                words.clear();
                remodulator.process(input, frames, &mut words);
                self.push_raw(&words);
                self.scratch.words = words;
                Ok(())
            }
            _ => {
                let mut floats = std::mem::take(&mut self.scratch.output);
                floats.clear();
                let result = self
                    .append_floats(input, frames, &mut floats)
                    .and_then(|()| self.emit(&mut floats, dsp));
                self.scratch.output = floats;
                result
            }
        }
    }

    /// Whether the track can be mixed into a crossfade: every PCM path can, raw DSD cannot.
    pub(crate) fn mixes(&self) -> bool {
        !matches!(self.mode, Mode::Dsd | Mode::Dop)
    }

    /// Converts `frames` decoded frames as `convert` does, but appends them to `out` as device-rate float frames with
    /// the track's gain, before the DSP chain, for a crossfade to mix.
    pub(crate) fn floats(
        &mut self,
        samples: &[i32],
        frames: usize,
        out: &mut Vec<f64>,
    ) -> Result<(), i32> {
        let start = out.len();
        self.append_floats(&samples[..frames * self.in_channels], frames, out)?;
        self.apply_gain(&mut out[start..]);
        Ok(())
    }

    /// Ends the source like `finish`, appending the resampler's tail to `out` as `floats` does.
    pub(crate) fn finish_floats(&mut self, out: &mut Vec<f64>) -> Result<(), i32> {
        if self.finished {
            return Ok(());
        }
        self.finished = true;
        let start = out.len();
        if let Some(resampler) = self.resampler.as_mut() {
            resampler
                .flush(out)
                .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT)?;
        }
        self.apply_gain(&mut out[start..]);
        Ok(())
    }

    /// Appends mixed float frames, which carry each track's gain already, through the DSP chain to the pending output,
    /// quantized as processed.
    pub(crate) fn emit_mixed(&mut self, floats: &mut [f64], dsp: &mut Dsp) -> Result<(), i32> {
        self.quantize(floats, dsp, true)
    }

    /// The converter with the track's ReplayGain as a factor.
    pub(crate) fn with_gain(mut self, gain: f64) -> Self {
        self.gain = gain;
        self
    }

    // Whether PCM goes through float: a gain or the DSP chain changes it.
    fn processes(&self, dsp: &Dsp) -> bool {
        self.gain != 1.0 || dsp.active()
    }

    /// Ends the source: flushes the resampler's tail into the pending output. Further calls do nothing.
    pub(crate) fn finish(&mut self, dsp: &mut Dsp) -> Result<(), i32> {
        if self.finished {
            return Ok(());
        }
        self.finished = true;
        if self.mode == Mode::Dop && !self.carry.is_empty() {
            // An odd final byte frame is completed with idle pattern; the stream stays whole DoP frames.
            self.push_dop(&[DSD_SILENCE; MAX_DSD_CHANNELS][..self.out_channels]);
        }
        let Some(resampler) = self.resampler.as_mut() else {
            return Ok(());
        };
        let mut tail = std::mem::take(&mut self.scratch.output);
        tail.clear();
        let result = resampler
            .flush(&mut tail)
            .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT)
            .and_then(|()| self.emit(&mut tail, dsp));
        self.scratch.output = tail;
        result
    }

    /// Emits `frames` frames of the DSP chain's tail after the stream's end: silence through the chain. DSD sent as it
    /// is passes no chain, so it has no tail.
    pub(crate) fn drain(&mut self, frames: usize, dsp: &mut Dsp) -> Result<(), i32> {
        if matches!(self.mode, Mode::Dsd | Mode::Dop) {
            return Ok(());
        }
        let mut silence = std::mem::take(&mut self.scratch.output);
        silence.clear();
        silence.resize(frames * self.out_channels, 0.0);
        let result = self.emit(&mut silence, dsp);
        self.scratch.output = silence;
        result
    }

    // Appends `frames` decoded frames to `out` as float frames on the device's channels at the device rate, before the
    // track's gain.
    fn append_floats(
        &mut self,
        input: &[i32],
        frames: usize,
        out: &mut Vec<f64>,
    ) -> Result<(), i32> {
        let mut values = std::mem::take(&mut self.scratch.values);
        values.clear();
        match self.mode {
            Mode::Bypass | Mode::Duplicate | Mode::Widen => self.to_device_channels(input, out),
            Mode::Process => {
                let scale = if self.in_float {
                    1.0
                } else {
                    1.0 / f64::from(1u32 << (self.in_bits - 1))
                };
                values.extend(input.iter().map(|&s| {
                    if self.in_float {
                        f64::from(f32::from_bits(s as u32))
                    } else {
                        f64::from(s) * scale
                    }
                }));
            }
            Mode::DsdPcm => {
                self.decimator
                    .as_mut()
                    .expect("DSD to PCM has a decimator")
                    .process(input, frames, &mut values);
            }
            Mode::Dsd | Mode::Dop => {
                self.scratch.values = values;
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            }
        }
        let result = if matches!(self.mode, Mode::Process | Mode::DsdPcm) {
            self.mix_and_resample(&values, out)
        } else {
            Ok(())
        };
        self.scratch.values = values;
        result
    }

    // Mixes float frames of `in_channels` to stereo and resamples them where the rate changes, appending to `out`.
    fn mix_and_resample(&mut self, values: &[f64], out: &mut Vec<f64>) -> Result<(), i32> {
        if self.resampler.is_none() {
            self.mix_floats(values, out);
            return Ok(());
        }
        let mut mixed = std::mem::take(&mut self.scratch.mixed);
        mixed.clear();
        self.mix_floats(values, &mut mixed);
        let result = self
            .resampler
            .as_mut()
            .expect("checked above")
            .process(&mixed, out)
            .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT);
        self.scratch.mixed = mixed;
        result
    }

    // Integer source frames as float in [-1, 1) on the device's channels, a mono channel on both.
    fn to_device_channels(&self, input: &[i32], out: &mut Vec<f64>) {
        let scale = 1.0 / f64::from(1u32 << (self.in_bits - 1));
        for frame in input.chunks_exact(self.in_channels) {
            for c in 0..self.out_channels {
                out.push(f64::from(frame[c.min(self.in_channels - 1)]) * scale);
            }
        }
    }

    // Float frames of `in_channels`, mixed to stereo and appended to `out`.
    fn mix_floats(&self, input: &[f64], out: &mut Vec<f64>) {
        let matrix = self
            .matrix
            .as_ref()
            .expect("processing has a channel matrix");
        for frame in input.chunks_exact(self.in_channels) {
            for row in matrix {
                out.push(frame.iter().zip(row).map(|(&s, &k)| s * k).sum());
            }
        }
    }

    // Like any processed source: 24-bit at most, MSB-aligned in a 32-bit container.
    fn device_bits(out: &AudioFormat) -> u32 {
        if out.sample_format == SAMPLE_F32_LE {
            0
        } else {
            out.valid_bits.min(24)
        }
    }

    // The single final stage, after the DSP chain while it acts: float32 for a float device, else TPDF-dithered
    // integers at `quantize_bits`, or at the device depth while the chain acts.
    fn emit(&mut self, floats: &mut [f64], dsp: &mut Dsp) -> Result<(), i32> {
        let processed = self.processes(dsp);
        self.apply_gain(floats);
        self.quantize(floats, dsp, processed)
    }

    fn apply_gain(&self, floats: &mut [f64]) {
        if self.gain != 1.0 {
            for v in floats.iter_mut() {
                *v *= self.gain;
            }
        }
    }

    fn quantize(&mut self, floats: &mut [f64], dsp: &mut Dsp, processed: bool) -> Result<(), i32> {
        if dsp.active() {
            dsp.process(floats);
        } else {
            // A downmix, a resampler's overshoot or a gain can pass full scale too.
            dsp.count_clipped(floats);
        }
        if self.out.sample_format == SAMPLE_F32_LE {
            // As in the integer path, what is not a number is silence.
            for &v in floats.iter() {
                let v = if v.is_nan() { 0.0 } else { v.clamp(-1.0, 1.0) };
                self.pending.extend_from_slice(&(v as f32).to_le_bytes());
            }
            self.pending_frames += (floats.len() / self.out_channels) as u64;
            return Ok(());
        }
        if !processed && !matches!(self.mode, Mode::Process | Mode::DsdPcm) {
            return Err(ENGINE_E_UNSUPPORTED_FORMAT);
        }
        let bits = if processed {
            self.dsp_bits
        } else {
            self.quantize_bits
        };
        let full = f64::from(1u32 << (bits - 1));
        let mut integers = std::mem::take(&mut self.scratch.words);
        integers.clear();
        for &v in floats.iter() {
            let q = (v * full + self.dither.next()).round();
            integers.push(if q.is_nan() {
                0
            } else {
                q.clamp(-full, full - 1.0) as i32
            });
        }
        let result = self.pack(&integers, bits);
        self.scratch.words = integers;
        result
    }

    // DSD byte frames of the source's channels to the pending native DSD or DoP output.
    fn push_raw(&mut self, input: &[i32]) {
        for frame in input.chunks_exact(self.in_channels) {
            let mut bytes = [0u8; MAX_DSD_CHANNELS];
            for (c, byte) in bytes[..self.out_channels].iter_mut().enumerate() {
                *byte = frame[c.min(self.in_channels - 1)] as u8;
            }
            if self.mode == Mode::Dsd {
                self.pending.extend_from_slice(&bytes[..self.out_channels]);
                self.pending_frames += 1;
            } else if self.carry.is_empty() {
                self.carry.extend_from_slice(&bytes[..self.out_channels]);
            } else {
                self.push_dop(&bytes[..self.out_channels]);
            }
        }
    }

    // One DoP frame from the carried byte frame and `second`. Render stamps the real markers.
    fn push_dop(&mut self, second: &[u8]) {
        let bytes_per_sample = self.bytes_per_frame / self.out_channels;
        let start = self.pending.len();
        self.pending.resize(start + self.bytes_per_frame, 0);
        for (c, sample) in self.pending[start..]
            .chunks_exact_mut(bytes_per_sample)
            .enumerate()
        {
            put_dop_sample(sample, self.carry[c], second[c], DOP_MARKERS[0]);
        }
        self.carry.clear();
        self.pending_frames += 1;
    }

    fn pack(&mut self, samples: &[i32], bits: u32) -> Result<(), i32> {
        let frames = samples.len() / self.out_channels;
        let start = self.pending.len();
        self.pending
            .resize(start + frames * self.bytes_per_frame, 0);
        let written = pack_pcm(samples, bits, &self.out, &mut self.pending[start..])?;
        self.pending.truncate(start + written);
        self.pending_frames += frames as u64;
        Ok(())
    }

    // Sizes the pending output and every working buffer for the largest block the DSP stage converts at once, the output
    // it resamples to and a crossfade's mixed blocks, so converting needs no heap (docs/Plan.md §5.2).
    fn reserve(&mut self) {
        let block = crate::MAX_PREFILL_FRAMES as usize;
        let output = self
            .resampler
            .as_ref()
            .map_or(block, |r| r.output_capacity(block) + RESAMPLER_FLUSH_FRAMES)
            + 3 * block;
        let out_channels = self.out_channels.max(2);
        self.pending.reserve_exact(output * self.bytes_per_frame);
        self.carry.reserve_exact(out_channels);
        if let Some(remodulator) = self.remodulator.as_mut() {
            remodulator.reserve(block);
        }
        let scratch = &mut self.scratch;
        scratch.words.reserve_exact(output * out_channels);
        scratch
            .values
            .reserve_exact(block * self.in_channels.max(out_channels));
        scratch.mixed.reserve_exact(block * out_channels);
        scratch.output.reserve_exact(output * out_channels);
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::{Converter, Dsp, Mode};
    use crate::decoder_host::{
        CamelSourceInfoV1, SAMPLE_ENCODING_FLOAT32, SAMPLE_ENCODING_INTEGER,
    };
    use crate::{
        AudioFormat, ABI_VERSION, AUDIO_PCM, ENGINE_E_UNSUPPORTED_FORMAT, QUALITY_HIGH,
        SAMPLE_F32_LE, SAMPLE_S16_LE, SAMPLE_S24_LE, SAMPLE_S32_LE,
    };

    // A DSP chain that never acts, as when every node is off.
    fn flat() -> Dsp {
        Dsp::new(48_000, 2)
    }

    fn source(rate: u32, channels: u32, bits: u32, float: bool) -> CamelSourceInfoV1 {
        CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: ABI_VERSION,
            sample_rate: rate,
            channel_count: channels,
            valid_bits: bits,
            sample_encoding: if float {
                SAMPLE_ENCODING_FLOAT32
            } else {
                SAMPLE_ENCODING_INTEGER
            },
            total_frames: 0,
            has_md5: 0,
            channel_mask: 0,
            md5: [0; 16],
        }
    }

    fn device(sample_format: u32) -> AudioFormat {
        let (valid_bits, container_bits) = match sample_format {
            SAMPLE_S16_LE => (16, 16),
            SAMPLE_S24_LE => (24, 24),
            _ => (32, 32),
        };
        AudioFormat {
            stream_kind: AUDIO_PCM,
            sample_format,
            channel_count: 2,
            valid_bits,
            container_bits,
        }
    }

    fn floats(bytes: &[u8]) -> Vec<f32> {
        let (chunks, _) = bytes.as_chunks::<4>();
        chunks.iter().map(|&b| f32::from_le_bytes(b)).collect()
    }

    fn ints(bytes: &[u8], width: usize) -> Vec<i32> {
        let shift = 32 - 8 * width as u32;
        bytes
            .chunks_exact(width)
            .map(|b| {
                let mut v = 0u32;
                for (i, &byte) in b.iter().enumerate() {
                    v |= u32::from(byte) << (8 * i);
                }
                ((v << shift) as i32) >> shift
            })
            .collect()
    }

    fn converter(info: CamelSourceInfoV1, out: u32, rate: u32, allow: bool) -> Converter {
        Converter::new(&info, &device(out), rate, allow, false, QUALITY_HIGH).unwrap()
    }

    #[test]
    fn an_acting_chain_dithers_to_24_bits_in_a_32_bit_device() {
        let mut c = converter(source(48_000, 2, 24, false), SAMPLE_S32_LE, 48_000, false);
        assert_eq!(c.mode, Mode::Bypass);
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(crate::dsp::Settings {
            order: crate::dsp::DEFAULT_ORDER,
            crossfeed: Some(crate::crossfeed::Params {
                cutoff_hz: 700,
                feed_db: 4.5,
            }),
            ..crate::dsp::Settings::default()
        });
        let samples = [1 << 20, -(1 << 20), 1 << 21, -(1 << 21)];
        c.convert(&samples, 2, &mut dsp).unwrap();
        let out = ints(c.pending(), 4);
        assert_eq!(out.len(), 4);
        assert!(out.iter().all(|s| s & 0xff == 0), "{out:?}");
        assert_ne!(out, samples.map(|s| s << 8));
    }

    // A DSP chain set on an engine that sends DSD as it is has nothing to act on: the chain's tail after the stream's
    // end adds nothing to the DSD, nor quantizes at the 0 bits a DSD converter has.
    #[test]
    fn a_dsp_tail_on_raw_dsd_adds_nothing() {
        let mut info = source(352_800, 2, 1, false);
        info.sample_encoding = crate::SAMPLE_ENCODING_DSD;
        let out = AudioFormat {
            stream_kind: crate::AUDIO_DSD,
            sample_format: crate::SAMPLE_DSD_U8,
            channel_count: 2,
            valid_bits: 1,
            container_bits: 8,
        };
        let mut c = Converter::new(&info, &out, 2_822_400, false, false, QUALITY_HIGH).unwrap();
        assert_eq!(c.mode, Mode::Dsd);
        let mut dsp = Dsp::new(352_800, 2);
        dsp.set(crate::dsp::Settings {
            order: crate::dsp::DEFAULT_ORDER,
            crossfeed: Some(crate::crossfeed::Params {
                cutoff_hz: 700,
                feed_db: 4.5,
            }),
            ..crate::dsp::Settings::default()
        });
        assert!(dsp.active());
        c.convert(&[0x69, 0x69, 0x96, 0x96], 2, &mut dsp).unwrap();
        c.finish(&mut dsp).unwrap();
        let before = c.pending().to_vec();
        assert_eq!(c.drain(1024, &mut dsp), Ok(()));
        assert_eq!(c.pending(), before.as_slice());
    }

    // A downmix that sums past full scale clips in the final quantization, and the clip is counted, as the DSP chain's
    // are, though no chain acts.
    #[test]
    fn a_downmix_that_clips_is_counted() {
        let mut c = converter(source(48_000, 6, 24, false), SAMPLE_S24_LE, 48_000, true);
        assert_eq!(c.mode, Mode::Process);
        let full = (1 << 23) - 1;
        // L, R, C, LFE, Ls, Rs: full-scale L, C and Ls sum to about 2.41 on the left.
        let frame = [full, 0, full, 0, full, 0];
        let samples: Vec<i32> = frame.iter().copied().cycle().take(6 * 64).collect();
        let mut dsp = flat();
        c.convert(&samples, 64, &mut dsp).unwrap();
        assert!(!dsp.active());
        assert_eq!(dsp.clipped(), 64);
    }

    #[test]
    fn devices_that_take_the_source_only_repack_it() {
        let mut c = converter(source(48_000, 2, 24, false), SAMPLE_S24_LE, 48_000, false);
        assert_eq!(c.mode, Mode::Bypass);
        let samples = [1, -1, 8_388_607, -8_388_608];
        c.convert(&samples, 2, &mut flat()).unwrap();
        assert_eq!(ints(c.pending(), 3), samples);
    }

    #[test]
    fn mono_is_duplicated_bit_for_bit() {
        let mut c = converter(source(44_100, 1, 16, false), SAMPLE_S32_LE, 44_100, false);
        assert_eq!(c.mode, Mode::Duplicate);
        c.convert(&[123, -32_768, 32_767], 3, &mut flat()).unwrap();
        let out: Vec<i32> = ints(c.pending(), 4).iter().map(|v| v >> 16).collect();
        assert_eq!(out, [123, 123, -32_768, -32_768, 32_767, 32_767]);
    }

    #[test]
    fn a_float_device_gets_integers_widened_exactly() {
        let mut c = converter(source(96_000, 2, 24, false), SAMPLE_F32_LE, 96_000, false);
        assert_eq!(c.mode, Mode::Widen);
        let samples = [8_388_607, -8_388_608, 1, -1];
        c.convert(&samples, 2, &mut flat()).unwrap();
        let back: Vec<i32> = floats(c.pending())
            .iter()
            .map(|&f| (f64::from(f) * 8_388_608.0) as i32)
            .collect();
        assert_eq!(back, samples);
    }

    // A float source on `mask`'s speakers, one frame of `frame`, downmixed to a float stereo device: its L and R.
    fn downmixed(mask: u32, frame: &[f32]) -> [f32; 2] {
        let mut info = source(48_000, frame.len() as u32, 32, true);
        info.channel_mask = mask;
        let mut c = converter(info, SAMPLE_F32_LE, 48_000, true);
        let words: Vec<i32> = frame.iter().map(|v| v.to_bits() as i32).collect();
        c.convert(&words, 1, &mut flat()).unwrap();
        let out = floats(c.pending());
        [out[0], out[1]]
    }

    // Each channel goes to the stereo device by the speaker it is for, not by its place in a default layout: an
    // LFE is never mixed in, a centre goes to both sides at -3 dB and a back centre at -6 dB.
    #[test]
    fn a_downmix_follows_each_channels_speaker() {
        let a = std::f32::consts::FRAC_1_SQRT_2;
        let close = |got: [f32; 2], want: [f32; 2]| {
            assert!(
                (got[0] - want[0]).abs() < 1e-6 && (got[1] - want[1]).abs() < 1e-6,
                "{got:?} for {want:?}"
            );
        };
        const FL: u32 = 0x1;
        const FR: u32 = 0x2;
        const FC: u32 = 0x4;
        const LFE: u32 = 0x8;
        const BL: u32 = 0x10;
        const BR: u32 = 0x20;
        const BC: u32 = 0x100;
        // Stereo and LFE.
        close(downmixed(FL | FR | LFE, &[0.5, 0.25, 0.8]), [0.5, 0.25]);
        // Mono and LFE: the programme on both sides.
        close(downmixed(FC | LFE, &[0.5, 0.8]), [0.5, 0.5]);
        // 3.1.
        close(
            downmixed(FL | FR | FC | LFE, &[0.5, 0.25, 0.2, 0.8]),
            [0.5 + a * 0.2, 0.25 + a * 0.2],
        );
        // L R S and C L R S, in the order L R C S.
        close(downmixed(FL | FR | BC, &[0.5, 0.25, 0.1]), [0.55, 0.3]);
        close(
            downmixed(FL | FR | FC | BC, &[0.5, 0.25, 0.2, 0.1]),
            [0.5 + a * 0.2 + 0.05, 0.25 + a * 0.2 + 0.05],
        );
        // 4.1: L R LFE Ls Rs.
        close(
            downmixed(FL | FR | LFE | BL | BR, &[0.5, 0.25, 0.8, 0.1, 0.05]),
            [0.5 + a * 0.1, 0.25 + a * 0.05],
        );
    }

    // What is not a number never reaches a float device, as the integer path already turns it into silence.
    #[test]
    fn a_float_device_gets_no_nan_or_infinity() {
        let mut c = converter(source(48_000, 2, 32, true), SAMPLE_F32_LE, 48_000, true);
        let samples = [f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 0.5].map(|v| v.to_bits() as i32);
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(crate::dsp::Settings {
            order: crate::dsp::DEFAULT_ORDER,
            volume_db: -6.0,
            ..Default::default()
        });
        c.convert(&samples, 2, &mut dsp).unwrap();
        let out = floats(c.pending());
        assert_eq!(out.len(), 4);
        assert!(out.iter().all(|v| v.is_finite()), "{out:?}");
        assert_eq!(out[0], 0.0);
    }

    #[test]
    fn conversion_needs_permission_except_for_a_lossy_decode() {
        let refused = |info: CamelSourceInfoV1, out: u32, rate: u32| {
            Converter::new(&info, &device(out), rate, false, false, QUALITY_HIGH).err()
        };
        let unsupported = Some(ENGINE_E_UNSUPPORTED_FORMAT);
        assert_eq!(
            refused(source(44_100, 2, 24, false), SAMPLE_S24_LE, 48_000),
            unsupported
        );
        assert_eq!(
            refused(source(48_000, 6, 24, false), SAMPLE_S24_LE, 48_000),
            unsupported
        );
        assert_eq!(
            refused(source(48_000, 2, 24, false), SAMPLE_S16_LE, 48_000),
            unsupported
        );
        assert_eq!(
            refused(source(48_000, 2, 24, true), SAMPLE_S16_LE, 48_000),
            unsupported
        );
        assert_eq!(
            refused(source(48_000, 2, 24, true), SAMPLE_S24_LE, 48_000),
            None
        );
        assert_eq!(
            refused(source(48_000, 1, 16, true), SAMPLE_F32_LE, 48_000),
            None
        );
        assert!(Converter::new(
            &source(48_000, 9, 24, false),
            &device(SAMPLE_S24_LE),
            48_000,
            true,
            false,
            QUALITY_HIGH
        )
        .is_err());
    }

    #[test]
    fn surround_downmixes_with_bs775_coefficients() {
        // 5.1 in WAVE order L R C LFE Ls Rs, one impulse per channel, through a float device (no dither).
        let mut c = converter(source(48_000, 6, 24, false), SAMPLE_F32_LE, 48_000, true);
        assert_eq!(c.mode, Mode::Process);
        let mut samples = vec![0; 36];
        for channel in 0..6 {
            samples[channel * 6 + channel] = 1 << 22; // half full scale
        }
        c.convert(&samples, 6, &mut flat()).unwrap();
        let out = floats(c.pending());
        let a = std::f32::consts::FRAC_1_SQRT_2 * 0.5;
        let expected = [
            [0.5, 0.0],
            [0.0, 0.5],
            [a, a],
            [0.0, 0.0],
            [a, 0.0],
            [0.0, a],
        ];
        for (frame, [left, right]) in expected.iter().enumerate() {
            assert!(
                (out[frame * 2] - left).abs() < 1e-6,
                "channel {frame} to left"
            );
            assert!(
                (out[frame * 2 + 1] - right).abs() < 1e-6,
                "channel {frame} to right"
            );
        }
    }

    #[test]
    fn bit_reduction_dithers_once_and_overs_clip() {
        let mut c = converter(source(48_000, 2, 24, false), SAMPLE_S16_LE, 48_000, true);
        let level = 1000.3; // in 16-bit LSB
        let sample = (level * 256.0) as i32;
        let frames = 20_000;
        c.convert(&vec![sample; frames * 2], frames, &mut flat())
            .unwrap();
        let out = ints(c.pending(), 2);
        let mean = out.iter().map(|&v| f64::from(v)).sum::<f64>() / out.len() as f64;
        assert!(
            (mean - f64::from(sample) / 256.0).abs() < 0.05,
            "TPDF keeps the mean: {mean}"
        );
        assert!(
            out.iter().all(|&v| (f64::from(v) - level).abs() <= 1.5),
            "error stays within the dither"
        );
        assert!(out.iter().any(|&v| v != out[0]), "dither is applied");

        // L and C at 0.9 full scale sum beyond full scale; the result clips instead of wrapping.
        let mut c = converter(source(48_000, 3, 24, false), SAMPLE_S24_LE, 48_000, true);
        let high = (0.9 * 8_388_608.0) as i32;
        c.convert(&[high, 0, high, -high, 0, -high], 2, &mut flat())
            .unwrap();
        let out = ints(c.pending(), 3);
        assert_eq!(out[0], 8_388_607);
        assert_eq!(out[2], -8_388_608);
    }

    #[test]
    fn resampled_sine_matches_the_ideal_one() {
        let (input_rate, output_rate) = (44_100u32, 48_000u32);
        let tone = |rate: u32, i: usize| {
            0.5 * (2.0 * std::f64::consts::PI * 1000.0 * i as f64 / f64::from(rate)).sin()
        };
        let mut c = converter(
            source(input_rate, 2, 24, false),
            SAMPLE_F32_LE,
            output_rate,
            true,
        );
        let samples: Vec<i32> = (0..44_100)
            .flat_map(|i| {
                let s = (tone(input_rate, i) * 8_388_608.0).round() as i32;
                [s, s]
            })
            .collect();
        for chunk in samples.chunks(2 * 4096) {
            c.convert(chunk, chunk.len() / 2, &mut flat()).unwrap();
        }
        c.finish(&mut flat()).unwrap();
        let out = floats(c.pending());
        assert_eq!(out.len() / 2, 48_000, "length follows the rate ratio");
        let (mut signal, mut noise) = (0.0, 0.0);
        for i in 2_000..46_000 {
            let ideal = tone(output_rate, i);
            signal += ideal * ideal;
            noise += (f64::from(out[i * 2]) - ideal).powi(2);
        }
        let snr = 10.0 * (signal / noise).log10();
        assert!(snr > 100.0, "no delay and a clean resample: {snr:.1} dB");
    }

    // A second-order sigma-delta modulator (NTF (1 - z^-1)^2, STF a pure delay): a sine at `amplitude` of full DSD
    // modulation as `bytes` MSB-first byte frames, mono.
    fn dsd_sine(dsd_rate: u32, frequency: f64, amplitude: f64, bytes: usize) -> Vec<i32> {
        let (mut u1, mut u2, mut y) = (0.0f64, 0.0f64, 1.0f64);
        (0..bytes)
            .map(|n| {
                let mut byte = 0u8;
                for bit in 0..8 {
                    let t = (n * 8 + bit) as f64 / f64::from(dsd_rate);
                    let x = amplitude * (2.0 * std::f64::consts::PI * frequency * t).sin();
                    u1 += x - y;
                    u2 += u1 - 2.0 * y;
                    y = if u2 >= 0.0 { 1.0 } else { -1.0 };
                    if y > 0.0 {
                        byte |= 0x80 >> bit;
                    }
                }
                i32::from(byte)
            })
            .collect()
    }

    fn dsd_source(byte_rate: u32) -> CamelSourceInfoV1 {
        let mut info = source(byte_rate, 1, 1, false);
        info.sample_encoding = crate::SAMPLE_ENCODING_DSD;
        info
    }

    // The left channel of `bytes` (mono DSD) converted to float PCM at `rate`.
    fn dsd_to_pcm(bytes: &[i32], byte_rate: u32, rate: u32) -> Vec<f64> {
        let mut c = Converter::new(
            &dsd_source(byte_rate),
            &device(SAMPLE_F32_LE),
            rate,
            false,
            true,
            QUALITY_HIGH,
        )
        .unwrap();
        assert_eq!(c.mode, Mode::DsdPcm);
        for chunk in bytes.chunks(4096) {
            c.convert(chunk, chunk.len(), &mut flat()).unwrap();
        }
        c.finish(&mut flat()).unwrap();
        floats(c.pending())
            .as_chunks::<2>()
            .0
            .iter()
            .map(|f| f64::from(f[0]))
            .collect()
    }

    // |X[k]|^2 of a power-of-two length block.
    pub(crate) fn power_spectrum(x: &[f64]) -> Vec<f64> {
        let n = x.len();
        let bits = n.trailing_zeros();
        let mut re: Vec<f64> = (0..n)
            .map(|i| x[i.reverse_bits() >> (usize::BITS - bits)])
            .collect();
        let mut im = vec![0.0; n];
        let mut size = 2;
        while size <= n {
            let step = -2.0 * std::f64::consts::PI / size as f64;
            for start in (0..n).step_by(size) {
                for k in 0..size / 2 {
                    let (s, c) = (step * k as f64).sin_cos();
                    let (a, b) = (start + k, start + k + size / 2);
                    let (tr, ti) = (re[b] * c - im[b] * s, re[b] * s + im[b] * c);
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
            size *= 2;
        }
        re.iter().zip(&im).map(|(r, i)| r * r + i * i).collect()
    }

    #[test]
    fn dsd_to_pcm_needs_permission_and_maps_full_modulation_to_full_scale() {
        let refused = Converter::new(
            &dsd_source(352_800),
            &device(SAMPLE_S24_LE),
            176_400,
            true,
            false,
            QUALITY_HIGH,
        );
        assert_eq!(refused.err(), Some(ENGINE_E_UNSUPPORTED_FORMAT));
        let mut surround = dsd_source(352_800);
        surround.channel_count = 6;
        let no_downmix = Converter::new(
            &surround,
            &device(SAMPLE_S24_LE),
            352_800,
            false,
            true,
            QUALITY_HIGH,
        );
        assert_eq!(no_downmix.err(), Some(ENGINE_E_UNSUPPORTED_FORMAT));

        let mut s32 = Converter::new(
            &dsd_source(352_800),
            &device(SAMPLE_S32_LE),
            352_800,
            false,
            true,
            QUALITY_HIGH,
        )
        .unwrap();
        s32.convert(&vec![0xFF; 2000], 2000, &mut flat()).unwrap();
        let top = ints(s32.pending(), 4)[2 * 1500] >> 8;
        assert!(
            (8_388_600..=8_388_607).contains(&top),
            "24 bits in the 32-bit container: {top}"
        );

        for byte_rate in [352_800, 1_411_200] {
            let settle = |out: Vec<f64>| out[1000..].to_vec();
            let ones = settle(dsd_to_pcm(&vec![0xFF; 20_000], byte_rate, 352_800));
            assert!(
                ones.iter().all(|v| (v - 1.0).abs() < 1e-6),
                "100% modulation is 0 dBFS"
            );
            let zeros = settle(dsd_to_pcm(&vec![0x00; 20_000], byte_rate, 352_800));
            assert!(zeros.iter().all(|v| (v + 1.0).abs() < 1e-6));
            let idle = settle(dsd_to_pcm(&vec![0x69; 20_000], byte_rate, 352_800));
            let peak = idle.iter().fold(0.0f64, |m, v| m.max(v.abs()));
            assert!(20.0 * peak.log10() < -120.0, "idle pattern: {peak:e}");
        }
    }

    // A modulated sine keeps its level across the band, and the modulator's ultrasonic noise does not alias into it:
    // the in-band SNR of the PCM equals that of the bitstream itself.
    #[test]
    fn dsd_sine_keeps_its_level_without_aliased_noise() {
        const N: usize = 1 << 15;
        for (dsd, rate) in [(1u32, 352_800u32), (2, 352_800), (1, 176_400)] {
            let n = N * rate as usize / 352_800;
            let bin = f64::from(rate) / n as f64;
            let byte_rate = dsd * 352_800;
            for k in [93usize, 1393] {
                let frequency = k as f64 * bin;
                let skip = 4096 * rate as usize / 352_800;
                let bytes = (skip + n + 64) * (byte_rate / rate) as usize;
                let dsd_bytes = dsd_sine(byte_rate * 8, frequency, 0.5, bytes);
                let pcm = dsd_to_pcm(&dsd_bytes, byte_rate, rate);
                let power = power_spectrum(&pcm[skip..skip + n]);
                let level = 20.0 * (2.0 * power[k].sqrt() / n as f64 / 0.5).log10();
                assert!(
                    level.abs() < 0.02,
                    "DSD{} to {rate} at {frequency:.0} Hz: {level:.4} dB",
                    dsd * 64
                );
                let in_band_snr = |power: &[f64]| {
                    let band = (20_000.0 / bin) as usize;
                    let noise: f64 = (2..=band)
                        .filter(|&b| b.abs_diff(k) > 1)
                        .map(|b| power[b])
                        .sum();
                    10.0 * (power[k] / noise).log10()
                };
                // The same block of the bitstream itself, at the DSD rate: the modulator's own in-band noise.
                let per_frame = (byte_rate / rate) as usize;
                let bits: Vec<f64> = dsd_bytes[skip * per_frame..(skip + n) * per_frame]
                    .iter()
                    .flat_map(|&b| {
                        (0..8).map(move |j| if b & (0x80 >> j) != 0 { 1.0 } else { -1.0 })
                    })
                    .collect();
                let reference = in_band_snr(&power_spectrum(&bits));
                let snr = in_band_snr(&power);
                assert!(
                    snr > reference - 0.5,
                    "DSD{} to {rate} at {frequency:.0} Hz: SNR {snr:.2} dB, bitstream {reference:.2} dB",
                    dsd * 64
                );
            }
        }
    }
}
