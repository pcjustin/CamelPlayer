//! Converts one decoded source to the device format (docs/Plan.md §5.2, §5.3).
//!
//! Integer sources the device takes as they are only repack (zero padding) and never touch float. Mono is
//! duplicated to both channels bit for bit. A float-only device gets integer samples widened exactly (at most 24
//! bits). Everything else — resampling, ITU-R BS.775 downmix, bit reduction and the decoded output of lossy sources —
//! is processed in 64-bit float and quantized once at the end with TPDF dither. Raw DSD reaches a native DSD or DoP
//! stream bit for bit (mono duplicated). DSD becomes PCM only with explicit permission: it is decimated to
//! 352.8 kHz, mixed and resampled in float like any processed source, and quantized once.

use crate::decoder_host::{CamelSourceInfoV1, SAMPLE_ENCODING_DSD, SAMPLE_ENCODING_FLOAT32};
use crate::dsd::{put_dop_sample, DOP_MARKERS, DSD_SILENCE};
use crate::dsd2pcm::{DsdDecimator, PCM_RATE};
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
    in_rate: u32,
    out_rate: u32,
}

/// Stereo downmix coefficients per source channel count, in WAVE/FLAC default order (ITU-R BS.775: centre and
/// surrounds at -3 dB, LFE dropped). 7: L R C LFE Cs Ls Rs; 8: L R C LFE Lb Rb Ls Rs.
fn downmix_matrix(channels: usize) -> Option<[Vec<f64>; 2]> {
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
    /// depth without it. `allow_dsd` permits DSD to PCM; downmixing DSD also needs `allow`.
    pub(crate) fn new(
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
                matrix: Some(downmix_matrix(in_channels).ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?),
                resampler,
                dither: Tpdf(0x9E37_79B9_7F4A_7C15),
                pending: Vec::new(),
                pending_frames: 0,
                finished: false,
                bytes_per_frame: crate::packing::bytes_per_frame(out)
                    .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?,
                carry: Vec::new(),
                decimator: Some(decimator),
                in_rate: info.sample_rate,
                out_rate,
            });
        }
        if in_dsd || out.stream_kind != AUDIO_PCM {
            // DSD to PCM is a separate, explicitly enabled path (docs/Plan.md §5.8).
            let rate_matches = match out.stream_kind {
                AUDIO_DSD => u64::from(info.sample_rate) * 8 == u64::from(out_rate),
                AUDIO_DOP => u64::from(info.sample_rate) == 2 * u64::from(out_rate),
                _ => false,
            };
            let channels_kept =
                in_channels == out_channels || (in_channels == 1 && out_channels == 2);
            if !in_dsd || !rate_matches || !channels_kept {
                return Err(ENGINE_E_UNSUPPORTED_FORMAT);
            }
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
                in_rate: info.sample_rate,
                out_rate,
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
            Some(downmix_matrix(in_channels).ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?)
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
            matrix,
            resampler,
            dither: Tpdf(0x9E37_79B9_7F4A_7C15),
            pending: Vec::new(),
            pending_frames: 0,
            finished: false,
            bytes_per_frame,
            carry: Vec::new(),
            decimator: None,
            in_rate: info.sample_rate,
            out_rate,
        })
    }

    /// Source frames to decode for about `output_frames` of output; any excess waits in the pending output.
    pub(crate) fn input_frames_for(&self, output_frames: u64) -> u64 {
        if self.mode == Mode::DsdPcm {
            let frames =
                u128::from(output_frames) * u128::from(self.in_rate) / u128::from(self.out_rate);
            return (frames as u64).max(1);
        }
        match &self.resampler {
            Some(r) => ((output_frames as f64 / r.ratio()).floor() as u64).max(1),
            None if self.mode == Mode::Dop => output_frames * 2,
            None => output_frames,
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
    /// float source) and appends the device bytes to the pending output.
    pub(crate) fn convert(&mut self, samples: &[i32], frames: usize) -> Result<(), i32> {
        let input = &samples[..frames * self.in_channels];
        match self.mode {
            Mode::Bypass => self.pack(input, self.in_bits),
            Mode::Duplicate => {
                let stereo: Vec<i32> = input.iter().flat_map(|&s| [s, s]).collect();
                self.pack(&stereo, self.in_bits)
            }
            Mode::Widen => {
                let scale = 1.0 / f64::from(1u32 << (self.in_bits - 1));
                let mut floats = Vec::with_capacity(frames * self.out_channels);
                for frame in input.chunks_exact(self.in_channels) {
                    for c in 0..self.out_channels {
                        floats.push(f64::from(frame[c.min(self.in_channels - 1)]) * scale);
                    }
                }
                self.emit(&floats)
            }
            Mode::Dsd => {
                for frame in input.chunks_exact(self.in_channels) {
                    for c in 0..self.out_channels {
                        self.pending.push(frame[c.min(self.in_channels - 1)] as u8);
                    }
                }
                self.pending_frames += frames as u64;
                Ok(())
            }
            Mode::Dop => {
                for frame in input.chunks_exact(self.in_channels) {
                    let bytes =
                        (0..self.out_channels).map(|c| frame[c.min(self.in_channels - 1)] as u8);
                    if self.carry.is_empty() {
                        self.carry.extend(bytes);
                    } else {
                        let second: Vec<u8> = bytes.collect();
                        self.push_dop(&second);
                    }
                }
                Ok(())
            }
            Mode::DsdPcm => {
                let mut decimated = Vec::new();
                self.decimator
                    .as_mut()
                    .expect("DSD to PCM has a decimator")
                    .process(input, frames, &mut decimated);
                let mixed = self.mix_floats(&decimated);
                match self.resampler.as_mut() {
                    Some(resampler) => {
                        let mut resampled = Vec::new();
                        resampler
                            .process(&mixed, &mut resampled)
                            .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT)?;
                        self.emit(&resampled)
                    }
                    None => self.emit(&mixed),
                }
            }
            Mode::Process => {
                let mixed = self.mix(input, frames);
                match self.resampler.as_mut() {
                    Some(resampler) => {
                        let mut resampled = Vec::new();
                        resampler
                            .process(&mixed, &mut resampled)
                            .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT)?;
                        self.emit(&resampled)
                    }
                    None => self.emit(&mixed),
                }
            }
        }
    }

    /// Ends the source: flushes the resampler's tail into the pending output. Further calls do nothing.
    pub(crate) fn finish(&mut self) -> Result<(), i32> {
        if self.finished {
            return Ok(());
        }
        self.finished = true;
        if self.mode == Mode::Dop && !self.carry.is_empty() {
            // An odd final byte frame is completed with idle pattern; the stream stays whole DoP frames.
            self.push_dop(&vec![DSD_SILENCE; self.out_channels]);
        }
        if let Some(resampler) = self.resampler.as_mut() {
            let mut tail = Vec::new();
            resampler
                .process(&[], &mut tail)
                .map_err(|()| ENGINE_E_UNSUPPORTED_FORMAT)?;
            return self.emit(&tail);
        }
        Ok(())
    }

    // Source samples as float in [-1, 1), mixed to stereo.
    fn mix(&self, input: &[i32], frames: usize) -> Vec<f64> {
        let scale = if self.in_float {
            1.0
        } else {
            1.0 / f64::from(1u32 << (self.in_bits - 1))
        };
        let value = |s: i32| {
            if self.in_float {
                f64::from(f32::from_bits(s as u32))
            } else {
                f64::from(s) * scale
            }
        };
        let values: Vec<f64> = input[..frames * self.in_channels]
            .iter()
            .map(|&s| value(s))
            .collect();
        self.mix_floats(&values)
    }

    // Float frames of `in_channels`, mixed to stereo.
    fn mix_floats(&self, input: &[f64]) -> Vec<f64> {
        let matrix = self
            .matrix
            .as_ref()
            .expect("processing has a channel matrix");
        let mut out = Vec::with_capacity(input.len() / self.in_channels * 2);
        for frame in input.chunks_exact(self.in_channels) {
            for row in matrix {
                out.push(frame.iter().zip(row).map(|(&s, &k)| s * k).sum());
            }
        }
        out
    }

    // The single final stage: float32 for a float device, else TPDF-dithered integers at `quantize_bits`.
    fn emit(&mut self, floats: &[f64]) -> Result<(), i32> {
        if self.out.sample_format == SAMPLE_F32_LE {
            for &v in floats {
                self.pending
                    .extend_from_slice(&(v.clamp(-1.0, 1.0) as f32).to_le_bytes());
            }
            self.pending_frames += (floats.len() / self.out_channels) as u64;
            return Ok(());
        }
        if !matches!(self.mode, Mode::Process | Mode::DsdPcm) {
            return Err(ENGINE_E_UNSUPPORTED_FORMAT);
        }
        let full = f64::from(1u32 << (self.quantize_bits - 1));
        let integers: Vec<i32> = floats
            .iter()
            .map(|&v| {
                let q = (v * full + self.dither.next()).round();
                if q.is_nan() {
                    0
                } else {
                    q.clamp(-full, full - 1.0) as i32
                }
            })
            .collect();
        self.pack(&integers, self.quantize_bits)
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
}

