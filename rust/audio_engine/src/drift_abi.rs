// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! A resampler whose ratio follows another device's clock (docs/Plan.md §5.12): a group's member that is not the clock
//! plays through it, its ratio moved a few ppm around 1 as its drift is measured, through libsoxr's variable-rate mode.

use std::ffi::{c_char, c_uint, c_ulong, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::{ENGINE_E_INVALID_ARGUMENT, ENGINE_E_PANIC, ENGINE_OK};

#[repr(C)]
#[derive(Clone, Copy)]
struct SoxrIoSpec {
    itype: c_uint,
    otype: c_uint,
    scale: f64,
    e: *mut c_void,
    flags: c_ulong,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SoxrQualitySpec {
    precision: f64,
    phase_response: f64,
    passband_end: f64,
    stopband_begin: f64,
    e: *mut c_void,
    flags: c_ulong,
}

const SOXR_FLOAT64_I: c_uint = 1;
const SOXR_HQ: c_ulong = 4;
const SOXR_VR: c_ulong = 32;
/// The largest input-to-output ratio asked for: drift is a few hundred ppm at most.
const MAX_RATIO: f64 = 1.05;

extern "C" {
    fn soxr_create(
        input_rate: f64,
        output_rate: f64,
        num_channels: c_uint,
        error: *mut *const c_char,
        io_spec: *const SoxrIoSpec,
        quality_spec: *const SoxrQualitySpec,
        runtime_spec: *const c_void,
    ) -> *mut c_void;
    fn soxr_process(
        resampler: *mut c_void,
        input: *const c_void,
        ilen: usize,
        idone: *mut usize,
        output: *mut c_void,
        olen: usize,
        odone: *mut usize,
    ) -> *const c_char;
    fn soxr_set_io_ratio(resampler: *mut c_void, io_ratio: f64, slew_len: usize) -> *const c_char;
    fn soxr_delay(resampler: *mut c_void) -> f64;
    fn soxr_delete(resampler: *mut c_void);
    fn soxr_quality_spec(recipe: c_ulong, flags: c_ulong) -> SoxrQualitySpec;
    fn soxr_io_spec(itype: c_uint, otype: c_uint) -> SoxrIoSpec;
}

pub struct CamelDriftResampler {
    handle: *mut c_void,
}

/// A resampler of `channels` interleaved 64-bit float channels, at a ratio of 1 until told otherwise; null when soxr
/// cannot make one.
#[no_mangle]
pub extern "C" fn camel_drift_resampler_create_v1(channels: u32) -> *mut CamelDriftResampler {
    catch_unwind(|| {
        if channels == 0 {
            return std::ptr::null_mut();
        }
        let mut error: *const c_char = std::ptr::null();
        let handle = unsafe {
            let io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
            let quality = soxr_quality_spec(SOXR_HQ, SOXR_VR);
            soxr_create(
                MAX_RATIO,
                1.0,
                channels as c_uint,
                &mut error,
                &io,
                &quality,
                std::ptr::null(),
            )
        };
        if handle.is_null() || !error.is_null() {
            if !handle.is_null() {
                unsafe { soxr_delete(handle) };
            }
            return std::ptr::null_mut();
        }
        unsafe { soxr_set_io_ratio(handle, 1.0, 0) };
        Box::into_raw(Box::new(CamelDriftResampler { handle }))
    })
    .unwrap_or(std::ptr::null_mut())
}

/// Input frames per output frame from now on, reached gradually over `slew_frames` output frames: above 1 the
/// follower catches up, below 1 it falls back.
///
/// # Safety
/// `resampler` comes from `camel_drift_resampler_create_v1`.
#[no_mangle]
pub unsafe extern "C" fn camel_drift_resampler_set_ratio_v1(
    resampler: *mut CamelDriftResampler,
    ratio: f64,
    slew_frames: usize,
) -> i32 {
    if resampler.is_null() || !(ratio > 1.0 / MAX_RATIO && ratio < MAX_RATIO) {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    if soxr_set_io_ratio((*resampler).handle, ratio, slew_frames).is_null() {
        ENGINE_OK
    } else {
        ENGINE_E_INVALID_ARGUMENT
    }
}

/// Resamples up to `frames` interleaved input frames into `output`, which holds `capacity` frames; `used` receives the
/// input frames taken and `produced` the output frames written.
///
/// # Safety
/// `resampler` comes from `camel_drift_resampler_create_v1`; `input` holds `frames` and `output` `capacity` frames of its
/// channels.
#[no_mangle]
pub unsafe extern "C" fn camel_drift_resampler_process_v1(
    resampler: *mut CamelDriftResampler,
    input: *const f64,
    frames: usize,
    used: *mut usize,
    output: *mut f64,
    capacity: usize,
    produced: *mut usize,
) -> i32 {
    if resampler.is_null()
        || used.is_null()
        || produced.is_null()
        || (frames > 0 && input.is_null())
        || (capacity > 0 && output.is_null())
    {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    *used = 0;
    *produced = 0;
    if frames == 0 || capacity == 0 {
        return ENGINE_OK;
    }
    let resampler = &mut *resampler;
    match catch_unwind(AssertUnwindSafe(|| {
        let (mut taken, mut written) = (0usize, 0usize);
        let error = soxr_process(
            resampler.handle,
            input.cast(),
            frames,
            &mut taken,
            output.cast(),
            capacity,
            &mut written,
        );
        (error.is_null(), taken, written)
    })) {
        Ok((true, taken, written)) => {
            *used = taken;
            *produced = written;
            ENGINE_OK
        }
        Ok((false, _, _)) => ENGINE_E_INVALID_ARGUMENT,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// Output frames the resampler holds that its input has given but it has not written yet.
///
/// # Safety
/// `resampler` comes from `camel_drift_resampler_create_v1`.
#[no_mangle]
pub unsafe extern "C" fn camel_drift_resampler_delay_v1(
    resampler: *mut CamelDriftResampler,
) -> f64 {
    if resampler.is_null() {
        return 0.0;
    }
    soxr_delay((*resampler).handle)
}

/// # Safety
/// `resampler` comes from `camel_drift_resampler_create_v1` and is not used again.
#[no_mangle]
pub unsafe extern "C" fn camel_drift_resampler_destroy_v1(resampler: *mut CamelDriftResampler) {
    if !resampler.is_null() {
        let resampler = Box::from_raw(resampler);
        soxr_delete(resampler.handle);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Feeds `seconds` of a 1 kHz tone at 44.1 kHz through the resampler at `ratio`, in blocks of 441 frames; returns
    // the input frames taken and the output frames written.
    fn run(ratio: f64, seconds: usize, channels: usize) -> (usize, usize, Vec<f64>) {
        let resampler = camel_drift_resampler_create_v1(channels as u32);
        assert!(!resampler.is_null());
        assert_eq!(
            unsafe { camel_drift_resampler_set_ratio_v1(resampler, ratio, 0) },
            ENGINE_OK
        );
        let (mut taken, mut written) = (0usize, 0usize);
        let mut out = Vec::new();
        let mut frame = 0usize;
        let total = seconds * 44_100;
        let mut block = vec![0.0f64; 441 * channels];
        let mut output = vec![0.0f64; 1024 * channels];
        while frame < total {
            for (i, sample) in block.chunks_mut(channels).enumerate() {
                let t = (frame + i) as f64 / 44_100.0;
                sample.fill((2.0 * std::f64::consts::PI * 1000.0 * t).sin() * 0.5);
            }
            let mut offset = 0;
            while offset < 441 {
                let (mut used, mut produced) = (0usize, 0usize);
                let status = unsafe {
                    camel_drift_resampler_process_v1(
                        resampler,
                        block[offset * channels..].as_ptr(),
                        441 - offset,
                        &mut used,
                        output.as_mut_ptr(),
                        1024,
                        &mut produced,
                    )
                };
                assert_eq!(status, ENGINE_OK);
                offset += used;
                taken += used;
                written += produced;
                out.extend_from_slice(&output[..produced * channels]);
                if used == 0 && produced == 0 {
                    break;
                }
            }
            frame += 441;
        }
        unsafe { camel_drift_resampler_destroy_v1(resampler) };
        (taken, written, out)
    }

    #[test]
    fn a_ratio_above_one_takes_more_input_than_it_writes() {
        let (taken, written, _) = run(1.0005, 10, 2);
        let delay = 2_000.0; // the filter's start, generously
        assert_eq!(taken, 441_000);
        let expected = taken as f64 / 1.0005;
        assert!(
            (written as f64 - expected).abs() < delay,
            "{written} for {expected}"
        );
        assert!(written < taken);
    }

    #[test]
    fn a_ratio_of_one_keeps_the_tone() {
        let (_, written, out) = run(1.0, 2, 1);
        assert!(written > 80_000);
        // Past the filter's start the tone comes out at its level.
        let peak = out[10_000..written - 1_000]
            .iter()
            .fold(0.0f64, |m, &s| m.max(s.abs()));
        assert!((peak - 0.5).abs() < 0.01, "peak {peak}");
    }

    #[test]
    fn a_ratio_out_of_range_is_refused() {
        let resampler = camel_drift_resampler_create_v1(2);
        assert_eq!(
            unsafe { camel_drift_resampler_set_ratio_v1(resampler, 2.0, 0) },
            ENGINE_E_INVALID_ARGUMENT
        );
        assert_eq!(
            unsafe { camel_drift_resampler_set_ratio_v1(resampler, 0.999, 100) },
            ENGINE_OK
        );
        assert!(unsafe { camel_drift_resampler_delay_v1(resampler) } >= 0.0);
        unsafe { camel_drift_resampler_destroy_v1(resampler) };
        assert!(camel_drift_resampler_create_v1(0).is_null());
    }
}
