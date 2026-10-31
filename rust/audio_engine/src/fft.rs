// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! 64-bit float FFTs of power-of-two sizes for the convolver: an iterative radix-2 complex transform and a real
//! transform of twice its size built on it. Plans are made once; transforms never allocate.

use std::f64::consts::PI;
use std::ops::{Add, AddAssign, Mul, Sub};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Complex {
    pub re: f64,
    pub im: f64,
}

impl Complex {
    pub(crate) const ZERO: Complex = Complex { re: 0.0, im: 0.0 };

    pub(crate) fn new(re: f64, im: f64) -> Self {
        Complex { re, im }
    }

    // e^(i·angle)
    pub(crate) fn unit(angle: f64) -> Self {
        let (sin, cos) = angle.sin_cos();
        Complex::new(cos, sin)
    }

    pub(crate) fn conj(self) -> Self {
        Complex::new(self.re, -self.im)
    }

    pub(crate) fn norm(self) -> f64 {
        self.re.hypot(self.im)
    }

    pub(crate) fn scale(self, factor: f64) -> Self {
        Complex::new(self.re * factor, self.im * factor)
    }

    pub(crate) fn div(self, other: Complex) -> Self {
        let d = other.re * other.re + other.im * other.im;
        Complex::new(
            (self.re * other.re + self.im * other.im) / d,
            (self.im * other.re - self.re * other.im) / d,
        )
    }
}

impl Add for Complex {
    type Output = Complex;
    fn add(self, other: Complex) -> Complex {
        Complex::new(self.re + other.re, self.im + other.im)
    }
}

impl AddAssign for Complex {
    fn add_assign(&mut self, other: Complex) {
        self.re += other.re;
        self.im += other.im;
    }
}

impl Sub for Complex {
    type Output = Complex;
    fn sub(self, other: Complex) -> Complex {
        Complex::new(self.re - other.re, self.im - other.im)
    }
}

impl Mul for Complex {
    type Output = Complex;
    fn mul(self, other: Complex) -> Complex {
        Complex::new(
            self.re * other.re - self.im * other.im,
            self.re * other.im + self.im * other.re,
        )
    }
}

// A complex FFT of `n` points, n a power of two.
struct ComplexFft {
    n: usize,
    // e^(-2πik/n) for k < n/2.
    twiddles: Vec<Complex>,
    reversed: Vec<usize>,
}

impl ComplexFft {
    fn new(n: usize) -> Self {
        assert!(n.is_power_of_two());
        let bits = n.trailing_zeros();
        ComplexFft {
            n,
            twiddles: (0..n / 2)
                .map(|k| Complex::unit(-2.0 * PI * k as f64 / n as f64))
                .collect(),
            reversed: (0..n)
                .map(|i| {
                    if bits == 0 {
                        0
                    } else {
                        i.reverse_bits() >> (usize::BITS - bits)
                    }
                })
                .collect(),
        }
    }

    fn forward(&self, data: &mut [Complex]) {
        let n = self.n;
        for i in 0..n {
            let j = self.reversed[i];
            if i < j {
                data.swap(i, j);
            }
        }
        let mut size = 2;
        while size <= n {
            let half = size / 2;
            let step = n / size;
            for start in (0..n).step_by(size) {
                for k in 0..half {
                    let w = self.twiddles[k * step];
                    let a = data[start + k];
                    let b = data[start + k + half] * w;
                    data[start + k] = a + b;
                    data[start + k + half] = a - b;
                }
            }
            size *= 2;
        }
    }
}

/// A real FFT of `n` points (a power of two, at least 2) giving `n / 2 + 1` bins, through a complex FFT of `n / 2`.
pub(crate) struct RealFft {
    n: usize,
    half: ComplexFft,
    // e^(-2πik/n) for k < n/2.
    twiddles: Vec<Complex>,
}

impl RealFft {
    pub(crate) fn new(n: usize) -> Self {
        assert!(n.is_power_of_two() && n >= 2);
        RealFft {
            n,
            half: ComplexFft::new(n / 2),
            twiddles: (0..n / 2)
                .map(|k| Complex::unit(-2.0 * PI * k as f64 / n as f64))
                .collect(),
        }
    }

    /// `input` (n samples) to `output` (n/2 + 1 bins); `scratch` holds n/2 values.
    pub(crate) fn forward(&self, input: &[f64], output: &mut [Complex], scratch: &mut [Complex]) {
        let m = self.n / 2;
        for (k, z) in scratch[..m].iter_mut().enumerate() {
            *z = Complex::new(input[2 * k], input[2 * k + 1]);
        }
        self.half.forward(&mut scratch[..m]);
        for k in 0..=m {
            let zk = scratch[k % m];
            let zc = scratch[(m - k) % m].conj();
            let even = (zk + zc).scale(0.5);
            let odd = (zk - zc) * Complex::new(0.0, -0.5);
            let w = if k < m {
                self.twiddles[k]
            } else {
                Complex::new(-1.0, 0.0)
            };
            output[k] = even + w * odd;
        }
    }

    /// `input` (n/2 + 1 bins) back to `output` (n samples), scaled by 1/n so it inverts `forward`; `scratch` holds
    /// n/2 values.
    pub(crate) fn inverse(&self, input: &[Complex], output: &mut [f64], scratch: &mut [Complex]) {
        let m = self.n / 2;
        for k in 0..m {
            let a = input[k];
            let b = input[m - k].conj();
            let even = (a + b).scale(0.5);
            let odd = ((a - b) * self.twiddles[k].conj()).scale(0.5);
            // The inverse of a forward transform by conjugation: conj(FFT(conj(Z))).
            scratch[k] = (even + Complex::new(-odd.im, odd.re)).conj();
        }
        self.half.forward(&mut scratch[..m]);
        let scale = 1.0 / m as f64;
        for k in 0..m {
            let z = scratch[k].conj().scale(scale);
            output[2 * k] = z.re;
            output[2 * k + 1] = z.im;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Complex, RealFft};
    use std::f64::consts::PI;

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

    fn dft(input: &[f64]) -> Vec<Complex> {
        let n = input.len();
        (0..=n / 2)
            .map(|k| {
                input
                    .iter()
                    .enumerate()
                    .fold(Complex::ZERO, |sum, (t, &x)| {
                        sum + Complex::unit(-2.0 * PI * (k * t) as f64 / n as f64).scale(x)
                    })
            })
            .collect()
    }

    #[test]
    fn the_real_transform_matches_the_definition_and_inverts() {
        for n in [2, 4, 8, 64, 256, 1024] {
            let fft = RealFft::new(n);
            let input = noise(n, n as u64);
            let mut bins = vec![Complex::ZERO; n / 2 + 1];
            let mut scratch = vec![Complex::ZERO; n / 2];
            fft.forward(&input, &mut bins, &mut scratch);
            for (got, want) in bins.iter().zip(dft(&input)) {
                assert!((*got - want).norm() < 1e-9 * n as f64, "n {n}");
            }
            let mut back = vec![0.0; n];
            fft.inverse(&bins, &mut back, &mut scratch);
            for (got, want) in back.iter().zip(&input) {
                assert!((got - want).abs() < 1e-12, "n {n}");
            }
        }
    }
}
