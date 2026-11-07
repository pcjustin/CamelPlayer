// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! DSD to a lower DSD rate (docs/Plan.md §5.8): decimated to 352.8 kHz by the DSD to PCM stages, interpolated to the
//! new DSD rate and modulated again to one bit.
//!
//! Interpolation: 2x to 705.6 kHz through a lowpass that also removes what the decimator leaves above 80 kHz of the
//! source's own shaped noise, 2x to 1411.2 kHz, then linear steps to the DSD rate. The modulator is an error-feedback
//! loop that passes the signal unchanged and shapes the noise by a 7th-order NTF: zeros optimized over the band up to
//! rate / 128 (22.05 kHz at DSD64), Butterworth highpass poles for an out-of-band gain of 1.5, run as second-order
//! sections. The error fed back is limited, so the loop's state stays bounded at any input, full modulation included;
//! a loop held at that limit is reset.

use std::f64::consts::PI;

use crate::dsd2pcm::{bessel_i0, dot, DsdDecimator, Ring, PASSBAND_HZ, PCM_RATE};
use crate::fft::Complex;

pub(crate) const ORDER: usize = 7;
pub(crate) const OSR: f64 = 64.0;
pub(crate) const MAX_GAIN: f64 = 1.3;
// The largest input the loop modulates cleanly; beyond it the input is clipped.
const INPUT_LIMIT: f64 = 0.7;
// The loop input beyond which the fed-back error is limited.
const ERROR_LIMIT: f64 = 2.0;
// Samples in a row at the limit after which the loop starts again from rest.
const RESET_AFTER: u32 = 64;
// The interpolation keeps PASSBAND_HZ flat and removes everything from STOP_HZ by STOP_DB.
const STOP_HZ: f64 = 80_000.0;
const STOP_DB: f64 = 100.0;

#[derive(Clone, Copy, Debug)]
pub(crate) struct Section {
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
}

// Roots of the Legendre polynomial of degree `n` above zero: the optimal NTF zeros relative to the band edge.
fn legendre_roots(n: usize) -> Vec<f64> {
    let value = |x: f64| {
        let (mut p0, mut p1) = (1.0, x);
        for k in 1..n {
            let k = k as f64;
            (p0, p1) = (p1, ((2.0 * k + 1.0) * x * p1 - k * p0) / (k + 1.0));
        }
        (p1, n as f64 * (x * p1 - p0) / (x * x - 1.0))
    };
    (1..=n / 2)
        .map(|k| {
            let mut x = (PI * (k as f64 - 0.25) / (n as f64 + 0.5)).cos();
            for _ in 0..64 {
                let (p, dp) = value(x);
                x -= p / dp;
            }
            x
        })
        .collect()
}

fn sections_for(zero_angles: &[f64], odd: bool, order: usize, cutoff: f64) -> Vec<Section> {
    let warped = 2.0 * (cutoff / 2.0).tan();
    let poles: Vec<Complex> = (0..order)
        .map(|k| {
            let prototype = Complex::unit(PI * (2 * k + order + 1) as f64 / (2 * order) as f64);
            let s = Complex::new(warped, 0.0).div(prototype);
            (Complex::new(2.0, 0.0) + s).div(Complex::new(2.0, 0.0) - s)
        })
        .collect();
    let mut sections: Vec<Section> = zero_angles
        .iter()
        .zip(poles.iter().filter(|p| p.im > 1e-12))
        .map(|(&angle, p)| Section {
            b1: -2.0 * angle.cos(),
            b2: 1.0,
            a1: -2.0 * p.re,
            a2: p.re * p.re + p.im * p.im,
        })
        .collect();
    if odd {
        let real = poles
            .iter()
            .find(|p| p.im.abs() <= 1e-12)
            .expect("an odd order has a real pole");
        sections.push(Section {
            b1: -1.0,
            b2: 0.0,
            a1: -real.re,
            a2: 0.0,
        });
    }
    sections
}

