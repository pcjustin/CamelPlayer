// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Parametric EQ (docs/Plan.md §5.9): biquads from the RBJ Audio EQ Cookbook in 64-bit float, any number of bands.
//!
//! A band that cannot change the signal (off, a peak or shelf at 0 dB, or a frequency at or above Nyquist) is left
//! out, and an EQ with no band left is bypassed entirely. The preamp lowers the level by the combined response's
//! largest boost, so a steady signal never exceeds full scale. A changed setting crossfades from the old filters to
//! the new ones over CROSSFADE_MS, so no change clicks.

use std::f64::consts::PI;

pub(crate) const KIND_PEAKING: u32 = 1;
pub(crate) const KIND_LOW_SHELF: u32 = 2;
pub(crate) const KIND_HIGH_SHELF: u32 = 3;
pub(crate) const KIND_LOW_PASS: u32 = 4;
pub(crate) const KIND_HIGH_PASS: u32 = 5;
pub(crate) const KIND_BAND_PASS: u32 = 6;
pub(crate) const KIND_NOTCH: u32 = 7;
pub(crate) const KIND_ALL_PASS: u32 = 8;

pub(crate) const CHANNELS_ALL: u32 = 0;
pub(crate) const CHANNELS_LEFT: u32 = 1;
pub(crate) const CHANNELS_RIGHT: u32 = 2;

const CROSSFADE_MS: f64 = 20.0;
// The response is searched for its largest boost at this many log-spaced frequencies, and at each band's own.
const RESPONSE_POINTS: usize = 512;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Band {
    pub kind: u32,
    pub channels: u32,
    /// Low- and high-pass steepness in dB per octave: 6 to 48 in steps of 6; 12 uses `q`, steeper slopes are
    /// Butterworth.
    pub slope: u32,
    pub frequency: f64,
    pub gain_db: f64,
    pub q: f64,
}

impl Band {
    fn valid(&self) -> bool {
        (KIND_PEAKING..=KIND_ALL_PASS).contains(&self.kind)
            && matches!(self.channels, CHANNELS_ALL | CHANNELS_LEFT | CHANNELS_RIGHT)
            && self.frequency.is_finite()
            && self.frequency > 0.0
            && self.gain_db.is_finite()
            && self.q.is_finite()
            && self.q > 0.0
            && (!self.is_pass() || (self.slope.is_multiple_of(6) && (6..=48).contains(&self.slope)))
    }

    fn is_pass(&self) -> bool {
        matches!(self.kind, KIND_LOW_PASS | KIND_HIGH_PASS)
    }

    // Whether the band changes the signal at `rate`.
    fn acts(&self, rate: f64) -> bool {
        let gain_only = matches!(self.kind, KIND_PEAKING | KIND_LOW_SHELF | KIND_HIGH_SHELF);
        self.frequency < rate / 2.0 && !(gain_only && self.gain_db == 0.0)
    }

    fn applies_to(&self, channel: usize) -> bool {
        match self.channels {
            CHANNELS_LEFT => channel == 0,
            CHANNELS_RIGHT => channel == 1,
            _ => true,
        }
    }

    // The band as second-order sections, `a0` normalized away: [b0, b1, b2, a1, a2] each.
    fn sections(&self, rate: f64) -> Vec<[f64; 5]> {
        let w0 = 2.0 * PI * self.frequency / rate;
        if self.is_pass() && self.slope != 12 {
            let order = (self.slope / 6) as usize;
            let mut sections = Vec::new();
            if order % 2 == 1 {
                sections.push(first_order(self.kind == KIND_LOW_PASS, w0));
            }
            for k in 1..=order / 2 {
                // Butterworth pole pairs.
                let angle = if order.is_multiple_of(2) {
                    PI * (2 * k - 1) as f64 / (2 * order) as f64
                } else {
                    PI * k as f64 / order as f64
                };
                sections.push(rbj(self.kind, w0, 0.0, 1.0 / (2.0 * angle.cos())));
            }
            return sections;
        }
        vec![rbj(self.kind, w0, self.gain_db, self.q)]
    }
}

fn normalized(b: [f64; 3], a: [f64; 3]) -> [f64; 5] {
    [
        b[0] / a[0],
        b[1] / a[0],
        b[2] / a[0],
        a[1] / a[0],
        a[2] / a[0],
    ]
}

