// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The DSP chain of the stage after the predecode ring (docs/Plan.md §5.2, §5.9): EQ, crossfeed and convolution in
//! the order the user set, after one preamp for the whole chain.
//!
//! The preamp drops by the largest gain of the chain's combined response on either output channel, cross paths
//! counted at their worst phase, so a steady signal never exceeds full scale. A changed node crossfades from its old
//! state to its new one, a changed preamp ramps, and a new order fades out and back in, so no change clicks. With no
//! node changing the signal the chain is bypassed and playback stays bit-perfect.

use std::sync::Arc;

use crate::convolver::{Convolver, ImpulseResponse};
use crate::crossfeed::{self, Crossfeed};
use crate::eq::{self, Band, EqNode};
use crate::fft::Complex;

pub(crate) const NODE_EQ: u32 = 1;
pub(crate) const NODE_CROSSFEED: u32 = 2;
pub(crate) const NODE_CONVOLUTION: u32 = 3;
pub(crate) const DEFAULT_ORDER: [u32; 3] = [NODE_EQ, NODE_CROSSFEED, NODE_CONVOLUTION];

const FADE_MS: f64 = 20.0;
// The chain works through blocks of at most this many frames, so its scratch space is fixed.
const BLOCK_FRAMES: usize = 1_024;
// Without an impulse response, the response is searched at this many log-spaced frequencies and each band's own.
const RESPONSE_POINTS: usize = 512;

#[derive(Clone, Default)]
pub(crate) struct Settings {
    pub order: [u32; 3],
    pub bands: Vec<Band>,
    pub crossfeed: Option<crossfeed::Params>,
    pub impulse: Option<Arc<ImpulseResponse>>,
    /// The software volume in dB, 0 or less; negative infinity mutes.
    pub volume_db: f64,
}

impl Settings {
    /// Whether the order names each node once and every value can be used at `rate`.
    pub(crate) fn valid(&self, rate: u32) -> bool {
        let mut sorted = self.order;
        sorted.sort_unstable();
        sorted == [NODE_EQ, NODE_CROSSFEED, NODE_CONVOLUTION]
            && self.crossfeed.is_none_or(|p| p.valid())
            && self.volume_db <= 0.0
            && self.impulse.as_ref().is_none_or(|ir| ir.rate() == rate)
    }
}

trait Node {
    fn is_identity(&self) -> bool;
    fn process(&mut self, samples: &mut [f64], channels: usize);
    fn reset(&mut self);
}

impl Node for EqNode {
    fn is_identity(&self) -> bool {
        EqNode::is_identity(self)
    }
    fn process(&mut self, samples: &mut [f64], channels: usize) {
        EqNode::process(self, samples, channels);
    }
    fn reset(&mut self) {
        EqNode::reset(self);
    }
}

impl Node for Option<Crossfeed> {
    fn is_identity(&self) -> bool {
        self.is_none()
    }
    fn process(&mut self, samples: &mut [f64], channels: usize) {
        if let Some(node) = self {
            node.process(samples, channels);
        }
    }
    fn reset(&mut self) {
        if let Some(node) = self {
            node.reset();
        }
    }
}

impl Node for Option<Convolver> {
    fn is_identity(&self) -> bool {
        self.is_none()
    }
    fn process(&mut self, samples: &mut [f64], channels: usize) {
        if let Some(node) = self {
            node.process(samples, channels);
        }
    }
    fn reset(&mut self) {
        if let Some(node) = self {
            node.reset();
        }
    }
}

// The most nodes a change fades from; a further change drops the faintest.
const MAX_BLENDED: usize = 4;

// A node and, while a change fades in, what was heard before it: the nodes it replaced, each at the share it had then.
struct Faded<N> {
    current: N,
    previous: Vec<(N, f64)>,
    left: usize,
    fade: usize,
    // Nodes whose fade has ended, freed at the next change rather than while audio is processed.
    done: Vec<N>,
    scratch: Vec<f64>,
    blend: Vec<f64>,
}