// |NTF| at `w` radians per sample.
pub(crate) fn ntf_gain(sections: &[Section], w: f64) -> f64 {
    let z = Complex::unit(w);
    let z2 = z * z;
    sections
        .iter()
        .map(|s| {
            let num = z2 + z.scale(s.b1) + Complex::new(s.b2, 0.0);
            let den = z2 + z.scale(s.a1) + Complex::new(s.a2, 0.0);
            num.norm() / den.norm()
        })
        .product()
}

fn peak_gain(sections: &[Section]) -> f64 {
    (1..=1024)
        .map(|i| ntf_gain(sections, PI * f64::from(i) / 1024.0))
        .fold(0.0, f64::max)
}

/// The NTF of `order` with its zeros over the band up to pi / `osr` and its out-of-band gain `max_gain`.
pub(crate) fn design(order: usize, osr: f64, max_gain: f64) -> Vec<Section> {
    let angles: Vec<f64> = legendre_roots(order).iter().map(|r| r * PI / osr).collect();
    let odd = order % 2 == 1;
    let (mut low, mut high) = (1e-6, PI * 0.99);
    for _ in 0..100 {
        let mid = 0.5 * (low + high);
        if peak_gain(&sections_for(&angles, odd, order, mid)) > max_gain {
            high = mid;
        } else {
            low = mid;
        }
    }
    sections_for(&angles, odd, order, low)
}

pub(crate) const SECTIONS: usize = ORDER.div_ceil(2);

/// The shipped NTF.
pub(crate) fn shipped() -> [Section; SECTIONS] {
    design(ORDER, OSR, MAX_GAIN)
        .try_into()
        .expect("ORDER makes SECTIONS sections")
}

/// Two channels' 1-bit modulators, stepped together so each operation serves both: the output bit for each input in
/// [-1, 1], true for +1.
///
/// The loop input is the input plus the sum of the sections' first states. Every new state is a multiple of the error
/// plus a part known before the error is, so that sum for the next step is one multiply-add away from the error and
/// only the quantizer and that multiply-add lie between one bit and the next.
pub(crate) struct Modulator {
    // Per section, per state, per channel.
    state: [[[f64; 2]; 2]; SECTIONS],
    sum: [f64; 2],
    limited: [u32; 2],
    pub(crate) resets: u64,
}

impl Modulator {
    pub(crate) fn new() -> Self {
        Self {
            state: [[[0.0; 2]; 2]; SECTIONS],
            sum: [0.0; 2],
            limited: [0; 2],
            resets: 0,
        }
    }

    #[inline(always)]
    pub(crate) fn step(&mut self, sections: &[Section; SECTIONS], input: [f64; 2]) -> [bool; 2] {
        let mut v = [0.0; 2];
        let mut e = [0.0; 2];
        for c in 0..2 {
            v[c] = input[c].clamp(-INPUT_LIMIT, INPUT_LIMIT) + self.sum[c];
            let y = if v[c] >= 0.0 { 1.0 } else { -1.0 };
            e[c] = y - v[c].clamp(-ERROR_LIMIT, ERROR_LIMIT);
        }
        let (mut before, mut gain, mut rest) = ([0.0; 2], 0.0, [0.0; 2]);
        for (s, k) in self.state.iter_mut().zip(sections) {
            gain += k.b1 - k.a1;
            for c in 0..2 {
                let after = before[c] + s[0][c];
                let first = k.b1 * before[c] - k.a1 * after + s[1][c];
                let second = k.b2 * before[c] - k.a2 * after;
                rest[c] += first;
                s[0][c] = (k.b1 - k.a1) * e[c] + first;
                s[1][c] = (k.b2 - k.a2) * e[c] + second;
                before[c] = after;
            }
        }
        for c in 0..2 {
            self.sum[c] = gain * e[c] + rest[c];
        }
        if v[0].abs() > ERROR_LIMIT || v[1].abs() > ERROR_LIMIT {
            self.limit(v);
        } else {
            self.limited = [0; 2];
        }
        [v[0] >= 0.0, v[1] >= 0.0]
    }