fn rbj(kind: u32, w0: f64, gain_db: f64, q: f64) -> [f64; 5] {
    let (sin, cos) = w0.sin_cos();
    let alpha = sin / (2.0 * q);
    let a = 10f64.powf(gain_db / 40.0);
    let root = 2.0 * a.sqrt() * alpha;
    match kind {
        KIND_PEAKING => normalized(
            [1.0 + alpha * a, -2.0 * cos, 1.0 - alpha * a],
            [1.0 + alpha / a, -2.0 * cos, 1.0 - alpha / a],
        ),
        KIND_LOW_SHELF => normalized(
            [
                a * ((a + 1.0) - (a - 1.0) * cos + root),
                2.0 * a * ((a - 1.0) - (a + 1.0) * cos),
                a * ((a + 1.0) - (a - 1.0) * cos - root),
            ],
            [
                (a + 1.0) + (a - 1.0) * cos + root,
                -2.0 * ((a - 1.0) + (a + 1.0) * cos),
                (a + 1.0) + (a - 1.0) * cos - root,
            ],
        ),
        KIND_HIGH_SHELF => normalized(
            [
                a * ((a + 1.0) + (a - 1.0) * cos + root),
                -2.0 * a * ((a - 1.0) + (a + 1.0) * cos),
                a * ((a + 1.0) + (a - 1.0) * cos - root),
            ],
            [
                (a + 1.0) - (a - 1.0) * cos + root,
                2.0 * ((a - 1.0) - (a + 1.0) * cos),
                (a + 1.0) - (a - 1.0) * cos - root,
            ],
        ),
        KIND_LOW_PASS => normalized(
            [(1.0 - cos) / 2.0, 1.0 - cos, (1.0 - cos) / 2.0],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        ),
        KIND_HIGH_PASS => normalized(
            [(1.0 + cos) / 2.0, -(1.0 + cos), (1.0 + cos) / 2.0],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        ),
        KIND_BAND_PASS => normalized([alpha, 0.0, -alpha], [1.0 + alpha, -2.0 * cos, 1.0 - alpha]),
        KIND_NOTCH => normalized(
            [1.0, -2.0 * cos, 1.0],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        ),
        _ => normalized(
            [1.0 - alpha, -2.0 * cos, 1.0 + alpha],
            [1.0 + alpha, -2.0 * cos, 1.0 - alpha],
        ),
    }
}

// A first-order low- or high-pass by the bilinear transform, for odd Butterworth orders.
fn first_order(low: bool, w0: f64) -> [f64; 5] {
    let k = (w0 / 2.0).tan();
    let a1 = (k - 1.0) / (k + 1.0);
    if low {
        [k / (k + 1.0), k / (k + 1.0), 0.0, a1, 0.0]
    } else {
        [1.0 / (k + 1.0), -1.0 / (k + 1.0), 0.0, a1, 0.0]
    }
}

fn magnitude(section: &[f64; 5], w: f64) -> f64 {
    let (s1, c1) = w.sin_cos();
    let (s2, c2) = (2.0 * w).sin_cos();
    let [b0, b1, b2, a1, a2] = *section;
    let (nr, ni) = (b0 + b1 * c1 + b2 * c2, -(b1 * s1 + b2 * s2));
    let (dr, di) = (1.0 + a1 * c1 + a2 * c2, -(a1 * s1 + a2 * s2));
    ((nr * nr + ni * ni) / (dr * dr + di * di)).sqrt()
}

// Whether `bands` change the signal at `rate`.
pub(crate) fn acts(bands: &[Band], rate: f64) -> bool {
    bands.iter().any(|b| b.valid() && b.acts(rate))
}

// The level in dB that `bands` give `channel` at `frequency`, before the preamp.
pub(crate) fn response_db(bands: &[Band], rate: f64, channel: usize, frequency: f64) -> f64 {
    let w = 2.0 * PI * frequency / rate;
    bands
        .iter()
        .filter(|b| b.valid() && b.acts(rate) && b.applies_to(channel))
        .flat_map(|b| b.sections(rate))
        .map(|s| 20.0 * magnitude(&s, w).log10())
        .sum()
}

// The preamp in dB (0 or less) that keeps the combined response of every channel at or below 0 dB.
pub(crate) fn preamp_db(bands: &[Band], rate: f64, channels: usize) -> f64 {
    let nyquist = rate / 2.0;
    let low: f64 = 10.0;
    let high = nyquist * 0.999;
    let mut frequencies: Vec<f64> = (0..RESPONSE_POINTS)
        .map(|i| low * (high / low).powf(i as f64 / (RESPONSE_POINTS - 1) as f64))
        .collect();
    frequencies.extend(bands.iter().map(|b| b.frequency).filter(|&f| f < nyquist));
    let peak = (0..channels)
        .flat_map(|c| frequencies.iter().map(move |&f| (c, f)))
        .map(|(c, f)| response_db(bands, rate, c, f))
        .fold(0.0, f64::max);
    -peak
}

