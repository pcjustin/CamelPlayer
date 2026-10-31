// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Convolution with an impulse response (docs/Plan.md §5.9), in 64-bit float and without latency.
//!
//! The first HEAD_TAPS taps are convolved directly. The rest are split into levels of uniformly partitioned
//! overlap-save FFT convolution whose blocks grow by GROWTH up to MAX_BLOCK. A level of block B starts at tap B, so the
//! input blocks it needs are complete when its output block begins, and every output sample lines up with its input
//! sample. An `ImpulseResponse` holds the partition spectra and is shared; a `Convolver` holds one stream's state.

use std::sync::Arc;

use crate::fft::{Complex, RealFft};

const HEAD_TAPS: usize = 128;
const GROWTH: usize = 8;
const MAX_BLOCK: usize = 16_384;
// The response summary for headroom: this many log-spaced frequencies, and each path's strongest peaks.
const SUMMARY_POINTS: usize = 4_096;
const SUMMARY_PEAKS: usize = 32;
// -120 dB: below this, a response's end is not played out after the last track.
const TAIL_FLOOR: f64 = 1e-6;

#[derive(Clone, Copy)]
struct Layout {
    head: usize,
    growth: usize,
    max_block: usize,
}

const LAYOUT: Layout = Layout {
    head: HEAD_TAPS,
    growth: GROWTH,
    max_block: MAX_BLOCK,
};

// Taps [block, block + parts * block) in `parts` partitions of `block` taps.
struct Level {
    block: usize,
    parts: usize,
    fft: RealFft,
}

fn levels(taps: usize, layout: Layout) -> Vec<Level> {
    let mut levels = Vec::new();
    let mut block = layout.head;
    while block < taps {
        let next = (block * layout.growth).min(layout.max_block);
        let end = if next > block { next.min(taps) } else { taps };
        levels.push(Level {
            block,
            parts: (end - block).div_ceil(block),
            fft: RealFft::new(2 * block),
        });
        if next == block {
            break;
        }
        block = next;
    }
    levels
}

// One input channel's contribution to one output channel.
struct Path {
    input: usize,
    output: usize,
    // The first taps, newest-first order reversed for a dot product with the oldest-first history.
    head: Vec<f64>,
    // Partition spectra by level, then part.
    parts: Vec<Vec<Vec<Complex>>>,
}

/// The complex response of each path at a set of frequencies, for the chain's headroom.
pub(crate) struct Summary {
    pub frequencies: Vec<f64>,
    // (input, output, response at each frequency)
    pub paths: Vec<(usize, usize, Vec<Complex>)>,
}

pub(crate) struct ImpulseResponse {
    rate: u32,
    // The last tap at or above TAIL_FLOOR of the peak: what follows the input's end and is still heard.
    tail: usize,
    layout: Layout,
    levels: Vec<Level>,
    paths: Vec<Path>,
    summary: Summary,
}

impl ImpulseResponse {
    /// `samples` interleaved: 1 channel for both outputs, 2 for left and right, or 4 for true stereo in the order
    /// left to left, left to right, right to left, right to left. None when the layout or a value is invalid.
    pub(crate) fn new(samples: &[f64], channels: usize, rate: u32) -> Option<Self> {
        Self::with_layout(samples, channels, rate, LAYOUT)
    }