    // A channel held at the error limit for RESET_AFTER steps starts again from rest.
    #[cold]
    fn limit(&mut self, v: [f64; 2]) {
        for c in 0..2 {
            if v[c].abs() <= ERROR_LIMIT {
                self.limited[c] = 0;
                continue;
            }
            self.limited[c] += 1;
            if self.limited[c] > RESET_AFTER {
                for s in &mut self.state {
                    s[0][c] = 0.0;
                    s[1][c] = 0.0;
                }
                self.sum[c] = 0.0;
                self.limited[c] = 0;
                self.resets += 1;
            }
        }
    }
}
/// A Kaiser-windowed lowpass at `rate`: flat to `pass`, `atten` dB from `stop`, taps summing to 1.
fn kaiser_lowpass(rate: f64, pass: f64, stop: f64, atten: f64) -> Vec<f64> {
    let design = atten + 10.0;
    let taps = ((design - 8.0) / (2.285 * 2.0 * PI * (stop - pass) / rate)).ceil() as usize + 1;
    let taps = taps + taps % 2; // an even length splits into two equal phases
    let cutoff = (pass + stop) / 2.0 / rate;
    let beta = 0.1102 * (design - 8.7);
    let centre = (taps - 1) as f64 / 2.0;
    let mut h: Vec<f64> = (0..taps)
        .map(|n| {
            let t = n as f64 - centre;
            let sinc = (2.0 * PI * cutoff * t).sin() / (PI * t);
            let r = t / centre;
            sinc * bessel_i0(beta * (1.0 - r * r).max(0.0).sqrt()) / bessel_i0(beta)
        })
        .collect();
    let sum: f64 = h.iter().sum();
    h.iter_mut().for_each(|v| *v /= sum);
    h
}

// A 2x interpolator's two phases, each with the zero-stuffing gain of 2, oldest input's tap first.
struct Phases {
    even: Vec<f64>,
    odd: Vec<f64>,
}

impl Phases {
    fn new(taps: &[f64]) -> Self {
        Self {
            even: taps.iter().step_by(2).rev().map(|h| 2.0 * h).collect(),
            odd: taps
                .iter()
                .skip(1)
                .step_by(2)
                .rev()
                .map(|h| 2.0 * h)
                .collect(),
        }
    }

    // Takes `x` into `history` and returns both phases' outputs for it.
    fn interpolate(&self, history: &mut Ring<f64>, x: f64) -> [f64; 2] {
        let window = history.push(x);
        [dot(&self.even, window), dot(&self.odd, window)]
    }
}

struct Lane {
    first: Ring<f64>,
    second: Ring<f64>,
    last: f64,
}

// Raw DSD reaches the device as mono or stereo (docs/Plan.md §5.3).
const MAX_CHANNELS: usize = 2;
// Output bits per channel for each 352.8 kHz frame at DSD256.
const MAX_BITS: usize = 32;

pub(crate) struct DsdRemodulator {
    channels: usize,
    decimator: DsdDecimator,
    sections: [Section; SECTIONS],
    first: Phases,
    second: Phases,
    // Linear steps from each 1411.2 kHz sample to the next: 2, 4 or 8.
    steps: usize,
    // Output bytes per channel for each 352.8 kHz frame: 1, 2 or 4.
    bytes_per_frame: usize,
    lanes: Vec<Lane>,
    modulator: Modulator,
    pcm: Vec<f64>,
}

