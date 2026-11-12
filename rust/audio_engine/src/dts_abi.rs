// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C ABI of rust/dts_core, which allows no unsafe code. Keep in sync with dts_abi.h.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

use dts_core::{find_sync, frame_info, DecodeError, Decoder, Speaker};

const DTS_OK: i32 = 0;
const DTS_E_NO_SYNC: i32 = -1;
const DTS_E_TRUNCATED: i32 = -2;
const DTS_E_UNSUPPORTED: i32 = -3;
const DTS_E_DECODE: i32 = -4;
const DTS_E_ARG: i32 = -5;

pub struct DtsDecoder {
    decoder: Decoder,
    pcm: Vec<f32>,
}

#[repr(C)]
pub struct DtsFrameInfo {
    container_bytes: u32,
    sample_rate: u32,
}

#[repr(C)]
pub struct DtsPcm {
    /// Set by the caller to the size it allocated.
    struct_size: u32,
    samples: *const f32,
    channels: u32,
    frames: u32,
    sample_rate: u32,
    /// WAVE speaker positions (SPEAKER_* bits) of the channels, in their order.
    channel_mask: u32,
}

fn code_of(error: &DecodeError) -> i32 {
    match error {
        DecodeError::NoSync => DTS_E_NO_SYNC,
        DecodeError::Truncated => DTS_E_TRUNCATED,
        DecodeError::UnsupportedChannels | DecodeError::UnsupportedSampleRate => DTS_E_UNSUPPORTED,
        DecodeError::Header(_) | DecodeError::Frame(_) | DecodeError::UnequalChannels => {
            DTS_E_DECODE
        }
    }
}

// The WAVE speaker position of a channel; the surrounds are the back pair, as in the WAVE 5.1 layout, so the bits
// rise in the order the channels come in.
fn speaker_bit(speaker: Speaker) -> u32 {
    match speaker {
        Speaker::Left => 0x1,
        Speaker::Right => 0x2,
        Speaker::Center => 0x4,
        Speaker::Lfe => 0x8,
        Speaker::LeftSurround => 0x10,
        Speaker::RightSurround => 0x20,
        Speaker::BackCenter => 0x100,
    }
}

unsafe fn bytes_of<'a>(bytes: *const u8, len: usize) -> &'a [u8] {
    if bytes.is_null() || len == 0 {
        &[]
    } else {
        slice::from_raw_parts(bytes, len)
    }
}

#[no_mangle]
pub extern "C" fn dts_new() -> *mut DtsDecoder {
    catch_unwind(|| {
        Box::into_raw(Box::new(DtsDecoder {
            decoder: Decoder::new(),
            pcm: Vec::new(),
        }))
    })
    .unwrap_or(ptr::null_mut())
}

/// # Safety
/// `decoder` comes from `dts_new` and is not used afterwards, or is null.
#[no_mangle]
pub unsafe extern "C" fn dts_free(decoder: *mut DtsDecoder) {
    if !decoder.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(decoder))));
    }
}

/// # Safety
/// `decoder` comes from `dts_new`, or is null.
#[no_mangle]
pub unsafe extern "C" fn dts_reset(decoder: *mut DtsDecoder) {
    if let Some(d) = decoder.as_mut() {
        d.decoder.reset();
    }
}

/// # Safety
/// `bytes` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn dts_find_sync(bytes: *const u8, len: usize, start: usize) -> i64 {
    let input = bytes_of(bytes, len);
    catch_unwind(|| find_sync(input, start).map_or(-1, |found| found.offset as i64)).unwrap_or(-1)
}

/// # Safety
/// `bytes` points to `len` readable bytes and `info` to a writable `DtsFrameInfo`.
#[no_mangle]
pub unsafe extern "C" fn dts_frame_info(
    bytes: *const u8,
    len: usize,
    info: *mut DtsFrameInfo,
) -> i32 {
    if info.is_null() {
        return DTS_E_ARG;
    }
    let input = bytes_of(bytes, len);
    match catch_unwind(|| frame_info(input)) {
        Ok(Ok(found)) => {
            *info = DtsFrameInfo {
                container_bytes: found.container_bytes as u32,
                sample_rate: found.sample_rate,
            };
            DTS_OK
        }
        Ok(Err(error)) => code_of(&error),
        Err(_) => DTS_E_DECODE,
    }
}