    fn with_layout(samples: &[f64], channels: usize, rate: u32, layout: Layout) -> Option<Self> {
        if !matches!(channels, 1 | 2 | 4)
            || rate == 0
            || samples.is_empty()
            || !samples.len().is_multiple_of(channels)
            || samples.iter().any(|v| !v.is_finite())
        {
            return None;
        }
        let frames = samples.len() / channels;
        let channel =
            |c: usize| -> Vec<f64> { samples.iter().skip(c).step_by(channels).copied().collect() };
        let routes: Vec<(usize, usize, Vec<f64>)> = match channels {
            1 => {
                let h = channel(0);
                vec![(0, 0, h.clone()), (1, 1, h)]
            }
            2 => vec![(0, 0, channel(0)), (1, 1, channel(1))],
            _ => vec![
                (0, 0, channel(0)),
                (0, 1, channel(1)),
                (1, 0, channel(2)),
                (1, 1, channel(3)),
            ],
        };
        // Trailing silence costs work and changes nothing.
        let taps = routes
            .iter()
            .filter_map(|(_, _, h)| h.iter().rposition(|&v| v != 0.0))
            .max()
            .map_or(1, |last| last + 1)
            .min(frames);
        let peak = routes
            .iter()
            .flat_map(|(_, _, h)| &h[..taps])
            .fold(0.0, |m: f64, v| m.max(v.abs()));
        let tail = routes
            .iter()
            .filter_map(|(_, _, h)| h[..taps].iter().rposition(|v| v.abs() >= peak * TAIL_FLOOR))
            .max()
            .unwrap_or(0);
        let levels = levels(taps, layout);
        let paths = routes
            .iter()
            .map(|(input, output, h)| {
                let mut head = vec![0.0; layout.head];
                for (j, &tap) in h.iter().take(layout.head.min(taps)).enumerate() {
                    head[layout.head - 1 - j] = tap;
                }
                let parts = levels
                    .iter()
                    .map(|level| {
                        let b = level.block;
                        let mut padded = vec![0.0; 2 * b];
                        let mut scratch = vec![Complex::ZERO; b];
                        (0..level.parts)
                            .map(|p| {
                                let start = b + p * b;
                                padded.fill(0.0);
                                for (i, slot) in padded[..b].iter_mut().enumerate() {
                                    if let Some(&tap) =
                                        h.get(start + i).filter(|_| start + i < taps)
                                    {
                                        *slot = tap;
                                    }
                                }
                                let mut bins = vec![Complex::ZERO; b + 1];
                                level.fft.forward(&padded, &mut bins, &mut scratch);
                                bins
                            })
                            .collect()
                    })
                    .collect();
                Path {
                    input: *input,
                    output: *output,
                    head,
                    parts,
                }
            })
            .collect();
        let summary = summarize(&routes, taps, rate);
        Some(ImpulseResponse {
            rate,
            tail,
            layout,
            levels,
            paths,
            summary,
        })
    }

    pub(crate) fn rate(&self) -> u32 {
        self.rate
    }

    /// The frames after the input ends that still carry its response above -120 dB.
    pub(crate) fn tail_frames(&self) -> usize {
        self.tail
    }

    pub(crate) fn summary(&self) -> &Summary {
        &self.summary
    }
}

fn summarize(routes: &[(usize, usize, Vec<f64>)], taps: usize, rate: u32) -> Summary {
    let rate = f64::from(rate);
    // Zero padding samples the response finely enough even for a short response.
    let n = (2 * taps).next_power_of_two().max(1 << 16);
    let fft = RealFft::new(n);
    let mut padded = vec![0.0; n];
    let mut scratch = vec![Complex::ZERO; n / 2];
    let spectra: Vec<Vec<Complex>> = routes
        .iter()
        .map(|(_, _, h)| {
            padded.fill(0.0);
            padded[..taps].copy_from_slice(&h[..taps]);
            let mut bins = vec![Complex::ZERO; n / 2 + 1];
            fft.forward(&padded, &mut bins, &mut scratch);
            bins
        })
        .collect();
    let nyquist = rate / 2.0;
    let (low, high) = (10.0f64.min(nyquist / 2.0), nyquist * 0.999);
    let mut frequencies: Vec<f64> = (0..SUMMARY_POINTS)
        .map(|i| low * (high / low).powf(i as f64 / (SUMMARY_POINTS - 1) as f64))
        .collect();
    frequencies.push(0.0);
    for bins in &spectra {
        let mut peaks: Vec<usize> = (1..bins.len() - 1)
            .filter(|&k| {
                bins[k].norm() >= bins[k - 1].norm() && bins[k].norm() >= bins[k + 1].norm()
            })
            .collect();
        peaks.sort_by(|&a, &b| bins[b].norm().total_cmp(&bins[a].norm()));
        frequencies.extend(
            peaks
                .iter()
                .take(SUMMARY_PEAKS)
                .map(|&k| k as f64 * rate / n as f64),
        );
    }
    frequencies.sort_by(f64::total_cmp);
    frequencies.dedup();
    let bin = |f: f64| ((f * n as f64 / rate).round() as usize).min(n / 2);
    Summary {
        paths: routes
            .iter()
            .zip(&spectra)
            .map(|((input, output, _), bins)| {
                (
                    *input,
                    *output,
                    frequencies.iter().map(|&f| bins[bin(f)]).collect(),
                )
            })
            .collect(),
        frequencies,
    }
}