impl DsdRemodulator {
    /// From DSD at `in_rate` byte frames per second to `out_rate`, a lower DSD rate from DSD64 to DSD256; mono or
    /// stereo.
    pub(crate) fn new(in_rate: u32, out_rate: u32, channels: usize) -> Option<Self> {
        let bytes_per_frame = match out_rate / PCM_RATE {
            n @ (1 | 2 | 4) if out_rate.is_multiple_of(PCM_RATE) => n as usize,
            _ => return None,
        };
        if in_rate <= out_rate || !in_rate.is_multiple_of(out_rate) || channels > MAX_CHANNELS {
            return None;
        }
        let decimator = DsdDecimator::new(in_rate, channels)?;
        let rate = f64::from(PCM_RATE);
        let first = Phases::new(&kaiser_lowpass(2.0 * rate, PASSBAND_HZ, STOP_HZ, STOP_DB));
        let second = Phases::new(&kaiser_lowpass(
            4.0 * rate,
            STOP_HZ,
            2.0 * rate - STOP_HZ,
            STOP_DB,
        ));
        let lanes = (0..channels)
            .map(|_| Lane {
                first: Ring::new(first.even.len(), 0.0),
                second: Ring::new(second.even.len(), 0.0),
                last: 0.0,
            })
            .collect();
        Some(Self {
            channels,
            decimator,
            sections: shipped(),
            first,
            second,
            steps: 2 * bytes_per_frame,
            bytes_per_frame,
            lanes,
            modulator: Modulator::new(),
            pcm: Vec::new(),
        })
    }

    #[cfg(test)]
    pub(crate) fn resets(&self) -> u64 {
        self.modulator.resets
    }

    /// The working buffer for `frames` input byte frames at once, so processing needs no heap.
    pub(crate) fn reserve(&mut self, frames: usize) {
        self.pcm.reserve_exact(frames * self.channels);
    }