#[cfg(test)]
mod tests {
    use super::{Converter, Mode};
    use crate::decoder_host::{
        CamelSourceInfoV1, SAMPLE_ENCODING_FLOAT32, SAMPLE_ENCODING_INTEGER,
    };
    use crate::{
        AudioFormat, ABI_VERSION, AUDIO_PCM, ENGINE_E_UNSUPPORTED_FORMAT, QUALITY_HIGH,
        SAMPLE_F32_LE, SAMPLE_S16_LE, SAMPLE_S24_LE, SAMPLE_S32_LE,
    };

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
            reserved2: 0,
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
    fn devices_that_take_the_source_only_repack_it() {
        let mut c = converter(source(48_000, 2, 24, false), SAMPLE_S24_LE, 48_000, false);
        assert_eq!(c.mode, Mode::Bypass);
        let samples = [1, -1, 8_388_607, -8_388_608];
        c.convert(&samples, 2).unwrap();
        assert_eq!(ints(c.pending(), 3), samples);
    }

    #[test]
    fn mono_is_duplicated_bit_for_bit() {
        let mut c = converter(source(44_100, 1, 16, false), SAMPLE_S32_LE, 44_100, false);
        assert_eq!(c.mode, Mode::Duplicate);
        c.convert(&[123, -32_768, 32_767], 3).unwrap();
        let out: Vec<i32> = ints(c.pending(), 4).iter().map(|v| v >> 16).collect();
        assert_eq!(out, [123, 123, -32_768, -32_768, 32_767, 32_767]);
    }

