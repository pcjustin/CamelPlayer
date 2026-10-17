//! C ABI over Symphonia's FLAC decoder for CamelPlayer (docs/Plan.md §16.3).
//! Samples are returned interleaved as i32, right-aligned to the stream's original bit depth.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};

use symphonia::core::audio::{AudioBufferRef, Signal};
use symphonia::core::codecs::{Decoder, DecoderOptions, VerificationCheck};
use symphonia::core::errors::Error;
use symphonia::core::formats::{FormatOptions, FormatReader, SeekMode, SeekTo};
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[no_mangle]
pub extern "C" fn camel_engine_abi_version() -> u32 {
    camel_audio_engine::camel_engine_abi_version()
}

/// # Safety
/// `config` and `out_engine` must point to valid readable and writable V1 structures.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_create_v1(
    config: *const camel_audio_engine::CamelEngineConfigV1,
    out_engine: *mut *mut camel_audio_engine::CamelEngine,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_create_v1(config, out_engine) }
}

/// # Safety
/// The engine and structures must be valid for the call, and the output buffer must cover its declared capacity.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_render_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    request: *const camel_audio_engine::CamelRenderRequestV1,
    result: *mut camel_audio_engine::CamelRenderResultV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_render_v1(engine, request, result) }
}

/// # Safety
/// All pointers must reference valid V1 structures. Decoder callbacks must remain valid until source replacement or Engine destruction.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_load_flac_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
    source: *const camel_audio_engine::CamelTrackSourceV1,
    decoder_kind: u32,
    decoder_host: *const camel_audio_engine::CamelDecoderHostV1,
) -> i32 {
    unsafe {
        camel_audio_engine::camel_engine_load_flac_v1(
            engine,
            command,
            source,
            decoder_kind,
            decoder_host,
        )
    }
}

/// # Safety
/// `engine`, `output`, and `frames_read` must reference valid storage for this call.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_read_decoded_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    output: *mut i32,
    frame_capacity: u64,
    frames_read: *mut u64,
) -> i32 {
    unsafe {
        camel_audio_engine::camel_engine_read_decoded_v1(
            engine,
            output,
            frame_capacity,
            frames_read,
        )
    }
}

/// # Safety
/// `engine` and `frames_prefilled` must reference valid storage. Call from one decode worker only.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_prefill_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    frame_limit: u64,
    frames_prefilled: *mut u64,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_prefill_v1(engine, frame_limit, frames_prefilled) }
}

/// # Safety
/// `engine` and `command` must remain valid for the call.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_seek_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
    frame: u64,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_seek_v1(engine, command, frame) }
}

/// # Safety
/// `engine` and `command` must reference a live engine and readable V1 command.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_play_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_play_v1(engine, command) }
}

/// # Safety
/// `engine` and `command` must reference a live engine and readable V1 command.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_pause_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_pause_v1(engine, command) }
}

/// # Safety
/// `engine` and `command` must reference a live engine and readable V1 command.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_stop_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_stop_v1(engine, command) }
}

/// # Safety
/// `engine` and `timeline` must reference a live engine and writable V1 timeline.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_get_timeline_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    timeline: *mut camel_audio_engine::CamelEngineTimelineV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_get_timeline_v1(engine, timeline) }
}

/// # Safety
/// `engine` and `command` must reference a live engine and readable V1 command.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_report_audible_position_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    command: *const camel_audio_engine::CamelCommandStampV1,
    audible_frame: u64,
) -> i32 {
    unsafe {
        camel_audio_engine::camel_engine_report_audible_position_v1(engine, command, audible_frame)
    }
}

/// # Safety
/// `engine` and `state` must reference a live handle and writable V1 state.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_get_state_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    state: *mut camel_audio_engine::CamelEngineStateV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_get_state_v1(engine, state) }
}

#[no_mangle]
/// # Safety
/// Both pointers must reference valid engine and writable integrity state for this call.
pub unsafe extern "C" fn camel_engine_get_flac_integrity_v1(
    engine: *mut camel_audio_engine::CamelEngine,
    integrity: *mut camel_audio_engine::CamelFlacIntegrityV1,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_get_flac_integrity_v1(engine, integrity) }
}

/// # Safety
/// `engine` must be a live handle, and the caller must prevent new calls from starting during quiescence.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_quiesce_v1(
    engine: *mut camel_audio_engine::CamelEngine,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_quiesce_v1(engine) }
}

/// # Safety
/// `engine` must be a live, quiesced handle with no concurrent calls.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_destroy_v1(
    engine: *mut camel_audio_engine::CamelEngine,
) -> i32 {
    unsafe { camel_audio_engine::camel_engine_destroy_v1(engine) }
}