    /// Takes `frames` byte frames (`input` interleaved, one byte in the low bits of each value) and appends the byte
    /// frames they complete at the output rate to `output` in the same layout.
    pub(crate) fn process(&mut self, input: &[i32], frames: usize, output: &mut Vec<i32>) {
        let mut pcm = std::mem::take(&mut self.pcm);
        pcm.clear();
        self.decimator.process(input, frames, &mut pcm);
        let bits = 8 * self.bytes_per_frame;
        let step = 1.0 / self.steps as f64;
        for frame in pcm.chunks_exact(self.channels) {
            let mut inputs = [[0.0f64; MAX_BITS]; MAX_CHANNELS];
            for ((lane, &x), values) in self.lanes.iter_mut().zip(frame).zip(&mut inputs) {
                let mut i = 0;
                for a in self.first.interpolate(&mut lane.first, x) {
                    for b in self.second.interpolate(&mut lane.second, a) {
                        let delta = (b - lane.last) * step;
                        for j in 1..=self.steps {
                            values[i] = lane.last + delta * j as f64;
                            i += 1;
                        }
                        lane.last = b;
                    }
                }
            }

            let mut bytes = [[0u8; MAX_BITS / 8]; MAX_CHANNELS];
            for i in 0..bits {
                let bit = self
                    .modulator
                    .step(&self.sections, [inputs[0][i], inputs[1][i]]);
                for (out, bit) in bytes.iter_mut().zip(bit) {
                    out[i / 8] = out[i / 8] << 1 | u8::from(bit);
                }
            }
            for j in 0..self.bytes_per_frame {
                output.extend(bytes[..self.channels].iter().map(|b| i32::from(b[j])));
            }
        }
        self.pcm = pcm;
    }
}
#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// Modulates `signal(t)` (t in seconds) at `bit_rate` with the shipped modulator: MSB-first byte frames, mono.
    pub(crate) fn modulate(bit_rate: u32, bytes: usize, signal: impl Fn(f64) -> f64) -> Vec<i32> {
        let sections = shipped();
        let mut m = Modulator::new();
        (0..bytes)
            .map(|n| {
                let mut byte = 0u8;
                for bit in 0..8 {
                    let t = (n * 8 + bit) as f64 / f64::from(bit_rate);
                    let x = signal(t);
                    byte = byte << 1 | u8::from(m.step(&sections, [x, x])[0]);
                }
                i32::from(byte)
            })
            .collect()
    }

    fn remodulate(source: &[i32], from: u32, to: u32) -> (Vec<i32>, u64) {
        let mut r = DsdRemodulator::new(from, to, 1).unwrap();
        let mut out = Vec::new();
        for chunk in source.chunks(4096) {
            r.process(chunk, chunk.len(), &mut out);
        }
        (out, r.resets())
    }

    const N: usize = 1 << 16;
    const WARM: usize = 8192;

    // The tone at bin `k` of an N-point block at 352.8 kHz.
    fn bin_frequency(k: usize) -> f64 {
        k as f64 * f64::from(PCM_RATE) / N as f64
    }

    fn bytes_for(byte_rate: u32) -> usize {
        (WARM + N + 64) * (byte_rate / PCM_RATE) as usize
    }

    // `bytes` decoded to 352.8 kHz and kept below 21 kHz (160 dB from 30 kHz), so the ultrasonic noise cannot leak into
    // the band: level of bin `k` against `amplitude` in dB, SNR and THD (harmonics 2 to 5) to 20 kHz in dB.
    fn measure(bytes: &[i32], byte_rate: u32, k: usize, amplitude: f64) -> (f64, f64, f64) {
        let mut d = DsdDecimator::new(byte_rate, 1).unwrap();
        let mut pcm = Vec::new();
        d.process(bytes, bytes.len(), &mut pcm);
        let rate = f64::from(PCM_RATE);
        let h = kaiser_lowpass(rate, 21_000.0, 30_000.0, 160.0);
        let x: Vec<f64> = (WARM..WARM + N)
            .map(|n| h.iter().enumerate().map(|(j, c)| c * pcm[n - j]).sum())
            .collect();
        let p = crate::convert::tests::power_spectrum(&x);
        let top = (20_000.0 / (rate / N as f64)) as usize;
        let signal = p[k];
        let distortion: f64 = (2..=5)
            .map(|i| i * k)
            .filter(|&b| b <= top)
            .map(|b| p[b])
            .sum();
        let noise: f64 = (2..=top).filter(|&b| b.abs_diff(k) > 1).map(|b| p[b]).sum();
        (
            20.0 * (2.0 * signal.sqrt() / N as f64 / amplitude).log10(),
            10.0 * (signal / noise).log10(),
            10.0 * (distortion / signal).log10(),
        )
    }

    #[test]
    fn the_ntf_has_its_zeros_in_band_and_its_gain_out_of_band() {
        let sections = shipped();
        assert!((peak_gain(&sections) - MAX_GAIN).abs() < 1e-3);
        let in_band = (1..=100)
            .map(|i| ntf_gain(&sections, PI / OSR * f64::from(i) / 100.0))
            .fold(0.0, f64::max);
        assert!(20.0 * in_band.log10() < -75.0, "{in_band:e}");
        assert!((ntf_gain(&sections, PI) - MAX_GAIN).abs() < 1e-2);
    }

    #[test]
    fn rates_must_step_down_within_dsd64_to_dsd256() {
        let dsd = |n: u32| n * PCM_RATE;
        assert!(DsdRemodulator::new(dsd(2), dsd(1), 2).is_some());
        assert!(DsdRemodulator::new(dsd(8), dsd(4), 1).is_some());
        assert!(DsdRemodulator::new(dsd(1), dsd(1), 2).is_none());
        assert!(DsdRemodulator::new(dsd(1), dsd(2), 2).is_none());
        assert!(DsdRemodulator::new(dsd(16), dsd(8), 2).is_none());
        assert!(DsdRemodulator::new(dsd(4), 44_100, 2).is_none());
        assert!(DsdRemodulator::new(dsd(4), dsd(2), 3).is_none());
    }

    // Full modulation is clipped to INPUT_LIMIT without the loop running away, and silence afterwards is silence.
    #[test]
    fn the_loop_stays_bounded_and_recovers_after_full_modulation() {
        let sections = shipped();
        let mut m = Modulator::new();
        let rate = 2_822_400.0;
        let (mut ones, mut low_ones) = (0usize, 0usize);
        for n in 0..400_000 {
            let t = f64::from(n) / rate;
            let x = match n {
                0..100_000 => (2.0 * PI * 1000.0 * t).sin().signum(),
                100_000..200_000 => (2.0 * PI * 1000.0 * t).sin(),
                _ => 1.0,
            };
            let bit = m.step(&sections, [x, -x]);
            if n >= 200_000 {
                ones += usize::from(bit[0]);
                low_ones += usize::from(bit[1]);
            }
            assert!(m.state.iter().flatten().flatten().all(|s| s.abs() < 1e3));
        }
        let mean = 2.0 * ones as f64 / 200_000.0 - 1.0;
        assert!(
            (mean - INPUT_LIMIT).abs() < 1e-3,
            "full modulation becomes {mean}"
        );
        let low = 2.0 * low_ones as f64 / 200_000.0 - 1.0;
        assert!(
            (low + INPUT_LIMIT).abs() < 1e-3,
            "and negative full modulation {low}"
        );
        assert_eq!(m.resets, 0);
        let mut sum = 0.0f64;
        for n in 0..100_000 {
            let bit = m.step(&sections, [0.0; 2])[0];
            if n >= 50_000 {
                sum += if bit { 1.0 } else { -1.0 };
            }
        }
        assert!((sum / 50_000.0).abs() < 1e-3, "back to silence: {sum}");
    }

    // A sine at the SACD reference level, remodulated and decoded back: its level within 0.01 dB, no measurable
    // distortion, and in-band noise at least 105 dB below it.
    #[test]
    fn remodulated_dsd_decodes_back_to_its_level_without_noise() {
        for (from, to) in [(2u32, 1u32), (4, 1), (4, 2)] {
            for k in [186usize, 1858] {
                let source = modulate(8 * from * PCM_RATE, bytes_for(from * PCM_RATE), |t| {
                    0.5 * (2.0 * PI * bin_frequency(k) * t).sin()
                });
                let (out, resets) = remodulate(&source, from * PCM_RATE, to * PCM_RATE);
                assert_eq!(out.len() * from as usize, source.len() * to as usize);
                let (level, snr, thd) = measure(&out, to * PCM_RATE, k, 0.5);
                let path = format!(
                    "DSD{} to DSD{} at {:.0} Hz",
                    from * 64,
                    to * 64,
                    bin_frequency(k)
                );
                assert!(level.abs() < 0.01, "{path}: level {level:.4} dB");
                assert!(snr > 105.0, "{path}: SNR {snr:.1} dB");
                assert!(thd < -120.0, "{path}: THD {thd:.1} dB");
                assert_eq!(resets, 0, "{path}");
            }
        }
    }

    // The measurements recorded in docs/reports/DSD-remodulator-2026-11-08.md:
    // cargo test --release -p camel_audio_engine measure_remodulation -- --ignored --nocapture
    #[test]
    #[ignore]
    fn measure_remodulation() {
        let sections = shipped();
        println!(
            "NTF order {ORDER}, OSR {OSR}, out-of-band gain {MAX_GAIN}, input limit {INPUT_LIMIT}"
        );
        for s in &sections {
            println!("  {s:?}");
        }
        println!("NTF gain at DSD64, dB:");
        for f in [
            1e3, 10e3, 20e3, 22.05e3, 30e3, 40e3, 50e3, 100e3, 200e3, 1411.2e3,
        ] {
            let gain = 20.0 * ntf_gain(&sections, 2.0 * PI * f / 2_822_400.0).log10();
            println!("  {:>8.2} kHz {gain:>8.1}", f / 1e3);
        }

        println!("Modulator alone, 1001 Hz, SNR and THD to 20 kHz in dB:");
        println!("  amplitude   DSD64 SNR   THD   DSD128 SNR   THD");
        for amplitude in [0.001, 0.01, 0.1, 0.3, 0.5, 0.6, 0.7, 0.75, 0.8, 1.0] {
            print!("  {amplitude:>9}");
            for dsd in [1u32, 2] {
                let rate = dsd * PCM_RATE;
                let bytes = modulate(8 * rate, bytes_for(rate), |t| {
                    amplitude * (2.0 * PI * bin_frequency(186) * t).sin()
                });
                let (_, snr, thd) = measure(&bytes, rate, 186, amplitude);
                print!(" {snr:>11.1} {thd:>6.1}");
            }
            println!();
        }

        println!(
            "Noise of the silent DSD64 bitstream by band (Hann window), dB re full scale per Hz:"
        );
        const BITS: usize = 1 << 20;
        let mut m = Modulator::new();
        let bits: Vec<f64> = (0..BITS)
            .map(|n| {
                let w = 0.5 - 0.5 * (2.0 * PI * n as f64 / BITS as f64).cos();
                w * if m.step(&sections, [0.0; 2])[0] {
                    1.0
                } else {
                    -1.0
                }
            })
            .collect();
        let p = crate::convert::tests::power_spectrum(&bits);
        let bin = 2_822_400.0 / BITS as f64;
        for (lo, hi) in [
            (20e3, 30e3),
            (30e3, 50e3),
            (50e3, 100e3),
            (100e3, 200e3),
            (200e3, 500e3),
        ] {
            let (a, b) = ((lo / bin) as usize, (hi / bin) as usize);
            // A Hann window keeps 3/8 of the power.
            let density =
                p[a..b].iter().sum::<f64>() * 2.0 / (BITS as f64).powi(2) / 0.375 / (hi - lo);
            println!(
                "  {:>4.0} - {:>4.0} kHz {:>8.1}",
                lo / 1e3,
                hi / 1e3,
                10.0 * density.log10()
            );
        }

        println!("Remodulation decoded back, amplitude 0.5 (SACD 0 dB), dB:");
        println!("  path             Hz     level   SNR     THD   source SNR  resets");
        for (from, to) in [(2u32, 1u32), (4, 1), (4, 2), (8, 1), (8, 2), (8, 4)] {
            for k in [186usize, 1858] {
                let source = modulate(8 * from * PCM_RATE, bytes_for(from * PCM_RATE), |t| {
                    0.5 * (2.0 * PI * bin_frequency(k) * t).sin()
                });
                let (out, resets) = remodulate(&source, from * PCM_RATE, to * PCM_RATE);
                let (level, snr, thd) = measure(&out, to * PCM_RATE, k, 0.5);
                let (_, source_snr, _) = measure(&source, from * PCM_RATE, k, 0.5);
                println!(
                    "  DSD{:<3} > DSD{:<3} {:>6.0} {level:>8.4} {snr:>6.1} {thd:>7.1} {source_snr:>8.1} {resets:>6}",
                    from * 64,
                    to * 64,
                    bin_frequency(k)
                );
            }
        }

        println!("Speed, stereo, 10 s of three tones, this machine:");
        for (from, to) in [(2u32, 1u32), (4, 1), (4, 2), (8, 4)] {
            let in_rate = from * PCM_RATE;
            let seconds = 10;
            let mono = modulate(in_rate * 8, in_rate as usize * seconds, |t| {
                0.2 * (2.0 * PI * 110.0 * t).sin()
                    + 0.15 * (2.0 * PI * 1250.0 * t).sin()
                    + 0.1 * (2.0 * PI * 7300.0 * t).sin()
            });
            let stereo: Vec<i32> = mono.iter().flat_map(|&b| [b, b]).collect();
            let mut r = DsdRemodulator::new(in_rate, to * PCM_RATE, 2).unwrap();
            let mut out = Vec::with_capacity(stereo.len());
            let start = std::time::Instant::now();
            for chunk in stereo.chunks(8192) {
                r.process(chunk, chunk.len() / 2, &mut out);
            }
            let took = start.elapsed().as_secs_f64();
            println!(
                "  DSD{:<3} > DSD{:<3} {took:>6.2} s: {:>5.1}x real time",
                from * 64,
                to * 64,
                seconds as f64 / took
            );
        }
    }
}