    #[test]
    fn a_float_device_gets_integers_widened_exactly() {
        let mut c = converter(source(96_000, 2, 24, false), SAMPLE_F32_LE, 96_000, false);
        assert_eq!(c.mode, Mode::Widen);
        let samples = [8_388_607, -8_388_608, 1, -1];
        c.convert(&samples, 2).unwrap();
        let back: Vec<i32> = floats(c.pending())
            .iter()
            .map(|&f| (f64::from(f) * 8_388_608.0) as i32)
            .collect();
        assert_eq!(back, samples);
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
        c.convert(&samples, 6).unwrap();
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
        c.convert(&vec![sample; frames * 2], frames).unwrap();
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
        c.convert(&[high, 0, high, -high, 0, -high], 2).unwrap();
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
            c.convert(chunk, chunk.len() / 2).unwrap();
        }
        c.finish().unwrap();
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
            c.convert(chunk, chunk.len()).unwrap();
        }
        c.finish().unwrap();
        floats(c.pending())
            .as_chunks::<2>()
            .0
            .iter()
            .map(|f| f64::from(f[0]))
            .collect()
    }

    // |X[k]|^2 of a power-of-two length block.
    fn power_spectrum(x: &[f64]) -> Vec<f64> {
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
        s32.convert(&vec![0xFF; 2000], 2000).unwrap();
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