#[derive(Clone)]
struct Section {
    coefficients: [f64; 5],
    channel_mask: Vec<bool>,
    // Transposed direct form II state per channel.
    state: Vec<[f64; 2]>,
}

#[derive(Clone, Default)]
struct Chain {
    gain: f64,
    sections: Vec<Section>,
}

impl Chain {
    fn new(bands: &[Band], rate: f64, channels: usize) -> Self {
        let acting: Vec<&Band> = bands.iter().filter(|b| b.valid() && b.acts(rate)).collect();
        if acting.is_empty() {
            return Chain::default();
        }
        let owned: Vec<Band> = acting.iter().map(|&&b| b).collect();
        let mut sections = Vec::new();
        for band in &acting {
            let mask: Vec<bool> = (0..channels).map(|c| band.applies_to(c)).collect();
            for coefficients in band.sections(rate) {
                sections.push(Section {
                    coefficients,
                    channel_mask: mask.clone(),
                    state: vec![[0.0; 2]; channels],
                });
            }
        }
        Chain {
            gain: 10f64.powf(preamp_db(&owned, rate, channels) / 20.0),
            sections,
        }
    }

    fn is_identity(&self) -> bool {
        self.sections.is_empty()
    }

    fn process(&mut self, frame: &mut [f64]) {
        if self.is_identity() {
            return;
        }
        for (channel, sample) in frame.iter_mut().enumerate() {
            let mut x = *sample * self.gain;
            for section in &mut self.sections {
                if !section.channel_mask[channel] {
                    continue;
                }
                let [b0, b1, b2, a1, a2] = section.coefficients;
                let s = &mut section.state[channel];
                let y = b0 * x + s[0];
                s[0] = b1 * x - a1 * y + s[1];
                s[1] = b2 * x - a2 * y;
                x = y;
            }
            *sample = x;
        }
    }

    // Takes over the state of `old`'s sections where the band layout matches, so a small change starts warm.
    fn adopt_state(&mut self, old: &Chain) {
        if self.sections.len() != old.sections.len() {
            return;
        }
        for (new, old) in self.sections.iter_mut().zip(&old.sections) {
            new.state.clone_from(&old.state);
        }
    }
}

pub(crate) struct Equalizer {
    rate: f64,
    channels: usize,
    current: Chain,
    // The chain being faded out and the frames of the fade left.
    previous: Option<(Chain, usize)>,
    started: bool,
    clipped: u64,
}

impl Equalizer {
    pub(crate) fn new(rate: u32, channels: usize) -> Self {
        Equalizer {
            rate: f64::from(rate),
            channels,
            current: Chain::default(),
            previous: None,
            started: false,
            clipped: 0,
        }
    }

    /// Applies `bands` (empty: off). Once audio has passed, the change crossfades.
    pub(crate) fn set(&mut self, bands: &[Band]) {
        let mut chain = Chain::new(bands, self.rate, self.channels);
        chain.adopt_state(&self.current);
        let old = std::mem::replace(&mut self.current, chain);
        if self.started && !(old.is_identity() && self.current.is_identity()) {
            let frames = (self.rate * CROSSFADE_MS / 1000.0).round() as usize;
            self.previous = Some((old, frames.max(1)));
        }
    }

    /// Whether the EQ changes the signal now: false once it is off or flat and any fade has ended.
    pub(crate) fn active(&self) -> bool {
        !self.current.is_identity() || self.previous.is_some()
    }

    /// Samples the EQ sent above full scale, which the final quantization clips.
    pub(crate) fn clipped(&self) -> u64 {
        self.clipped
    }

