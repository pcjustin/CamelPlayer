// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The DSD-to-PCM decimator for callers outside playback, such as the waveform analysis: DSD byte frames in, 352.8 kHz
//! PCM out, as playback converts them.

use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::dsd2pcm::{DsdDecimator, PCM_RATE};
use crate::{ENGINE_E_BUFFER_TOO_SMALL, ENGINE_E_INVALID_ARGUMENT, ENGINE_E_PANIC, ENGINE_OK};

pub struct CamelDsdToPcm {
    decimator: DsdDecimator,
    channels: usize,
    input: Vec<i32>,
    output: Vec<f64>,
}

/// A decimator for DSD at `dsd_rate` bits per second per channel (DSD64 to DSD512) with `channels` channels; null when
/// the rate or the channel count is not one playback converts.
#[no_mangle]
pub extern "C" fn camel_dsd_to_pcm_create_v1(dsd_rate: u32, channels: u32) -> *mut CamelDsdToPcm {
    catch_unwind(|| {
        let channels = usize::try_from(channels).unwrap_or(0);
        DsdDecimator::new(dsd_rate / 8, channels).map_or(std::ptr::null_mut(), |decimator| {
            Box::into_raw(Box::new(CamelDsdToPcm {
                decimator,
                channels,
                input: Vec::new(),
                output: Vec::new(),
            }))
        })
    })
    .unwrap_or(std::ptr::null_mut())
}

/// The PCM rate the decimator gives.
#[no_mangle]
pub extern "C" fn camel_dsd_to_pcm_rate_v1() -> u32 {
    PCM_RATE
}

/// Converts `frames` byte frames (`input`, interleaved, the oldest sample in the most significant bit) and writes the
/// PCM frames they complete, interleaved and at full scale 1.0, to `output`, which holds `capacity` frames; at most
/// `frames` frames come out. `produced` receives how many.
///
/// # Safety
/// `converter` comes from `camel_dsd_to_pcm_create_v1`; `input` holds `frames` times the channels bytes and `output`
/// `capacity` times the channels floats.
#[no_mangle]
pub unsafe extern "C" fn camel_dsd_to_pcm_process_v1(
    converter: *mut CamelDsdToPcm,
    input: *const u8,
    frames: usize,
    output: *mut f32,
    capacity: usize,
    produced: *mut usize,
) -> i32 {
    if converter.is_null()
        || produced.is_null()
        || (frames > 0 && (input.is_null() || output.is_null()))
    {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    if capacity < frames {
        return ENGINE_E_BUFFER_TOO_SMALL;
    }
    // The buffers may be null with no frames, and a slice is never made of a null pointer.
    if frames == 0 {
        *produced = 0;
        return ENGINE_OK;
    }
    let converter = &mut *converter;
    let channels = converter.channels;
    let bytes = std::slice::from_raw_parts(input, frames * channels);
    match catch_unwind(AssertUnwindSafe(|| {
        converter.input.clear();
        converter.input.extend(bytes.iter().map(|&b| i32::from(b)));
        converter.output.clear();
        converter
            .decimator
            .process(&converter.input, frames, &mut converter.output);
        let out = std::slice::from_raw_parts_mut(output, capacity * channels);
        for (to, &from) in out.iter_mut().zip(&converter.output) {
            *to = from as f32;
        }
        converter.output.len() / channels
    })) {
        Ok(count) => {
            *produced = count;
            ENGINE_OK
        }
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `converter` comes from `camel_dsd_to_pcm_create_v1` and is not used again.
#[no_mangle]
pub unsafe extern "C" fn camel_dsd_to_pcm_destroy_v1(converter: *mut CamelDsdToPcm) {
    if !converter.is_null() {
        drop(Box::from_raw(converter));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // No frames may come with no buffers: nothing is converted, and no slice is made of a null pointer.
    #[test]
    fn no_frames_with_null_buffers_converts_nothing() {
        let converter = camel_dsd_to_pcm_create_v1(2_822_400, 2);
        assert!(!converter.is_null());
        let mut produced = usize::MAX;
        let result = unsafe {
            camel_dsd_to_pcm_process_v1(
                converter,
                std::ptr::null(),
                0,
                std::ptr::null_mut(),
                0,
                &mut produced,
            )
        };
        assert_eq!(result, ENGINE_OK);
        assert_eq!(produced, 0);
        unsafe { camel_dsd_to_pcm_destroy_v1(converter) };
    }

    #[test]
    fn silence_stays_near_zero_and_a_full_scale_pattern_does_not() {
        let converter = camel_dsd_to_pcm_create_v1(2_822_400, 2);
        assert!(!converter.is_null());
        let frames = 4096;
        let mut out = vec![0.0f32; frames * 2];
        let mut produced = 0usize;
        let silence = vec![0x69u8; frames * 2];
        unsafe {
            assert_eq!(
                camel_dsd_to_pcm_process_v1(
                    converter,
                    silence.as_ptr(),
                    frames,
                    out.as_mut_ptr(),
                    frames,
                    &mut produced
                ),
                ENGINE_OK
            );
        }
        assert!(produced > 0);
        assert!(out[..produced * 2].iter().all(|v| v.abs() < 0.01));
        let ones = vec![0xFFu8; frames * 2];
        unsafe {
            camel_dsd_to_pcm_process_v1(
                converter,
                ones.as_ptr(),
                frames,
                out.as_mut_ptr(),
                frames,
                &mut produced,
            );
            camel_dsd_to_pcm_destroy_v1(converter);
        }
        assert!(out[..produced * 2].iter().any(|v| v.abs() > 0.5));
    }

    #[test]
    fn rates_playback_does_not_convert_are_refused() {
        assert!(camel_dsd_to_pcm_create_v1(44_100, 2).is_null());
        assert!(camel_dsd_to_pcm_create_v1(2_822_400, 0).is_null());
    }
}