impl<N: Node> Faded<N> {
    fn new(node: N, fade: usize, channels: usize) -> Self {
        Faded {
            current: node,
            previous: Vec::with_capacity(MAX_BLENDED),
            left: 0,
            fade,
            done: Vec::with_capacity(MAX_BLENDED),
            scratch: vec![0.0; BLOCK_FRAMES * channels],
            blend: vec![0.0; BLOCK_FRAMES * channels],
        }
    }

    fn active(&self) -> bool {
        !self.current.is_identity() || !self.previous.is_empty()
    }

    // Replaces the node, fading once audio has passed; returns a node no longer needed, if any. A change within a
    // fade fades from what is heard at that moment, the earlier nodes in their proportions then.
    fn set(&mut self, node: N, started: bool) -> Option<N> {
        self.done.clear();
        let old = std::mem::replace(&mut self.current, node);
        if !started || (self.previous.is_empty() && old.is_identity() && self.current.is_identity())
        {
            return Some(old);
        }
        let mut share = if self.previous.is_empty() {
            1.0
        } else {
            1.0 - self.left as f64 / self.fade as f64
        };
        for (_, weight) in &mut self.previous {
            *weight *= 1.0 - share;
        }
        let mut dropped = None;
        if self.previous.len() == MAX_BLENDED {
            let faintest = (0..MAX_BLENDED)
                .min_by(|&a, &b| self.previous[a].1.total_cmp(&self.previous[b].1))
                .expect("MAX_BLENDED is not 0");
            let (node, weight) = self.previous.swap_remove(faintest);
            let kept = 1.0 - weight;
            for (_, w) in &mut self.previous {
                *w /= kept;
            }
            share /= kept;
            dropped = Some(node);
        }
        self.previous.push((old, share));
        self.left = self.fade;
        dropped
    }

    fn reset(&mut self) {
        self.current.reset();
        self.done.extend(self.previous.drain(..).map(|(n, _)| n));
        self.left = 0;
    }

    // At most BLOCK_FRAMES frames.
    fn process(&mut self, samples: &mut [f64], channels: usize) {
        let Self {
            current,
            previous,
            left,
            fade,
            done,
            scratch,
            blend,
        } = self;
        if previous.is_empty() {
            current.process(samples, channels);
            return;
        }
        let blend = &mut blend[..samples.len()];
        blend.fill(0.0);
        for (node, weight) in previous.iter_mut() {
            let dry = &mut scratch[..samples.len()];
            dry.copy_from_slice(samples);
            node.process(dry, channels);
            for (to, &from) in blend.iter_mut().zip(dry.iter()) {
                *to += *weight * from;
            }
        }
        current.process(samples, channels);
        let total = *fade as f64;
        for (frame, heard) in samples
            .chunks_exact_mut(channels)
            .zip(blend.chunks_exact(channels))
        {
            let t = 1.0 - *left as f64 / total;
            for (wet, &was) in frame.iter_mut().zip(heard) {
                *wet = was + (*wet - was) * t;
            }
            *left = left.saturating_sub(1);
        }
        if *left == 0 {
            done.extend(previous.drain(..).map(|(n, _)| n));
        }
    }
}

/// What the chain does at a rate: whether it changes the signal and the preamp it needs.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Analysis {
    pub active: bool,
    pub preamp_db: f64,
}

type Matrix = [[Complex; 2]; 2];

fn multiply(a: &Matrix, b: &Matrix) -> Matrix {
    let mut out = [[Complex::ZERO; 2]; 2];
    for (o, row) in out.iter_mut().enumerate() {
        for (i, cell) in row.iter_mut().enumerate() {
            *cell = a[o][0] * b[0][i] + a[o][1] * b[1][i];
        }
    }
    out
}

