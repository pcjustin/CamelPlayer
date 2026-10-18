//! Streaming sample-rate conversion through libsoxr (docs/Plan.md §5.2), on interleaved 64-bit float frames.

use std::ffi::{c_char, c_uint, c_ulong, c_void};

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
    fn soxr_delete(resampler: *mut c_void);
    fn soxr_quality_spec(recipe: c_ulong, flags: c_ulong) -> SoxrQualitySpec;
    fn soxr_io_spec(itype: c_uint, otype: c_uint) -> SoxrIoSpec;
}

/// soxr recipes behind the quick / high / very high setting.
pub const QUALITY_QUICK: u32 = 0;
pub const QUALITY_HIGH: u32 = 1;
pub const QUALITY_VERY_HIGH: u32 = 2;

pub(crate) struct Resampler {
    handle: *mut c_void,
    channels: usize,
    ratio: f64,
}

// The handle is only used by the decode worker that owns the source.
unsafe impl Send for Resampler {}

impl Resampler {
    pub(crate) fn new(
        input_rate: u32,
        output_rate: u32,
        channels: usize,
        quality: u32,
    ) -> Option<Self> {
        let recipe: c_ulong = match quality {
            QUALITY_QUICK => 0,     // SOXR_QQ
            QUALITY_VERY_HIGH => 6, // SOXR_VHQ
            _ => 4,                 // SOXR_HQ
        };
        let mut error: *const c_char = std::ptr::null();
        let handle = unsafe {
            let io = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
            let quality = soxr_quality_spec(recipe, 0);
            soxr_create(
                f64::from(input_rate),
                f64::from(output_rate),
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
            return None;
        }
        Some(Self {
            handle,
            channels,
            ratio: f64::from(output_rate) / f64::from(input_rate),
        })
    }

    /// Output frames per input frame.
    pub(crate) fn ratio(&self) -> f64 {
        self.ratio
    }

    /// Output frames that `input_frames` can produce, with room for the filter's buffered tail.
    pub(crate) fn output_capacity(&self, input_frames: usize) -> usize {
        (input_frames as f64 * self.ratio).ceil() as usize + 1024
    }

    /// Resamples `input` (interleaved frames) and appends the output to `output`. An empty `input` ends the stream
    /// and flushes the remaining output.
    pub(crate) fn process(&mut self, input: &[f64], output: &mut Vec<f64>) -> Result<(), ()> {
        let end = input.is_empty();
        let mut offset = 0;
        loop {
            let remaining = (input.len() - offset) / self.channels;
            let capacity = self.output_capacity(remaining.max(1));
            let start = output.len();
            output.resize(start + capacity * self.channels, 0.0);
            let (mut used, mut produced) = (0usize, 0usize);
            let error = unsafe {
                soxr_process(
                    self.handle,
                    if end {
                        std::ptr::null()
                    } else {
                        input[offset..].as_ptr().cast()
                    },
                    remaining,
                    &mut used,
                    output[start..].as_mut_ptr().cast(),
                    capacity,
                    &mut produced,
                )
            };
            output.truncate(start + produced * self.channels);
            if !error.is_null() {
                return Err(());
            }
            offset += used * self.channels;
            // Input is consumed once the resampler takes it; a flush continues until nothing more comes out.
            if (!end && offset >= input.len()) || (end && produced == 0) {
                return Ok(());
            }
        }
    }
}

impl Drop for Resampler {
    fn drop(&mut self) {
        unsafe { soxr_delete(self.handle) };
    }
}
