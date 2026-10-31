// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Headphone crossfeed (docs/Plan.md §5.9): Bauer stereophonic-to-binaural (bs2b), following the filters of libbs2b
//! by Boris Mikhaylov (MIT). Each ear gets its own channel through a first-order high boost and the other channel
//! through a first-order low-pass `feed_db` lower at low frequencies; a mono signal keeps its level at DC. Unlike
//! libbs2b it does not clip: the chain's headroom and the final quantization do.

use std::f64::consts::PI;

use crate::fft::Complex;

pub(crate) const MIN_CUTOFF_HZ: u32 = 300;
pub(crate) const MAX_CUTOFF_HZ: u32 = 2_000;
pub(crate) const MIN_FEED_DB: f64 = 1.0;
pub(crate) const MAX_FEED_DB: f64 = 15.0;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Params {
    pub cutoff_hz: u32,
    pub feed_db: f64,
}

impl Params {
    pub(crate) fn valid(&self) -> bool {
        (MIN_CUTOFF_HZ..=MAX_CUTOFF_HZ).contains(&self.cutoff_hz)
            && self.feed_db.is_finite()
            && (MIN_FEED_DB..=MAX_FEED_DB).contains(&self.feed_db)
    }
}

#[derive(Clone, Copy)]
struct Coefficients {
    a0_lo: f64,
    b1_lo: f64,
    a0_hi: f64,
    a1_hi: f64,
    b1_hi: f64,
    gain: f64,
}

impl Coefficients {
    fn new(params: Params, rate: f64) -> Self {
        let fc_lo = f64::from(params.cutoff_hz);
        let level = params.feed_db;
        let gb_lo = level * -5.0 / 6.0 - 3.0;
        let gb_hi = level / 6.0 - 3.0;
        let g_lo = 10f64.powf(gb_lo / 20.0);
        let g_hi = 1.0 - 10f64.powf(gb_hi / 20.0);
        let fc_hi = fc_lo * 2f64.powf((gb_lo - 20.0 * g_hi.log10()) / 12.0);
        let x_lo = (-2.0 * PI * fc_lo / rate).exp();
        let x_hi = (-2.0 * PI * fc_hi / rate).exp();
        Coefficients {
            a0_lo: g_lo * (1.0 - x_lo),
            b1_lo: x_lo,
            a0_hi: 1.0 - g_hi * (1.0 - x_hi),
            a1_hi: -x_hi,
            b1_hi: x_hi,
            gain: 1.0 / (1.0 - g_hi + g_lo),
        }
    }
}

/// The response from a channel to its own ear and to the other ear at `frequency`.
pub(crate) fn response(params: Params, rate: f64, frequency: f64) -> (Complex, Complex) {
    let c = Coefficients::new(params, rate);
    let z1 = Complex::unit(-2.0 * PI * frequency / rate);
    let one = Complex::new(1.0, 0.0);
    let direct = (Complex::new(c.a0_hi, 0.0) + z1.scale(c.a1_hi)).div(one - z1.scale(c.b1_hi));
    let cross = Complex::new(c.a0_lo, 0.0).div(one - z1.scale(c.b1_lo));
    (direct.scale(c.gain), cross.scale(c.gain))
}

pub(crate) struct Crossfeed {
    coefficients: Coefficients,
    lo: [f64; 2],
    hi: [f64; 2],
    previous: [f64; 2],
}

impl Crossfeed {
    pub(crate) fn new(params: Params, rate: f64) -> Self {
        Crossfeed {
            coefficients: Coefficients::new(params, rate),
            lo: [0.0; 2],
            hi: [0.0; 2],
            previous: [0.0; 2],
        }
    }

    pub(crate) fn reset(&mut self) {
        self.lo = [0.0; 2];
        self.hi = [0.0; 2];
        self.previous = [0.0; 2];
    }

