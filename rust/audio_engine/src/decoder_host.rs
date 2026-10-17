use std::ffi::c_void;

use crate::{CamelTrackSourceV1, ENGINE_E_DECODE, ENGINE_E_SEEK, ENGINE_E_SOURCE_IO};

pub const DECODER_FFMPEG: u32 = 1;
pub const DECODER_SYMPHONIA: u32 = 2;
pub const DECODER_DR_FLAC: u32 = 3;
pub const DECODER_SNDFILE: u32 = 4;

pub const HOST_OK: i32 = 0;
pub const HOST_E_IO: i32 = -1;
pub const HOST_E_FORMAT: i32 = -2;
pub const HOST_E_UNSUPPORTED: i32 = -3;
pub const HOST_E_DECODE: i32 = -4;
pub const HOST_E_SEEK: i32 = -5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelSourceInfoV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub sample_rate: u32,
    pub channel_count: u32,
    pub valid_bits: u32,
    pub reserved: u32,
    pub total_frames: u64,
    pub has_md5: u32,
    pub reserved2: u32,
    pub md5: [u8; 16],
}

pub type DecoderOpenV1 = unsafe extern "C" fn(
    context: *mut c_void,
    decoder_kind: u32,
    path_utf8: *const u8,
    path_length_bytes: u64,
    out_decoder: *mut *mut c_void,
    info: *mut CamelSourceInfoV1,
) -> i32;
pub type DecoderReadV1 = unsafe extern "C" fn(
    context: *mut c_void,
    decoder: *mut c_void,
    output: *mut i32,
    frames: u64,
    frames_read: *mut u64,
) -> i32;
pub type DecoderSeekV1 =
    unsafe extern "C" fn(context: *mut c_void, decoder: *mut c_void, frame: u64) -> i32;
pub type DecoderCloseV1 = unsafe extern "C" fn(context: *mut c_void, decoder: *mut c_void);

#[repr(C)]
pub struct CamelDecoderHostV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub context: *mut c_void,
    pub open: Option<DecoderOpenV1>,
    pub read: Option<DecoderReadV1>,
    pub seek: Option<DecoderSeekV1>,
    pub close: Option<DecoderCloseV1>,
}

#[derive(Clone, Copy)]
pub(crate) struct DecoderHost {
    context: usize,
    open: DecoderOpenV1,
    read: DecoderReadV1,
    seek: DecoderSeekV1,
    close: DecoderCloseV1,
}

impl DecoderHost {
    pub(crate) unsafe fn from_abi(host: &CamelDecoderHostV1) -> Option<Self> {
        Some(Self {
            context: host.context as usize,
            open: host.open?,
            read: host.read?,
            seek: host.seek?,
            close: host.close?,
        })
    }

    fn context(self) -> *mut c_void {
        self.context as *mut c_void
    }
}

pub(crate) struct DecoderSource {
    decoder: usize,
    host: DecoderHost,
    pub(crate) info: CamelSourceInfoV1,
    position: u64,
    start_frame: u64,
    end_frame: Option<u64>,
}

impl DecoderSource {
    pub(crate) unsafe fn open(
        source: &CamelTrackSourceV1,
        decoder_kind: u32,
        host: DecoderHost,
    ) -> Result<Self, i32> {
        if !matches!(
            decoder_kind,
            DECODER_FFMPEG | DECODER_SYMPHONIA | DECODER_DR_FLAC | DECODER_SNDFILE
        ) || source.path_utf8.is_null()
            || source.path_length_bytes == 0
            || source.path_length_bytes > 32_768
            || source.path_length_bytes > usize::MAX as u64
            || source.has_end_frame > 1
            || source.reserved != 0
        {
            return Err(crate::ENGINE_E_INVALID_ARGUMENT);
        }

        let path_bytes =
            std::slice::from_raw_parts(source.path_utf8, source.path_length_bytes as usize);
        let path = std::str::from_utf8(path_bytes)
            .map_err(|_| crate::ENGINE_E_INVALID_ARGUMENT)?
            .to_owned();
        if path.as_bytes().contains(&0) {
            return Err(crate::ENGINE_E_INVALID_ARGUMENT);
        }

        let mut info = CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: crate::ABI_VERSION,
            sample_rate: 0,
            channel_count: 0,
            valid_bits: 0,
            reserved: 0,
            total_frames: 0,
            has_md5: 0,
            reserved2: 0,
            md5: [0; 16],
        };
        let mut decoder = std::ptr::null_mut();
        let open_status = (host.open)(
            host.context(),
            decoder_kind,
            path.as_ptr(),
            path.len() as u64,
            &mut decoder,
            &mut info,
        );
        if open_status != HOST_OK {
            return Err(map_host_status(open_status));
        }
        if decoder.is_null()
            || info.abi_version != crate::ABI_VERSION
            || info.struct_size < std::mem::size_of::<CamelSourceInfoV1>() as u32
            || info.sample_rate == 0
            || info.channel_count == 0
            || info.channel_count > 32
            || info.reserved != 0
            || info.reserved2 != 0
        {
            if !decoder.is_null() {
                (host.close)(host.context(), decoder);
            }
            return Err(crate::ENGINE_E_UNSUPPORTED_FORMAT);
        }
        if !matches!(info.valid_bits, 16 | 24) {
            (host.close)(host.context(), decoder);
            return Err(crate::ENGINE_E_UNSUPPORTED_FORMAT);
        }
        if info.has_md5 > 1 {
            (host.close)(host.context(), decoder);
            return Err(crate::ENGINE_E_UNSUPPORTED_FORMAT);
        }