/// Whether `settings` change the signal at `rate` on `channels`, and the preamp in dB (0 or less) that keeps their
/// combined response at or below 0 dB on every channel.
pub(crate) fn analyze(settings: &Settings, rate: u32, channels: usize) -> Analysis {
    let rate_f = f64::from(rate);
    let eq_on = eq::acts(&settings.bands, rate_f);
    let crossfeed = settings.crossfeed.filter(|p| p.valid() && channels >= 2);
    let impulse = settings.impulse.as_ref().filter(|ir| ir.rate() == rate);
    if !eq_on && crossfeed.is_none() && impulse.is_none() {
        return Analysis {
            active: false,
            preamp_db: 0.0,
        };
    }
    let frequencies: Vec<f64> = match impulse {
        Some(ir) => ir.summary().frequencies.clone(),
        None => {
            let (low, high) = (10.0, rate_f / 2.0 * 0.999);
            let mut points: Vec<f64> = (0..RESPONSE_POINTS)
                .map(|i| low * (high / low).powf(i as f64 / (RESPONSE_POINTS - 1) as f64))
                .collect();
            points.extend(
                settings
                    .bands
                    .iter()
                    .map(|b| b.frequency)
                    .filter(|&f| f < rate_f / 2.0),
            );
            points.extend(crossfeed.map(|p| f64::from(p.cutoff_hz)));
            points
        }
    };
    // The filters are worked out once, not at every frequency: it ran under the stage lock.
    let eq_sections: [Vec<[f64; 5]>; 3] = if eq_on {
        std::array::from_fn(|channel| eq::channel_sections(&settings.bands, rate_f, channel))
    } else {
        Default::default()
    };
    let crossfeed_at = crossfeed.map(|p| crossfeed::responses(p, rate_f));
    let one = Complex::new(1.0, 0.0);
    let identity = [[one, Complex::ZERO], [Complex::ZERO, one]];
    let mut peak: f64 = 0.0;
    for (i, &f) in frequencies.iter().enumerate() {
        let mut chain = identity;
        for node in settings.order {
            let stage: Option<Matrix> = match node {
                NODE_EQ => eq_on.then(|| {
                    [
                        [
                            eq::sections_response(&eq_sections[0], rate_f, f),
                            Complex::ZERO,
                        ],
                        [
                            Complex::ZERO,
                            eq::sections_response(&eq_sections[1], rate_f, f),
                        ],
                    ]
                }),
                NODE_CROSSFEED => crossfeed_at.as_ref().map(|at| {
                    let (direct, cross) = at(f);
                    [[direct, cross], [cross, direct]]
                }),
                NODE_CONVOLUTION => impulse.map(|ir| {
                    let mut m = [[Complex::ZERO; 2]; 2];
                    for (input, output, values) in &ir.summary().paths {
                        m[*output][*input] = values[i];
                    }
                    m
                }),
                _ => None,
            };
            if let Some(stage) = stage {
                chain = multiply(&stage, &chain);
            }
        }
        for row in chain.iter().take(channels.min(2)) {
            peak = peak.max(row[0].norm() + row[1].norm());
        }
        if channels > 2 && eq_on {
            // Further channels get only the bands for every channel.
            peak = peak.max(eq::sections_response(&eq_sections[2], rate_f, f).norm());
        }
    }
    Analysis {
        active: true,
        preamp_db: if peak > 1.0 {
            -20.0 * peak.log10()
        } else {
            0.0
        },
    }
}

pub(crate) struct Dsp {
    rate: u32,
    channels: usize,
    settings: Settings,
    eq: Faded<EqNode>,
    crossfeed: Faded<Option<Crossfeed>>,
    convolution: Faded<Option<Convolver>>,
    order: [u32; 3],
    // A new order and the frames left of the fade out, then of the fade in.
    reorder: Option<([u32; 3], usize, usize)>,
    gain: f64,
    target_gain: f64,
    ramp_left: usize,
    started: bool,
    clipped: u64,
    // Replaced convolvers, freed on the next change rather than in the DSP stage.
    retired: Vec<Convolver>,
}