    /// Processes interleaved frames of `channels` in place.
    pub(crate) fn process(&mut self, samples: &mut [f64]) {
        self.started = true;
        if !self.active() {
            return;
        }
        let mut dry = vec![0.0; self.channels];
        for frame in samples.chunks_exact_mut(self.channels) {
            if let Some((old, left)) = self.previous.as_mut() {
                dry.copy_from_slice(frame);
                old.process(&mut dry);
                self.current.process(frame);
                let total = (self.rate * CROSSFADE_MS / 1000.0).round().max(1.0);
                let t = 1.0 - *left as f64 / total;
                for (wet, &faded) in frame.iter_mut().zip(&dry) {
                    *wet = faded + (*wet - faded) * t;
                }
                *left -= 1;
                if *left == 0 {
                    self.previous = None;
                }
            } else {
                self.current.process(frame);
            }
            self.clipped += frame.iter().filter(|s| s.abs() > 1.0).count() as u64;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn band(kind: u32, frequency: f64, gain_db: f64, q: f64) -> Band {
        Band {
            kind,
            channels: CHANNELS_ALL,
            slope: 12,
            frequency,
            gain_db,
            q,
        }
    }

    // The level of a steady sine at `frequency` after `eq`, in dB.
    fn measured_db(eq: &mut Equalizer, frequency: f64, rate: f64) -> f64 {
        let frames = rate as usize;
        let mut samples: Vec<f64> = (0..frames)
            .flat_map(|i| {
                let v = 0.25 * (2.0 * PI * frequency * i as f64 / rate).sin();
                [v, v]
            })
            .collect();
        eq.process(&mut samples);
        // Skip the first half second: the filters settle and any fade ends.
        let tail = &samples[frames..];
        let rms =
            (tail.iter().step_by(2).map(|v| v * v).sum::<f64>() / (tail.len() / 2) as f64).sqrt();
        20.0 * (rms / (0.25 / 2f64.sqrt())).log10()
    }

    #[test]
    fn every_kind_matches_the_cookbook_response_at_its_frequency() {
        for rate in [44_100.0, 96_000.0, 384_000.0] {
            let at = |b: Band, f: f64| response_db(&[b], rate, 0, f);
            assert!((at(band(KIND_PEAKING, 1000.0, 6.0, 1.0), 1000.0) - 6.0).abs() < 1e-9);
            assert!((at(band(KIND_PEAKING, 1000.0, -9.5, 2.0), 1000.0) + 9.5).abs() < 1e-9);
            // Shelves reach their gain far from the corner and half of it at the corner.
            assert!((at(band(KIND_LOW_SHELF, 200.0, 6.0, 0.707), 20.0) - 6.0).abs() < 0.1);
            assert!((at(band(KIND_LOW_SHELF, 200.0, 6.0, 0.707), 200.0) - 3.0).abs() < 1e-9);
            assert!((at(band(KIND_HIGH_SHELF, 5000.0, -4.0, 0.707), 5000.0) + 2.0).abs() < 1e-9);
            // A Butterworth Q puts the corner of a 12 dB/oct pass at -3.01 dB.
            let corner = -10.0 * 2f64.log10();
            let q = 1.0 / 2f64.sqrt();
            assert!((at(band(KIND_LOW_PASS, 2000.0, 0.0, q), 2000.0) - corner).abs() < 1e-9);
            assert!((at(band(KIND_HIGH_PASS, 80.0, 0.0, q), 80.0) - corner).abs() < 1e-9);
            assert!(at(band(KIND_BAND_PASS, 3000.0, 0.0, 1.0), 3000.0).abs() < 1e-9);
            assert!(at(band(KIND_NOTCH, 60.0, 0.0, 5.0), 60.0) < -100.0);
            for f in [30.0, 1000.0, 15000.0] {
                assert!(at(band(KIND_ALL_PASS, 1000.0, 0.0, 1.0), f).abs() < 1e-9);
            }
        }
    }

    #[test]
    fn steeper_passes_are_butterworth_with_the_slope_asked_for() {
        let rate = 96_000.0;
        for slope in [6, 18, 24, 36, 48] {
            let mut low = band(KIND_LOW_PASS, 1000.0, 0.0, 9.0); // Q is ignored off 12 dB/oct
            low.slope = slope;
            let corner = response_db(&[low], rate, 0, 1000.0);
            assert!(
                (corner + 10.0 * 2f64.log10()).abs() < 1e-6,
                "slope {slope}: {corner}"
            );
            // Two octaves up the response falls by about the slope per octave.
            let fall = response_db(&[low], rate, 0, 4000.0) - response_db(&[low], rate, 0, 8000.0);
            assert!(
                (fall - f64::from(slope)).abs() < 1.5,
                "slope {slope}: {fall}"
            );
        }
    }

    #[test]
    fn a_processed_sine_follows_the_computed_response() {
        let rate = 48_000.0;
        let bands = [
            band(KIND_PEAKING, 1000.0, 6.0, 1.0),
            band(KIND_HIGH_SHELF, 8000.0, -3.0, 0.707),
        ];
        let preamp = preamp_db(&bands, rate, 2);
        for f in [100.0, 1000.0, 3000.0, 12000.0] {
            let mut eq = Equalizer::new(48_000, 2);
            eq.set(&bands);
            let expected = response_db(&bands, rate, 0, f) + preamp;
            let measured = measured_db(&mut eq, f, rate);
            assert!(
                (measured - expected).abs() < 0.05,
                "{f} Hz: {measured} vs {expected}"
            );
        }
    }

    #[test]
    fn the_preamp_removes_the_largest_boost_and_never_raises_the_level() {
        let rate = 44_100.0;
        let boost = [
            band(KIND_PEAKING, 100.0, 8.0, 0.7),
            band(KIND_PEAKING, 120.0, 4.0, 0.7),
        ];
        let preamp = preamp_db(&boost, rate, 2);
        assert!(preamp < -8.0 && preamp > -12.5, "{preamp}");
        assert_eq!(
            preamp_db(&[band(KIND_PEAKING, 1000.0, -6.0, 1.0)], rate, 2),
            0.0
        );
        let mut eq = Equalizer::new(44_100, 2);
        eq.set(&boost);
        let mut full: Vec<f64> = (0..44_100)
            .flat_map(|i| {
                let v = 0.99 * (2.0 * PI * 110.0 * i as f64 / rate).sin();
                [v, v]
            })
            .collect();
        eq.process(&mut full);
        assert_eq!(eq.clipped(), 0);
    }

    #[test]
    fn bands_that_cannot_change_the_signal_leave_it_bypassed() {
        let rate = 44_100;
        let mut eq = Equalizer::new(rate, 2);
        eq.set(&[
            band(KIND_PEAKING, 1000.0, 0.0, 1.0),
            band(KIND_LOW_SHELF, 100.0, 0.0, 0.7),
            band(KIND_HIGH_SHELF, 30_000.0, 6.0, 0.7), // above Nyquist at 44.1 kHz
            band(KIND_PEAKING, f64::NAN, 3.0, 1.0),
        ]);
        assert!(!eq.active());
        let mut samples = vec![0.5, -0.25, 0.125, 0.0];
        eq.process(&mut samples);
        assert_eq!(samples, [0.5, -0.25, 0.125, 0.0]);
        let mut eq = Equalizer::new(96_000, 2);
        eq.set(&[band(KIND_HIGH_SHELF, 30_000.0, 6.0, 0.7)]);
        assert!(eq.active());
    }

    #[test]
    fn a_band_for_one_channel_leaves_the_other_untouched() {
        let rate = 48_000.0;
        let mut left = band(KIND_PEAKING, 1000.0, -6.0, 1.0);
        left.channels = CHANNELS_LEFT;
        assert!((response_db(&[left], rate, 0, 1000.0) + 6.0).abs() < 1e-9);
        assert_eq!(response_db(&[left], rate, 1, 1000.0), 0.0);
        let mut eq = Equalizer::new(48_000, 2);
        eq.set(&[left]);
        let mut samples: Vec<f64> = (0..4800)
            .flat_map(|i| [0.0, (i as f64 * 0.01).sin() * 0.5])
            .collect();
        let right: Vec<f64> = samples.iter().skip(1).step_by(2).copied().collect();
        eq.process(&mut samples);
        let after: Vec<f64> = samples.iter().skip(1).step_by(2).copied().collect();
        assert_eq!(after, right);
    }

    #[test]
    fn a_change_crossfades_without_a_jump() {
        let rate = 48_000.0;
        let mut eq = Equalizer::new(48_000, 2);
        eq.set(&[band(KIND_PEAKING, 1000.0, -12.0, 1.0)]);
        let sine = |i: usize| 0.5 * (2.0 * PI * 1000.0 * i as f64 / rate).sin();
        let mut first: Vec<f64> = (0..4800).flat_map(|i| [sine(i), sine(i)]).collect();
        eq.process(&mut first);
        eq.set(&[band(KIND_PEAKING, 1000.0, 12.0, 1.0)]);
        let mut second: Vec<f64> = (4800..9600).flat_map(|i| [sine(i), sine(i)]).collect();
        eq.process(&mut second);
        let joined: Vec<f64> = first.iter().chain(&second).step_by(2).copied().collect();
        // A 1 kHz sine at 48 kHz moves at most about 0.13 x its amplitude per sample; a hard switch would jump.
        let largest_step = joined
            .windows(2)
            .map(|w| (w[1] - w[0]).abs())
            .fold(0.0, f64::max);
        assert!(largest_step < 0.2, "{largest_step}");
        // Turning it off fades out, then the EQ is bypassed.
        eq.set(&[]);
        assert!(eq.active());
        let mut rest: Vec<f64> = (9600..14400).flat_map(|i| [sine(i), sine(i)]).collect();
        eq.process(&mut rest);
        assert!(!eq.active());
    }
}