struct LevelState {
    // Per input channel: the previous input block, then the one being filled.
    windows: Vec<Vec<f64>>,
    // Per input channel: the spectra of the latest input blocks, block k at k % ring.
    spectra: Vec<Vec<Vec<Complex>>>,
    ring: usize,
    // Per output channel: this level's part of the current output block.
    out: Vec<Vec<f64>>,
    acc: Vec<Complex>,
    time: Vec<f64>,
    scratch: Vec<Complex>,
}

/// One stream's convolution state over a shared impulse response.
pub(crate) struct Convolver {
    ir: Arc<ImpulseResponse>,
    inputs: usize,
    outputs: usize,
    frame: u64,
    // Per input channel: the last `head` samples twice over, so they read as one contiguous slice.
    history: Vec<Vec<f64>>,
    history_at: usize,
    levels: Vec<LevelState>,
}

impl Convolver {
    pub(crate) fn new(ir: Arc<ImpulseResponse>) -> Self {
        let inputs = ir.paths.iter().map(|p| p.input + 1).max().unwrap_or(1);
        let outputs = ir.paths.iter().map(|p| p.output + 1).max().unwrap_or(1);
        let levels = ir
            .levels
            .iter()
            .map(|level| {
                let b = level.block;
                let ring = 1 + level.parts;
                LevelState {
                    windows: vec![vec![0.0; 2 * b]; inputs],
                    spectra: vec![vec![vec![Complex::ZERO; b + 1]; ring]; inputs],
                    ring,
                    out: vec![vec![0.0; b]; outputs],
                    acc: vec![Complex::ZERO; b + 1],
                    time: vec![0.0; 2 * b],
                    scratch: vec![Complex::ZERO; b],
                }
            })
            .collect();
        Convolver {
            history: vec![vec![0.0; 2 * ir.layout.head]; inputs],
            history_at: 0,
            ir,
            inputs,
            outputs,
            frame: 0,
            levels,
        }
    }

    pub(crate) fn impulse_response(&self) -> &Arc<ImpulseResponse> {
        &self.ir
    }

    /// Forgets the input so far; the next frame starts a new stream.
    pub(crate) fn reset(&mut self) {
        self.frame = 0;
        for h in &mut self.history {
            h.fill(0.0);
        }
        for level in &mut self.levels {
            for w in &mut level.windows {
                w.fill(0.0);
            }
        }
    }

    // Starts output block `m` of level `l`: stores the input block just completed and sums the partitions.
    fn begin_block(&mut self, l: usize, m: u64) {
        let ir = &*self.ir;
        let level = &ir.levels[l];
        let state = &mut self.levels[l];
        let b = level.block;
        if m > 0 {
            let slot = ((m - 1) % state.ring as u64) as usize;
            for c in 0..self.inputs {
                level.fft.forward(
                    &state.windows[c],
                    &mut state.spectra[c][slot],
                    &mut state.scratch,
                );
                state.windows[c].copy_within(b.., 0);
            }
        }
        for o in 0..self.outputs {
            state.acc.fill(Complex::ZERO);
            let mut any = false;
            for path in ir.paths.iter().filter(|p| p.output == o) {
                for (p, part) in path.parts[l].iter().enumerate() {
                    // The block `part` meets: its first tap is 1 + p blocks back.
                    let Some(q) = m.checked_sub(1 + p as u64) else {
                        break;
                    };
                    let x = &state.spectra[path.input][(q % state.ring as u64) as usize];
                    for ((a, &h), &v) in state.acc.iter_mut().zip(part).zip(x) {
                        *a += h * v;
                    }
                    any = true;
                }
            }
            if any {
                level
                    .fft
                    .inverse(&state.acc, &mut state.time, &mut state.scratch);
                state.out[o].copy_from_slice(&state.time[b..]);
            } else {
                state.out[o].fill(0.0);
            }
        }
    }