/// # Safety
/// `decoder` comes from `dts_new`, `bytes` points to `len` readable bytes and `pcm` to a writable `DtsPcm`, whose samples
/// stay valid until the next call with this decoder.
#[no_mangle]
pub unsafe extern "C" fn dts_decode(
    decoder: *mut DtsDecoder,
    bytes: *const u8,
    len: usize,
    pcm: *mut DtsPcm,
) -> i32 {
    let Some(d) = decoder.as_mut() else {
        return DTS_E_ARG;
    };
    if pcm.is_null() || (*pcm).struct_size < std::mem::size_of::<DtsPcm>() as u32 {
        return DTS_E_ARG;
    }
    let input = bytes_of(bytes, len);
    match catch_unwind(AssertUnwindSafe(|| d.decoder.decode(input))) {
        Ok(Ok(decoded)) => {
            d.pcm = decoded.pcm;
            *pcm = DtsPcm {
                struct_size: std::mem::size_of::<DtsPcm>() as u32,
                samples: d.pcm.as_ptr(),
                channels: decoded.speakers.len() as u32,
                frames: decoded.samples as u32,
                sample_rate: decoded.sample_rate,
                channel_mask: decoded.speakers.iter().map(|&s| speaker_bit(s)).sum(),
            };
            DTS_OK
        }
        Ok(Err(error)) => code_of(&error),
        Err(_) => {
            d.decoder.reset();
            DTS_E_DECODE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_pcm() -> DtsPcm {
        DtsPcm {
            struct_size: std::mem::size_of::<DtsPcm>() as u32,
            samples: ptr::null(),
            channels: 0,
            frames: 0,
            sample_rate: 0,
            channel_mask: 0,
        }
    }

    // The roles cross the ABI with the channels: a 5.1 frame is front left, right and centre, LFE and the two
    // surrounds, the WAVE 5.1 layout.
    #[test]
    fn a_frame_names_the_speaker_of_each_channel() {
        let frame = &include_bytes!("../../dts_core/tests/fixtures/dts_51_lfe.bin")[..1024];
        unsafe {
            let decoder = dts_new();
            let mut pcm = empty_pcm();
            assert_eq!(
                dts_decode(decoder, frame.as_ptr(), frame.len(), &mut pcm),
                DTS_OK
            );
            assert_eq!(pcm.channels, 6);
            assert_eq!(pcm.channel_mask, 0x3F);
            pcm.struct_size = 4;
            assert_eq!(
                dts_decode(decoder, frame.as_ptr(), frame.len(), &mut pcm),
                DTS_E_ARG
            );
            dts_free(decoder);
        }
    }

    #[test]
    fn nonsense_gets_an_error_and_no_decoder_needs_bytes() {
        unsafe {
            let decoder = dts_new();
            let mut pcm = empty_pcm();
            assert_eq!(dts_decode(decoder, ptr::null(), 0, &mut pcm), DTS_E_NO_SYNC);
            assert_eq!(
                dts_decode(ptr::null_mut(), ptr::null(), 0, &mut pcm),
                DTS_E_ARG
            );
            assert_eq!(
                dts_decode(decoder, ptr::null(), 0, ptr::null_mut()),
                DTS_E_ARG
            );
            assert_eq!(dts_find_sync(ptr::null(), 0, 0), -1);
            let zeros = [0_u8; 64];
            assert_eq!(dts_find_sync(zeros.as_ptr(), zeros.len(), 0), -1);
            dts_reset(decoder);
            dts_free(decoder);
            dts_free(ptr::null_mut());
        }
    }
}