pub const SYM_OK: i32 = 0;
pub const SYM_END: i32 = 1;
pub const SYM_E_IO: i32 = -1;
pub const SYM_E_FORMAT: i32 = -2;
pub const SYM_E_UNSUPPORTED: i32 = -3;
pub const SYM_E_DECODE: i32 = -4;
pub const SYM_E_SEEK: i32 = -5;
pub const SYM_E_ARG: i32 = -6;

#[repr(C)]
pub struct SymInfo {
    pub sample_rate: u32,
    pub channels: u32,
    pub bits_per_sample: u32,
    pub total_frames: u64,
    pub has_md5: u32,
    pub md5: [u8; 16],
}

pub struct SymDecoder {
    format: Box<dyn FormatReader>,
    decoder: Box<dyn Decoder>,
    track_id: u32,
    channels: usize,
    shift: u32,
    /// Decoded, interleaved, right-aligned samples not yet handed out.
    pending: Vec<i32>,
    pending_pos: usize,
    /// Frames still to drop after a seek so the next read starts exactly at the requested frame.
    skip_frames: u64,
    last_error: String,
}

fn map_err(e: &Error) -> i32 {
    match e {
        Error::IoError(_) => SYM_E_IO,
        Error::Unsupported(_) => SYM_E_UNSUPPORTED,
        Error::SeekError(_) => SYM_E_SEEK,
        _ => SYM_E_DECODE,
    }
}

// Unwinding out of an extern "C" function aborts the process, so a Symphonia panic becomes an error code.
fn guarded(body: impl FnOnce() -> i32) -> i32 {
    catch_unwind(AssertUnwindSafe(body)).unwrap_or(SYM_E_DECODE)
}

fn interleave(buf: AudioBufferRef, ch: usize, shift: u32) -> Result<Vec<i32>, i32> {
    let frames = buf.frames();
    let mut out = Vec::with_capacity(frames * ch);
    match buf {
        AudioBufferRef::S32(b) => {
            for f in 0..frames {
                for c in 0..ch {
                    out.push(b.chan(c)[f] >> shift);
                }
            }
        }
        _ => return Err(SYM_E_UNSUPPORTED),
    }
    Ok(out)
}

impl SymDecoder {
    fn set_pending(&mut self, out: Vec<i32>) {
        let ch = self.channels;
        let frames = out.len() / ch;
        let drop = (self.skip_frames as usize).min(frames);
        self.skip_frames -= drop as u64;
        self.pending = out;
        self.pending_pos = drop * ch;
    }

    /// Decodes the next packet of our track into `pending`. Ok(false) at end of stream.
    fn next_packet(&mut self) -> Result<bool, i32> {
        loop {
            let packet = match self.format.next_packet() {
                Ok(p) => p,
                Err(Error::IoError(ref e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                    return Ok(false)
                }
                Err(e) => {
                    self.last_error = e.to_string();
                    return Err(map_err(&e));
                }
            };
            if packet.track_id() != self.track_id {
                continue;
            }
            match self.decoder.decode(&packet) {
                Ok(buf) => {
                    let out = interleave(buf, self.channels, self.shift)?;
                    self.set_pending(out);
                    return Ok(true);
                }
                Err(e) => {
                    self.last_error = e.to_string();
                    return Err(map_err(&e));
                }
            }
        }
    }
}

/// Opens `path` (UTF-8). On success stores a decoder handle in `out` and fills `info`.
///
/// # Safety
/// `path` must be NUL-terminated, and `out` and `info` must point to writable storage.
#[no_mangle]
pub unsafe extern "C" fn sym_open(
    path: *const c_char,
    out: *mut *mut SymDecoder,
    info: *mut SymInfo,
) -> i32 {
    if path.is_null() || out.is_null() || info.is_null() {
        return SYM_E_ARG;
    }
    *out = std::ptr::null_mut();
    guarded(|| open_decoder(path, out, info))
}

unsafe fn open_decoder(path: *const c_char, out: *mut *mut SymDecoder, info: *mut SymInfo) -> i32 {
    let path = CStr::from_ptr(path).to_string_lossy().into_owned();
    let file = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(_) => return SYM_E_IO,
    };
    let mss = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    hint.with_extension("flac");
    let probed = match symphonia::default::get_probe().format(
        &hint,
        mss,
        &FormatOptions::default(),
        &MetadataOptions::default(),
    ) {
        Ok(p) => p,
        Err(e) => {
            return if matches!(e, Error::Unsupported(_)) {
                SYM_E_UNSUPPORTED
            } else {
                SYM_E_FORMAT
            }
        }
    };
    let format = probed.format;
    let Some(track) = format.default_track() else {
        return SYM_E_FORMAT;
    };
    let params = track.codec_params.clone();
    let track_id = track.id;
    let decoder =
        match symphonia::default::get_codecs().make(&params, &DecoderOptions { verify: false }) {
            Ok(d) => d,
            Err(_) => return SYM_E_UNSUPPORTED,
        };
    let channels = params.channels.map(|c| c.count()).unwrap_or(0);
    let bits = params.bits_per_sample.unwrap_or(0);
    if channels == 0 || bits == 0 || bits > 32 {
        return SYM_E_UNSUPPORTED;
    }
    let (has_md5, md5) = match params.verification_check {
        Some(VerificationCheck::Md5(m)) => (1, m),
        _ => (0, [0u8; 16]),
    };
    *info = SymInfo {
        sample_rate: params.sample_rate.unwrap_or(0),
        channels: channels as u32,
        bits_per_sample: bits,
        total_frames: params.n_frames.unwrap_or(0),
        has_md5,
        md5,
    };
    let dec = Box::new(SymDecoder {
        format,
        decoder,
        track_id,
        channels,
        shift: 32 - bits,
        pending: Vec::new(),
        pending_pos: 0,
        skip_frames: 0,
        last_error: String::new(),
    });
    *out = Box::into_raw(dec);
    SYM_OK
}