    /// Processes the first two of `channels` interleaved channels in place; a mono stream is left alone.
    pub(crate) fn process(&mut self, samples: &mut [f64], channels: usize) {
        if channels < 2 {
            return;
        }
        let c = self.coefficients;
        for frame in samples.chunks_exact_mut(channels) {
            let x = [frame[0], frame[1]];
            let (lo, hi, previous) = (self.lo, self.hi, self.previous);
            self.lo = [0, 1].map(|i| c.a0_lo * x[i] + c.b1_lo * lo[i]);
            self.hi = [0, 1].map(|i| c.a0_hi * x[i] + c.a1_hi * previous[i] + c.b1_hi * hi[i]);
            self.previous = x;
            frame[0] = (self.hi[0] + self.lo[1]) * c.gain;
            frame[1] = (self.hi[1] + self.lo[0]) * c.gain;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{response, Crossfeed, Params};

    // libbs2b's presets, as its levels encode them: cut-off in Hz, feed in tenths of a dB.
    const PRESETS: [(u32, u32); 3] = [(700, 45), (700, 60), (650, 95)];

    fn params(cutoff_hz: u32, feed_tenths: u32) -> Params {
        Params {
            cutoff_hz,
            feed_db: f64::from(feed_tenths) / 10.0,
        }
    }

    // libbs2b's cross_feed_d, transcribed: its init() and the per-sample filters, without the final clipping.
    fn reference(cutoff_hz: u32, feed_tenths: u32, rate: f64, input: &[[f64; 2]]) -> Vec<[f64; 2]> {
        let fc_lo = f64::from(cutoff_hz);
        let level = f64::from(feed_tenths) / 10.0;
        let gb_lo = level * -5.0 / 6.0 - 3.0;
        let gb_hi = level / 6.0 - 3.0;
        let g_lo = 10f64.powf(gb_lo / 20.0);
        let g_hi = 1.0 - 10f64.powf(gb_hi / 20.0);
        let fc_hi = fc_lo * 2f64.powf((gb_lo - 20.0 * g_hi.log10()) / 12.0);
        let x = (-2.0 * std::f64::consts::PI * fc_lo / rate).exp();
        let (b1_lo, a0_lo) = (x, g_lo * (1.0 - x));
        let x = (-2.0 * std::f64::consts::PI * fc_hi / rate).exp();
        let (b1_hi, a0_hi, a1_hi) = (x, 1.0 - g_hi * (1.0 - x), -x);
        let gain = 1.0 / (1.0 - g_hi + g_lo);
        let (mut lo, mut hi, mut asis) = ([0.0; 2], [0.0; 2], [0.0; 2]);
        input
            .iter()
            .map(|s| {
                lo[0] = a0_lo * s[0] + b1_lo * lo[0];
                lo[1] = a0_lo * s[1] + b1_lo * lo[1];
                hi[0] = a0_hi * s[0] + a1_hi * asis[0] + b1_hi * hi[0];
                hi[1] = a0_hi * s[1] + a1_hi * asis[1] + b1_hi * hi[1];
                asis = *s;
                [(hi[0] + lo[1]) * gain, (hi[1] + lo[0]) * gain]
            })
            .collect()
    }

    #[test]
    fn the_presets_and_custom_values_match_libbs2b() {
        let input: Vec<[f64; 2]> = (0..2_000)
            .map(|n| {
                let t = n as f64;
                [(t * 0.031).sin() * 0.8, (t * 0.17).cos() * 0.3 - 0.2]
            })
            .collect();
        for (cutoff, feed) in PRESETS
            .into_iter()
            .chain([(300, 10), (2_000, 150), (1_234, 77)])
        {
            for rate in [44_100.0, 96_000.0, 384_000.0] {
                let want = reference(cutoff, feed, rate, &input);
                let mut node = Crossfeed::new(params(cutoff, feed), rate);
                let mut samples: Vec<f64> = input.iter().flatten().copied().collect();
                node.process(&mut samples, 2);
                for (got, want) in samples.as_chunks::<2>().0.iter().zip(&want) {
                    assert!((got[0] - want[0]).abs() < 1e-12 && (got[1] - want[1]).abs() < 1e-12);
                }
            }
        }
    }

    #[test]
    fn a_mono_signal_keeps_its_level_at_dc_and_the_feed_is_the_set_level_below() {
        for (cutoff, feed) in PRESETS {
            let p = params(cutoff, feed);
            let (direct, cross) = response(p, 48_000.0, 0.0);
            assert!(((direct + cross).norm() - 1.0).abs() < 1e-12);
            let level = 20.0 * (direct.norm() / cross.norm()).log10();
            assert!((level - p.feed_db).abs() < 1e-9);
            // Far above the cut-off the other ear hears little.
            let (direct, cross) = response(p, 48_000.0, 15_000.0);
            assert!(cross.norm() < 0.1 * direct.norm());
        }
    }

    #[test]
    fn the_response_matches_the_processed_signal() {
        let p = params(700, 45);
        let rate = 48_000.0;
        let frequency = 1_000.0;
        let mut node = Crossfeed::new(p, rate);
        let mut samples: Vec<f64> = (0..48_000)
            .flat_map(|n| {
                [
                    (2.0 * std::f64::consts::PI * frequency * n as f64 / rate).sin(),
                    0.0,
                ]
            })
            .collect();
        node.process(&mut samples, 2);
        let peak = |c: usize| {
            samples
                .as_chunks::<2>()
                .0
                .iter()
                .skip(24_000)
                .map(|f| f[c].abs())
                .fold(0.0, f64::max)
        };
        let (direct, cross) = response(p, rate, frequency);
        assert!((peak(0) - direct.norm()).abs() < 1e-3);
        assert!((peak(1) - cross.norm()).abs() < 1e-3);
    }

    #[test]
    fn values_outside_libbs2bs_ranges_are_refused() {
        assert!(params(700, 45).valid());
        assert!(!params(299, 45).valid());
        assert!(!params(2_001, 45).valid());
        assert!(!params(700, 9).valid());
        assert!(!params(700, 151).valid());
    }
}