    /// Convolves interleaved frames of `channels` in place; channels the response does not reach pass unchanged.
    pub(crate) fn process(&mut self, samples: &mut [f64], channels: usize) {
        let head = self.ir.layout.head;
        let mut input = [0.0; 2];
        for frame in samples.chunks_exact_mut(channels) {
            for l in 0..self.levels.len() {
                let b = self.ir.levels[l].block as u64;
                if self.frame.is_multiple_of(b) {
                    self.begin_block(l, self.frame / b);
                }
            }
            for (c, x) in input.iter_mut().enumerate().take(self.inputs) {
                *x = frame.get(c).copied().unwrap_or(0.0);
            }
            self.history_at = (self.history_at + 1) % head;
            for (c, h) in self.history.iter_mut().enumerate() {
                h[self.history_at] = input[c];
                h[self.history_at + head] = input[c];
            }
            for (l, state) in self.levels.iter_mut().enumerate() {
                let b = self.ir.levels[l].block;
                let i = (self.frame % b as u64) as usize;
                for (c, w) in state.windows.iter_mut().enumerate() {
                    w[b + i] = input[c];
                }
            }
            for (o, out) in frame.iter_mut().enumerate().take(self.outputs) {
                let mut y = 0.0;
                for path in self.ir.paths.iter().filter(|p| p.output == o) {
                    let recent =
                        &self.history[path.input][self.history_at + 1..=self.history_at + head];
                    y += path
                        .head
                        .iter()
                        .zip(recent)
                        .map(|(h, x)| h * x)
                        .sum::<f64>();
                }
                for (l, state) in self.levels.iter().enumerate() {
                    let b = self.ir.levels[l].block as u64;
                    y += state.out[o][(self.frame % b) as usize];
                }
                *out = y;
            }
            self.frame += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Convolver, ImpulseResponse, Layout};
    use std::sync::Arc;

    fn noise(n: usize, seed: u64) -> Vec<f64> {
        let mut state = seed;
        (0..n)
            .map(|_| {
                state = state
                    .wrapping_mul(6_364_136_223_846_793_005)
                    .wrapping_add(1_442_695_040_888_963_407);
                (state >> 11) as f64 / (1u64 << 53) as f64 - 0.5
            })
            .collect()
    }

    fn direct(signal: &[f64], h: &[f64]) -> Vec<f64> {
        (0..signal.len())
            .map(|n| (0..h.len().min(n + 1)).map(|j| h[j] * signal[n - j]).sum())
            .collect()
    }

    // Runs `ir` over stereo `left` and `right` in chunks of varying size.
    fn run(ir: ImpulseResponse, left: &[f64], right: &[f64]) -> (Vec<f64>, Vec<f64>) {
        let mut convolver = Convolver::new(Arc::new(ir));
        let mut frames: Vec<f64> = left.iter().zip(right).flat_map(|(&l, &r)| [l, r]).collect();
        let sizes = [1, 7, 64, 3, 250, 1, 999];
        let mut at = 0;
        for &size in sizes.iter().cycle() {
            if at >= left.len() {
                break;
            }
            let end = (at + size).min(left.len());
            convolver.process(&mut frames[2 * at..2 * end], 2);
            at = end;
        }
        (
            frames.iter().step_by(2).copied().collect(),
            frames.iter().skip(1).step_by(2).copied().collect(),
        )
    }

    fn assert_close(got: &[f64], want: &[f64]) {
        for (n, (g, w)) in got.iter().zip(want).enumerate() {
            assert!(
                (g - w).abs() <= 1e-9 * (1.0 + w.abs()),
                "sample {n}: {g} vs {w}"
            );
        }
    }

    const SMALL: Layout = Layout {
        head: 4,
        growth: 2,
        max_block: 32,
    };

    #[test]
    fn every_partition_layout_matches_direct_convolution_without_latency() {
        let left = noise(3_000, 1);
        let right = noise(3_000, 2);
        for taps in [1, 3, 4, 5, 8, 33, 64, 100, 257, 1_000] {
            let h = noise(taps, taps as u64 + 10);
            let ir = ImpulseResponse::with_layout(&h, 1, 48_000, SMALL).unwrap();
            let (l, r) = run(ir, &left, &right);
            assert_close(&l, &direct(&left, &h));
            assert_close(&r, &direct(&right, &h));
        }
    }

    #[test]
    fn the_default_layout_matches_a_long_response() {
        let left = noise(30_000, 3);
        let h = noise(17_000, 4);
        let ir = ImpulseResponse::new(&h, 1, 44_100).unwrap();
        let (l, _) = run(ir, &left, &left);
        assert_close(&l, &direct(&left, &h));
    }

    #[test]
    fn stereo_and_true_stereo_route_each_path() {
        let left = noise(2_000, 5);
        let right = noise(2_000, 6);
        let [ll, lr, rl, rr] = [noise(300, 7), noise(300, 8), noise(300, 9), noise(300, 10)];
        let stereo: Vec<f64> = ll.iter().zip(&rr).flat_map(|(&a, &b)| [a, b]).collect();
        let (l, r) = run(
            ImpulseResponse::with_layout(&stereo, 2, 48_000, SMALL).unwrap(),
            &left,
            &right,
        );
        assert_close(&l, &direct(&left, &ll));
        assert_close(&r, &direct(&right, &rr));

        let four: Vec<f64> = (0..300)
            .flat_map(|i| [ll[i], lr[i], rl[i], rr[i]])
            .collect();
        let (l, r) = run(
            ImpulseResponse::with_layout(&four, 4, 48_000, SMALL).unwrap(),
            &left,
            &right,
        );
        let sum = |a: Vec<f64>, b: Vec<f64>| -> Vec<f64> {
            a.iter().zip(&b).map(|(x, y)| x + y).collect()
        };
        assert_close(&l, &sum(direct(&left, &ll), direct(&right, &rl)));
        assert_close(&r, &sum(direct(&left, &lr), direct(&right, &rr)));
    }

    #[test]
    fn a_reset_starts_a_new_stream() {
        let h = noise(200, 11);
        let signal = noise(500, 12);
        let mut convolver = Convolver::new(Arc::new(
            ImpulseResponse::with_layout(&h, 1, 48_000, SMALL).unwrap(),
        ));
        let mut first: Vec<f64> = signal.iter().flat_map(|&v| [v, v]).collect();
        convolver.process(&mut first, 2);
        convolver.reset();
        let mut second: Vec<f64> = signal.iter().flat_map(|&v| [v, v]).collect();
        convolver.process(&mut second, 2);
        assert_close(&second, &first);
    }

    #[test]
    fn trailing_silence_is_trimmed_and_bad_layouts_are_refused() {
        let mut h = noise(50, 13);
        h.extend(std::iter::repeat_n(0.0, 1_000));
        h[49] = 1.0;
        let ir = ImpulseResponse::new(&h, 1, 48_000).unwrap();
        assert_eq!(ir.tail_frames(), 49);
        let decaying = ImpulseResponse::new(&[1.0, 0.5, 1e-8, 1e-9], 1, 48_000).unwrap();
        assert_eq!(decaying.tail_frames(), 1);
        assert!(ImpulseResponse::new(&h, 3, 48_000).is_none());
        assert!(ImpulseResponse::new(&[f64::NAN], 1, 48_000).is_none());
        assert!(ImpulseResponse::new(&[], 1, 48_000).is_none());
    }

    #[test]
    fn the_summary_holds_the_response_at_its_frequencies() {
        // A two-tap average: |H(f)| = |cos(πf/rate)|.
        let ir = ImpulseResponse::new(&[0.5, 0.5], 1, 48_000).unwrap();
        let summary = ir.summary();
        for (i, &f) in summary.frequencies.iter().enumerate() {
            let want = (std::f64::consts::PI * f / 48_000.0).cos().abs();
            assert!((summary.paths[0].2[i].norm() - want).abs() < 1e-3, "{f} Hz");
        }
    }
}