/// Reads up to `frames` frames into `dst` (frames * channels i32). Returns SYM_OK with `*read` frames,
/// SYM_END with `*read` = 0 at end of stream, or a negative error code.
///
/// # Safety
/// `dec` must be a live exclusive handle; `dst` must hold `frames * channels` samples and `read` must be writable.
#[no_mangle]
pub unsafe extern "C" fn sym_read(
    dec: *mut SymDecoder,
    dst: *mut i32,
    frames: usize,
    read: *mut usize,
) -> i32 {
    if dec.is_null() || dst.is_null() || read.is_null() {
        return SYM_E_ARG;
    }
    *read = 0;
    guarded(|| read_frames(&mut *dec, dst, frames, read))
}

unsafe fn read_frames(d: &mut SymDecoder, dst: *mut i32, frames: usize, read: *mut usize) -> i32 {
    let Some(want) = frames.checked_mul(d.channels) else {
        return SYM_E_ARG;
    };
    let out = std::slice::from_raw_parts_mut(dst, want);
    let mut filled = 0;
    while filled < want {
        if d.pending_pos >= d.pending.len() {
            match d.next_packet() {
                Ok(true) => continue,
                Ok(false) => break,
                Err(code) => {
                    *read = filled / d.channels;
                    return code;
                }
            }
        }
        let n = (want - filled).min(d.pending.len() - d.pending_pos);
        out[filled..filled + n].copy_from_slice(&d.pending[d.pending_pos..d.pending_pos + n]);
        filled += n;
        d.pending_pos += n;
    }
    *read = filled / d.channels;
    if filled == 0 {
        SYM_END
    } else {
        SYM_OK
    }
}

/// Seeks so the next read starts exactly at `frame`.
///
/// # Safety
/// `dec` must be a live exclusive handle with no concurrent calls.
#[no_mangle]
pub unsafe extern "C" fn sym_seek(dec: *mut SymDecoder, frame: u64) -> i32 {
    if dec.is_null() {
        return SYM_E_ARG;
    }
    guarded(|| seek_frame(&mut *dec, frame))
}

fn seek_frame(d: &mut SymDecoder, frame: u64) -> i32 {
    let seeked = match d.format.seek(
        SeekMode::Accurate,
        SeekTo::TimeStamp {
            ts: frame,
            track_id: d.track_id,
        },
    ) {
        Ok(s) => s,
        Err(e) => {
            d.last_error = e.to_string();
            return SYM_E_SEEK;
        }
    };
    d.decoder.reset();
    d.pending.clear();
    d.pending_pos = 0;
    d.skip_frames = seeked.required_ts.saturating_sub(seeked.actual_ts);
    SYM_OK
}

/// Copies the last error message (UTF-8, NUL-terminated, truncated to `len`).
///
/// # Safety
/// `dec` must be a live handle, and `buf` must be writable for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn sym_last_error(dec: *const SymDecoder, buf: *mut c_char, len: usize) {
    if dec.is_null() || buf.is_null() || len == 0 {
        return;
    }
    let msg = (*dec).last_error.as_bytes();
    let n = msg.len().min(len - 1);
    std::ptr::copy_nonoverlapping(msg.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
}

/// # Safety
/// `dec` must be a live handle with no concurrent calls; it is invalid after this call.
#[no_mangle]
pub unsafe extern "C" fn sym_close(dec: *mut SymDecoder) {
    if !dec.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(dec))));
    }
}