impl Dsp {
    pub(crate) fn new(rate: u32, channels: usize) -> Self {
        let fade = ((f64::from(rate) * FADE_MS / 1000.0).round() as usize).max(1);
        Dsp {
            rate,
            channels,
            settings: Settings {
                order: DEFAULT_ORDER,
                ..Settings::default()
            },
            eq: Faded::new(EqNode::default(), fade, channels),
            crossfeed: Faded::new(None, fade, channels),
            convolution: Faded::new(None, fade, channels),
            order: DEFAULT_ORDER,
            reorder: None,
            gain: 1.0,
            target_gain: 1.0,
            ramp_left: 0,
            started: false,
            clipped: 0,
            retired: Vec::with_capacity(4),
        }
    }

    fn fade_frames(&self) -> usize {
        self.eq.fade
    }

    fn retire(&mut self, node: Option<Option<Convolver>>) {
        if let Some(Some(convolver)) = node {
            self.retired.push(convolver);
        }
    }

    /// Applies `settings`, which must be valid for this rate. Once audio has passed, the change fades in.
    pub(crate) fn set(&mut self, settings: Settings) {
        self.retired.clear();
        let rate = f64::from(self.rate);
        let started = self.started;
        if settings.bands != self.settings.bands {
            let mut node = EqNode::new(&settings.bands, rate, self.channels);
            node.adopt_state(&self.eq.current);
            self.eq.set(node, started);
        }
        if settings.crossfeed != self.settings.crossfeed {
            let node = settings.crossfeed.map(|p| Crossfeed::new(p, rate));
            self.crossfeed.set(node, started);
        }
        let same_impulse = match (&settings.impulse, &self.settings.impulse) {
            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        if !same_impulse {
            let node = settings.impulse.clone().map(Convolver::new);
            let old = self.convolution.set(node, started);
            self.retire(old);
        }
        let pending = self.reorder.map_or(self.order, |(order, _, _)| order);
        if settings.order != pending {
            if started && self.active() {
                let half = (self.fade_frames() / 2).max(1);
                // From the level heard now: a fade out under way goes on, and one fading back in turns down from there.
                let out = match self.reorder {
                    Some((_, out, _)) if out > 0 => out,
                    Some((_, _, into)) => half - into.min(half),
                    None => half,
                };
                self.reorder = Some((settings.order, out, half));
            } else {
                self.order = settings.order;
                self.reorder = None;
            }
        }
        // The volume is the chain's last gain and the preamp its first; both are scalars, so they apply as one.
        let target = 10f64.powf(
            (analyze(&settings, self.rate, self.channels).preamp_db + settings.volume_db) / 20.0,
        );
        if target != self.target_gain {
            self.target_gain = target;
            if started {
                self.ramp_left = self.fade_frames();
            } else {
                self.gain = target;
            }
        }
        self.settings = settings;
    }

    /// Whether the chain changes the signal now: false once nothing does and every fade has ended.
    pub(crate) fn active(&self) -> bool {
        self.eq.active()
            || self.crossfeed.active()
            || self.convolution.active()
            || self.reorder.is_some()
            || self.ramp_left > 0
            || self.gain != 1.0
    }

    /// Forgets the stream: filter state, tails and fades. The next frame starts a new stream.
    pub(crate) fn reset(&mut self) {
        self.eq.reset();
        self.crossfeed.reset();
        self.convolution.reset();
        if let Some((order, _, _)) = self.reorder.take() {
            self.order = order;
        }
        self.gain = self.target_gain;
        self.ramp_left = 0;
    }

    /// The frames after the input ends that still carry the chain's response.
    pub(crate) fn tail_frames(&self) -> usize {
        self.convolution
            .current
            .as_ref()
            .map_or(0, |c| c.impulse_response().tail_frames())
    }

    /// Samples sent above full scale, which the final quantization clips: the chain's, or while no chain acts, those
    /// of the conversion before it.
    pub(crate) fn clipped(&self) -> u64 {
        self.clipped
    }

    /// Counts the samples above full scale of a conversion that passes no chain.
    pub(crate) fn count_clipped(&mut self, samples: &[f64]) {
        self.clipped += samples.iter().filter(|s| s.abs() > 1.0).count() as u64;
    }

    /// Processes interleaved frames in place.
    pub(crate) fn process(&mut self, samples: &mut [f64]) {
        self.started = true;
        if !self.active() {
            return;
        }
        let channels = self.channels;
        let mut rest = samples;
        while !rest.is_empty() {
            let mut frames = (rest.len() / channels).min(BLOCK_FRAMES);
            if let Some((_, out, _)) = self.reorder.filter(|&(_, out, _)| out > 0) {
                frames = frames.min(out);
            }
            let (block, next) = std::mem::take(&mut rest).split_at_mut(frames * channels);
            self.process_block(block);
            rest = next;
        }
    }

    fn process_block(&mut self, block: &mut [f64]) {
        let channels = self.channels;
        for frame in block.chunks_exact_mut(channels) {
            if self.ramp_left > 0 {
                self.ramp_left -= 1;
                self.gain = if self.ramp_left == 0 {
                    self.target_gain
                } else {
                    self.gain + (self.target_gain - self.gain) / (self.ramp_left + 1) as f64
                };
            }
            for sample in frame {
                *sample *= self.gain;
            }
        }
        for node in self.order {
            match node {
                NODE_EQ => self.eq.process(block, channels),
                NODE_CROSSFEED => self.crossfeed.process(block, channels),
                _ => self.convolution.process(block, channels),
            }
        }
        let half = (self.fade_frames() / 2).max(1) as f64;
        if let Some((order, out, into)) = self.reorder.as_mut() {
            for frame in block.chunks_exact_mut(channels) {
                let level = if *out > 0 {
                    *out -= 1;
                    *out as f64 / half
                } else {
                    *into = into.saturating_sub(1);
                    1.0 - *into as f64 / half
                };
                for sample in frame {
                    *sample *= level;
                }
            }
            if *out == 0 && self.order != *order {
                self.order = *order;
            }
            if *out == 0 && *into == 0 {
                self.reorder = None;
            }
        }
        self.clipped += block.iter().filter(|s| s.abs() > 1.0).count() as u64;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::eq::{CHANNELS_ALL, CHANNELS_LEFT, KIND_HIGH_SHELF, KIND_LOW_SHELF, KIND_PEAKING};
    use std::f64::consts::PI;

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

    fn eq(bands: &[Band]) -> Settings {
        Settings {
            order: DEFAULT_ORDER,
            bands: bands.to_vec(),
            ..Settings::default()
        }
    }

    fn sine(frequency: f64, rate: f64, level: f64, frames: std::ops::Range<usize>) -> Vec<f64> {
        frames
            .flat_map(|i| {
                let v = level * (2.0 * PI * frequency * i as f64 / rate).sin();
                [v, v]
            })
            .collect()
    }

    #[test]
    fn the_preamp_removes_the_largest_boost_and_never_raises_the_level() {
        let boost = [
            band(KIND_PEAKING, 100.0, 8.0, 0.7),
            band(KIND_PEAKING, 120.0, 4.0, 0.7),
        ];
        let preamp = analyze(&eq(&boost), 44_100, 2).preamp_db;
        assert!(preamp < -8.0 && preamp > -12.5, "{preamp}");
        let cut = analyze(&eq(&[band(KIND_PEAKING, 1000.0, -6.0, 1.0)]), 44_100, 2);
        assert_eq!(
            cut,
            Analysis {
                active: true,
                preamp_db: 0.0
            }
        );
        let mut dsp = Dsp::new(44_100, 2);
        dsp.set(eq(&boost));
        let mut full = sine(110.0, 44_100.0, 0.99, 0..44_100);
        dsp.process(&mut full);
        assert_eq!(dsp.clipped(), 0);
    }

    #[test]
    fn opposite_corrections_cost_level_once() {
        // A boost before a cut of the same shape is flat: the chain keeps its level.
        let bands = [band(KIND_LOW_SHELF, 200.0, 6.0, 0.7)];
        let mut cut = vec![0.0; 4_096];
        cut[0] = 1.0;
        let mut node = EqNode::new(&[band(KIND_LOW_SHELF, 200.0, -6.0, 0.7)], 48_000.0, 1);
        node.process(&mut cut, 1);
        let settings = Settings {
            impulse: Some(Arc::new(ImpulseResponse::new(&cut, 1, 48_000).unwrap())),
            ..eq(&bands)
        };
        let preamp = analyze(&settings, 48_000, 2).preamp_db;
        assert!(preamp > -0.05, "{preamp}");
        assert!(analyze(&eq(&bands), 48_000, 2).preamp_db < -5.9);
    }

    #[test]
    fn cross_paths_count_at_their_worst_phase() {
        // True stereo where each input reaches both outputs at full level: up to 6 dB.
        let four = [1.0; 4];
        let settings = Settings {
            order: DEFAULT_ORDER,
            impulse: Some(Arc::new(ImpulseResponse::new(&four, 4, 48_000).unwrap())),
            ..Settings::default()
        };
        let preamp = analyze(&settings, 48_000, 2).preamp_db;
        assert!((preamp + 20.0 * 2f64.log10()).abs() < 1e-9, "{preamp}");
    }

    #[test]
    fn nothing_that_changes_the_signal_leaves_it_bypassed() {
        let mut dsp = Dsp::new(44_100, 2);
        dsp.set(eq(&[
            band(KIND_PEAKING, 1000.0, 0.0, 1.0),
            band(KIND_HIGH_SHELF, 30_000.0, 6.0, 0.7),
        ]));
        assert!(!dsp.active());
        let mut samples = vec![0.5, -0.25, 0.125, 0.0];
        dsp.process(&mut samples);
        assert_eq!(samples, [0.5, -0.25, 0.125, 0.0]);
        assert!(!analyze(&eq(&[]), 44_100, 2).active);
    }

    #[test]
    fn a_change_crossfades_without_a_jump_and_ends_bypassed() {
        let rate = 48_000.0;
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(eq(&[band(KIND_PEAKING, 1000.0, -12.0, 1.0)]));
        let mut first = sine(1000.0, rate, 0.5, 0..4800);
        dsp.process(&mut first);
        dsp.set(eq(&[band(KIND_PEAKING, 1000.0, 12.0, 1.0)]));
        let mut second = sine(1000.0, rate, 0.5, 4800..9600);
        dsp.process(&mut second);
        let joined: Vec<f64> = first.iter().chain(&second).step_by(2).copied().collect();
        // A 1 kHz sine at 48 kHz moves at most about 0.13 x its amplitude per sample; a hard switch would jump.
        let largest_step = joined
            .windows(2)
            .map(|w| (w[1] - w[0]).abs())
            .fold(0.0, f64::max);
        assert!(largest_step < 0.2, "{largest_step}");
        dsp.set(eq(&[]));
        assert!(dsp.active());
        let mut rest = sine(1000.0, rate, 0.5, 9600..14400);
        dsp.process(&mut rest);
        assert!(!dsp.active());
    }

    fn largest_step(parts: &[&[f64]]) -> f64 {
        let left: Vec<f64> = parts
            .iter()
            .flat_map(|p| p.iter().step_by(2))
            .copied()
            .collect();
        left.windows(2)
            .map(|w| (w[1] - w[0]).abs())
            .fold(0.0, f64::max)
    }

    // A second change within the fade, an EQ band dragged or a node toggled twice, goes on from what is heard then.
    #[test]
    fn a_change_within_a_fade_goes_on_from_what_is_heard() {
        let rate = 48_000.0;
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(eq(&[band(KIND_PEAKING, 1000.0, -12.0, 1.0)]));
        let mut first = sine(1000.0, rate, 0.5, 0..4800);
        dsp.process(&mut first);
        dsp.set(eq(&[band(KIND_PEAKING, 1000.0, 12.0, 1.0)]));
        let mut second = sine(1000.0, rate, 0.5, 4800..5052);
        dsp.process(&mut second);
        dsp.set(eq(&[band(KIND_PEAKING, 1000.0, -6.0, 1.0)]));
        let mut third = sine(1000.0, rate, 0.5, 5052..9600);
        dsp.process(&mut third);
        let step = largest_step(&[&first, &second, &third]);
        assert!(step < 0.2, "{step}");

        let mut left_cut = band(KIND_PEAKING, 300.0, -12.0, 1.0);
        left_cut.channels = CHANNELS_LEFT;
        let ordered = |order| Settings {
            order,
            bands: vec![left_cut],
            crossfeed: Some(crossfeed::Params {
                cutoff_hz: 700,
                feed_db: 4.5,
            }),
            impulse: None,
            volume_db: 0.0,
        };
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(ordered(DEFAULT_ORDER));
        let mut first = sine(500.0, rate, 0.5, 0..4800);
        dsp.process(&mut first);
        dsp.set(ordered([NODE_CROSSFEED, NODE_EQ, NODE_CONVOLUTION]));
        // Past the fade out and into the fade in, then the order changes back.
        let mut second = sine(500.0, rate, 0.5, 4800..5340);
        dsp.process(&mut second);
        dsp.set(ordered(DEFAULT_ORDER));
        let mut third = sine(500.0, rate, 0.5, 5340..9600);
        dsp.process(&mut third);
        let step = largest_step(&[&first, &second, &third]);
        assert!(step < 0.1, "{step}");
        assert!(dsp.reorder.is_none());
        assert_eq!(dsp.order, DEFAULT_ORDER);
    }

    #[test]
    fn the_order_matters_only_where_channels_differ_and_changes_fade() {
        let rate = 48_000;
        let mut left_cut = band(KIND_PEAKING, 300.0, -12.0, 1.0);
        left_cut.channels = CHANNELS_LEFT;
        let crossfeed = Some(crossfeed::Params {
            cutoff_hz: 700,
            feed_db: 4.5,
        });
        let run = |order: [u32; 3], bands: Vec<Band>| -> Vec<f64> {
            let mut dsp = Dsp::new(rate, 2);
            dsp.set(Settings {
                order,
                bands,
                crossfeed,
                impulse: None,
                volume_db: 0.0,
            });
            let mut samples: Vec<f64> = (0..9_600)
                .flat_map(|i| [(i as f64 * 0.04).sin() * 0.3, 0.0])
                .collect();
            dsp.process(&mut samples);
            samples
        };
        let other = [NODE_CROSSFEED, NODE_EQ, NODE_CONVOLUTION];
        let same = |a: &[f64], b: &[f64]| a.iter().zip(b).all(|(x, y)| (x - y).abs() < 1e-12);
        let both = band(KIND_PEAKING, 300.0, -12.0, 1.0);
        assert!(same(
            &run(DEFAULT_ORDER, vec![both]),
            &run(other, vec![both])
        ));
        assert!(!same(
            &run(DEFAULT_ORDER, vec![left_cut]),
            &run(other, vec![left_cut])
        ));

        let mut dsp = Dsp::new(rate, 2);
        dsp.set(Settings {
            order: DEFAULT_ORDER,
            bands: vec![left_cut],
            crossfeed,
            impulse: None,
            volume_db: 0.0,
        });
        let mut first = sine(500.0, 48_000.0, 0.5, 0..4800);
        dsp.process(&mut first);
        dsp.set(Settings {
            order: other,
            bands: vec![left_cut],
            crossfeed,
            impulse: None,
            volume_db: 0.0,
        });
        let mut second = sine(500.0, 48_000.0, 0.5, 4800..9600);
        dsp.process(&mut second);
        assert_eq!(dsp.order, other);
        assert!(dsp.reorder.is_none());
        let joined: Vec<f64> = first.iter().chain(&second).step_by(2).copied().collect();
        let largest_step = joined
            .windows(2)
            .map(|w| (w[1] - w[0]).abs())
            .fold(0.0, f64::max);
        assert!(largest_step < 0.1, "{largest_step}");
    }

    #[test]
    fn convolution_runs_in_the_chain_and_its_tail_is_known() {
        let rate = 48_000;
        let h = [0.5, 0.25, 0.125];
        let settings = Settings {
            order: DEFAULT_ORDER,
            impulse: Some(Arc::new(ImpulseResponse::new(&h, 1, rate).unwrap())),
            ..Settings::default()
        };
        let mut dsp = Dsp::new(rate, 2);
        dsp.set(settings);
        assert!(dsp.active());
        assert_eq!(dsp.tail_frames(), 2);
        let mut samples = vec![1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0];
        dsp.process(&mut samples);
        // The response's peak gain is 0.875 at DC, so no preamp: the impulse comes out as the response.
        assert_eq!(samples, [0.5, 0.0, 0.25, 0.5, 0.125, 0.25, 0.0, 0.125]);
        dsp.reset();
        let mut again = vec![1.0, 0.0];
        dsp.process(&mut again);
        assert_eq!(again, [0.5, 0.0]);
    }

    #[test]
    fn the_volume_ramps_and_its_absence_is_a_bypass() {
        let mut dsp = Dsp::new(48_000, 2);
        dsp.set(Settings {
            order: DEFAULT_ORDER,
            volume_db: 0.0,
            ..Settings::default()
        });
        assert!(!dsp.active());
        let mut first = sine(1000.0, 48_000.0, 0.5, 0..4800);
        dsp.process(&mut first);
        dsp.set(Settings {
            order: DEFAULT_ORDER,
            volume_db: -20.0,
            ..Settings::default()
        });
        assert!(dsp.active());
        let mut second = sine(1000.0, 48_000.0, 0.5, 4800..9600);
        dsp.process(&mut second);
        // The fade has ended: a tenth of the level.
        let peak = second[1920..].iter().fold(0.0, |m: f64, v| m.max(v.abs()));
        assert!((peak - 0.05).abs() < 1e-3, "{peak}");
        let joined: Vec<f64> = first.iter().chain(&second).step_by(2).copied().collect();
        let largest_step = joined
            .windows(2)
            .map(|w| (w[1] - w[0]).abs())
            .fold(0.0, f64::max);
        assert!(largest_step < 0.1, "{largest_step}");
        // Back at 0 dB the chain fades back to a bypass.
        dsp.set(Settings {
            order: DEFAULT_ORDER,
            ..Settings::default()
        });
        let mut rest = sine(1000.0, 48_000.0, 0.5, 9600..14400);
        dsp.process(&mut rest);
        assert!(!dsp.active());
    }

    #[test]
    fn invalid_settings_are_recognized() {
        let mut settings = eq(&[]);
        assert!(settings.valid(48_000));
        settings.order = [NODE_EQ, NODE_EQ, NODE_CONVOLUTION];
        assert!(!settings.valid(48_000));
        settings.order = DEFAULT_ORDER;
        settings.volume_db = 0.5;
        assert!(!settings.valid(48_000));
        settings.volume_db = f64::NEG_INFINITY;
        assert!(settings.valid(48_000));
        settings.impulse = Some(Arc::new(ImpulseResponse::new(&[1.0], 1, 44_100).unwrap()));
        assert!(!settings.valid(48_000));
    }
}
