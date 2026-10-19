//! DSD to PCM (docs/Plan.md §5.8): multistage FIR decimation to 352.8 kHz in the manner of dsd2pcm.
//!
//! The first stage filters the 1-bit stream and keeps one value per DSD byte (decimation by 8), summing precomputed
//! per-byte tables instead of multiplying bits. Each further stage halves the rate until 352.8 kHz. Every stage is a
//! linear-phase Kaiser-windowed sinc with unity DC gain whose stopband covers everything that would alias into the
//! protected band, so 100% DSD modulation maps to PCM full scale.

/// The rate every DSD rate is decimated to; soxr takes it to the configured output rate.
pub(crate) const PCM_RATE: u32 = 352_800;
/// The band kept free of aliases and flat.
pub(crate) const PASSBAND_HZ: f64 = 40_000.0;
/// Attenuation of everything that aliases into the passband.
pub(crate) const STOPBAND_DB: f64 = 120.0;
// Kaiser's length estimate is approximate; designing for more keeps every stage clear of STOPBAND_DB.
const DESIGN_DB: f64 = STOPBAND_DB + 10.0;

fn bessel_i0(x: f64) -> f64 {
    let mut sum = 1.0;
    let mut term = 1.0;
    for k in 1..64 {
        term *= (x / (2.0 * k as f64)).powi(2);
        sum += term;
        if term < sum * 1e-17 {
            break;
        }
    }
    sum
}

/// A lowpass for decimating from `rate_in` to `rate_out`: flat to PASSBAND_HZ, STOPBAND_DB from `rate_out` -
/// PASSBAND_HZ, length a multiple of `multiple`, taps summing to 1.
pub(crate) fn design_lowpass(rate_in: f64, rate_out: f64, multiple: usize) -> Vec<f64> {
    let transition = (rate_out - 2.0 * PASSBAND_HZ) / rate_in;
    let estimate = (DESIGN_DB - 8.0) / (2.285 * 2.0 * std::f64::consts::PI * transition);
    let taps = (estimate.ceil() as usize + 1).div_ceil(multiple) * multiple;
    let cutoff = rate_out / 2.0 / rate_in;
    let beta = 0.1102 * (DESIGN_DB - 8.7);
    let centre = (taps - 1) as f64 / 2.0;
    let mut h: Vec<f64> = (0..taps)
        .map(|n| {
            let t = n as f64 - centre;
            let sinc = if t == 0.0 {
                2.0 * cutoff
            } else {
                (2.0 * std::f64::consts::PI * cutoff * t).sin() / (std::f64::consts::PI * t)
            };
            let r = t / centre.max(1.0);
            sinc * bessel_i0(beta * (1.0 - r * r).max(0.0).sqrt()) / bessel_i0(beta)
        })
        .collect();
    let sum: f64 = h.iter().sum();
    h.iter_mut().for_each(|v| *v /= sum);
    h
}

/// Magnitude of the response of `taps` at `frequency` (both relative to the filter's input rate).
#[cfg(test)]
pub(crate) fn response(taps: &[f64], frequency: f64) -> f64 {
    let w = 2.0 * std::f64::consts::PI * frequency;
    let (re, im) = taps
        .iter()
        .enumerate()
        .fold((0.0, 0.0), |(re, im), (n, &h)| {
            (re + h * (w * n as f64).cos(), im - h * (w * n as f64).sin())
        });
    (re * re + im * im).sqrt()
}

struct HalfStage {
    taps: Vec<f64>,
    history: Vec<Vec<f64>>, // per channel, oldest first
    odd: bool,
}

pub(crate) struct DsdDecimator {
    channels: usize,
    tables: Vec<[f64; 256]>, // stage 1, one table per byte of the filter span, oldest byte first
    bytes: Vec<Vec<u8>>,     // per channel, the bytes under the stage-1 filter, oldest first
    halves: Vec<HalfStage>,
    frame: Vec<f64>,
}