        let end_frame = if source.has_end_frame != 0 {
            if source.end_frame_exclusive <= source.start_frame
                || (info.total_frames != 0 && source.end_frame_exclusive > info.total_frames)
            {
                (host.close)(host.context(), decoder);
                return Err(crate::ENGINE_E_INVALID_ARGUMENT);
            }
            Some(source.end_frame_exclusive)
        } else {
            None
        };
        if info.total_frames != 0 && source.start_frame >= info.total_frames {
            (host.close)(host.context(), decoder);
            return Err(crate::ENGINE_E_SEEK);
        }
        if source.start_frame != 0 {
            let status = (host.seek)(host.context(), decoder, source.start_frame);
            if status != HOST_OK {
                (host.close)(host.context(), decoder);
                return Err(map_host_status(status));
            }
        }

        Ok(Self {
            decoder: decoder as usize,
            host,
            info,
            position: source.start_frame,
            start_frame: source.start_frame,
            end_frame,
        })
    }

    pub(crate) fn read(&mut self, output: &mut [i32], frame_capacity: u64) -> Result<u64, i32> {
        let capacity = frame_capacity.min(output.len() as u64 / u64::from(self.info.channel_count));
        let remaining = self
            .end_frame
            .map(|end| end.saturating_sub(self.position))
            .unwrap_or(u64::MAX);
        let frames = capacity.min(remaining);
        if frames == 0 {
            return Ok(0);
        }
        let mut frames_read = 0;
        let status = unsafe {
            (self.host.read)(
                self.host.context(),
                self.decoder as *mut c_void,
                output.as_mut_ptr(),
                frames,
                &mut frames_read,
            )
        };
        if status != HOST_OK {
            return Err(map_host_status(status));
        }
        if frames_read > frames {
            return Err(crate::ENGINE_E_DECODE);
        }
        self.position = self
            .position
            .checked_add(frames_read)
            .ok_or(crate::ENGINE_E_DECODE)?;
        Ok(frames_read)
    }

    pub(crate) fn seek(&mut self, frame: u64) -> Result<(), i32> {
        if frame < self.start_frame
            || self.end_frame.is_some_and(|end| frame >= end)
            || (self.info.total_frames != 0 && frame >= self.info.total_frames)
        {
            return Err(crate::ENGINE_E_SEEK);
        }
        let status =
            unsafe { (self.host.seek)(self.host.context(), self.decoder as *mut c_void, frame) };
        if status != HOST_OK {
            return Err(map_host_status(status));
        }
        self.position = frame;
        Ok(())
    }

    pub(crate) fn position(&self) -> u64 {
        self.position
    }
}

impl Drop for DecoderSource {
    fn drop(&mut self) {
        unsafe { (self.host.close)(self.host.context(), self.decoder as *mut c_void) }
    }
}

fn map_host_status(status: i32) -> i32 {
    match status {
        HOST_E_IO => ENGINE_E_SOURCE_IO,
        HOST_E_FORMAT | HOST_E_UNSUPPORTED => crate::ENGINE_E_UNSUPPORTED_FORMAT,
        HOST_E_DECODE => ENGINE_E_DECODE,
        HOST_E_SEEK => ENGINE_E_SEEK,
        _ => ENGINE_E_DECODE,
    }
}

const _: () = assert!(std::mem::size_of::<CamelSourceInfoV1>() == 56);
const _: () = assert!(std::mem::size_of::<CamelDecoderHostV1>() == 48);