impl DsdDecimator {
    /// For a DSD stream of `byte_rate` byte frames per second (DSD rate / 8); DSD64 to DSD512.
    pub(crate) fn new(byte_rate: u32, channels: usize) -> Option<Self> {
        let halvings = match byte_rate / PCM_RATE {
            1 => 0,
            2 => 1,
            4 => 2,
            8 => 3,
            _ => return None,
        };
        if !byte_rate.is_multiple_of(PCM_RATE) || channels == 0 {
            return None;
        }
        let stage1 = design_lowpass(f64::from(byte_rate) * 8.0, f64::from(byte_rate), 8);
        let tables = stage1
            .as_chunks::<8>()
            .0
            .iter()
            .map(|bits| {
                let mut table = [0.0; 256];
                for (value, entry) in table.iter_mut().enumerate() {
                    // The oldest sample is the most significant bit; a set bit is +1, a clear one -1.
                    *entry = bits
                        .iter()
                        .enumerate()
                        .map(|(j, &h)| if value & (0x80 >> j) != 0 { h } else { -h })
                        .sum();
                }
                table
            })
            .collect::<Vec<_>>();
        let span = tables.len();
        let mut rate = f64::from(byte_rate);
        let halves = (0..halvings)
            .map(|_| {
                let taps = design_lowpass(rate, rate / 2.0, 1);
                rate /= 2.0;
                HalfStage {
                    history: vec![vec![0.0; taps.len()]; channels],
                    taps,
                    odd: false,
                }
            })
            .collect();
        Some(Self {
            channels,
            tables,
            // Idle pattern before the first byte, so the stream starts from silence.
            bytes: vec![vec![crate::dsd::DSD_SILENCE; span]; channels],
            halves,
            frame: vec![0.0; channels],
        })
    }

    /// Filters `frames` byte frames (`input` interleaved, one byte in the low bits of each value) and appends the
    /// 352.8 kHz frames they complete to `output`, interleaved.
    pub(crate) fn process(&mut self, input: &[i32], frames: usize, output: &mut Vec<f64>) {
        for frame in input[..frames * self.channels].chunks_exact(self.channels) {
            for (c, &byte) in frame.iter().enumerate() {
                let bytes = &mut self.bytes[c];
                bytes.copy_within(1.., 0);
                *bytes.last_mut().expect("the filter spans bytes") = byte as u8;
                self.frame[c] = self
                    .tables
                    .iter()
                    .zip(bytes.iter())
                    .map(|(table, &b)| table[usize::from(b)])
                    .sum();
            }
            if self.halve() {
                output.extend_from_slice(&self.frame);
            }
        }
    }

    // Runs `frame` through the 2x stages; true when the last one produced an output frame.
    fn halve(&mut self) -> bool {
        for stage in &mut self.halves {
            for (c, value) in self.frame.iter_mut().enumerate() {
                let history = &mut stage.history[c];
                history.copy_within(1.., 0);
                *history.last_mut().expect("stages have taps") = *value;
                *value = history.iter().zip(&stage.taps).map(|(x, h)| x * h).sum();
            }
            stage.odd = !stage.odd;
            if stage.odd {
                return false;
            }
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db(x: f64) -> f64 {
        20.0 * x.log10()
    }

    // Every stage: unity DC gain, flat passband, STOPBAND_DB over each band that aliases into the passband.
    #[test]
    fn stages_are_flat_and_reject_aliases() {
        for dsd in [1u32, 2, 4, 8] {
            let mut rate_in = f64::from(dsd * PCM_RATE) * 8.0;
            let mut rate_out = f64::from(dsd * PCM_RATE);
            let mut multiple = 8; // the byte-table stage, then the 2x stages
            while rate_out >= f64::from(PCM_RATE) {
                let taps = design_lowpass(rate_in, rate_out, multiple);
                multiple = 1;
                assert!((taps.iter().sum::<f64>() - 1.0).abs() < 1e-12);
                for i in 0..=40 {
                    let f = PASSBAND_HZ * f64::from(i) / 40.0 / rate_in;
                    assert!(
                        db(response(&taps, f)).abs() < 0.001,
                        "DSD{} {rate_in}: {f}",
                        dsd * 64
                    );
                }
                // Images of the passband around every multiple of the output rate up to the input Nyquist.
                let mut image = rate_out;
                while image - PASSBAND_HZ < rate_in / 2.0 {
                    for i in 0..=40 {
                        let f = image - PASSBAND_HZ + 2.0 * PASSBAND_HZ * f64::from(i) / 40.0;
                        if f <= rate_in / 2.0 {
                            let attenuation = db(response(&taps, f / rate_in));
                            assert!(
                                attenuation < -STOPBAND_DB,
                                "DSD{} at {f} Hz: {attenuation} dB",
                                dsd * 64
                            );
                        }
                    }
                    image += rate_out;
                }
                rate_in = rate_out;
                rate_out /= 2.0;
            }
        }
    }

    #[test]
    fn rejects_other_rates() {
        assert!(DsdDecimator::new(PCM_RATE * 3, 2).is_none());
        assert!(DsdDecimator::new(PCM_RATE * 16, 2).is_none());
        assert!(DsdDecimator::new(44_100, 2).is_none());
        assert_eq!(DsdDecimator::new(PCM_RATE * 8, 2).unwrap().halves.len(), 3);
    }
}
