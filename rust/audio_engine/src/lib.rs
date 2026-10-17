use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::thread;

mod decoder_host;

pub use decoder_host::{CamelDecoderHostV1, CamelSourceInfoV1};
use decoder_host::{DecoderHost, DecoderSource};

pub const ABI_VERSION: u32 = 1;
pub const MAX_RENDER_UNITS: u64 = 65_536;

pub const AUDIO_PCM: u32 = 1;
pub const AUDIO_DSD: u32 = 2;
pub const AUDIO_DOP: u32 = 3;

pub const SAMPLE_S16_LE: u32 = 1;
pub const SAMPLE_S24_LE: u32 = 2;
pub const SAMPLE_S24_IN_32_LE: u32 = 3;
pub const SAMPLE_S32_LE: u32 = 4;
pub const SAMPLE_F32_LE: u32 = 5;
pub const SAMPLE_DSD_U8: u32 = 6;
pub const SAMPLE_DOP_S24_LE: u32 = 7;

pub const ENGINE_OK: i32 = 0;
pub const ENGINE_END: i32 = 1;
pub const ENGINE_UNDERRUN: i32 = 2;
pub const ENGINE_E_INVALID_ARGUMENT: i32 = -1;
pub const ENGINE_E_ABI_MISMATCH: i32 = -2;
pub const ENGINE_E_STRUCT_SIZE: i32 = -3;
pub const ENGINE_E_BUFFER_TOO_SMALL: i32 = -4;
pub const ENGINE_E_INVALID_STATE: i32 = -5;
pub const ENGINE_E_UNSUPPORTED_FORMAT: i32 = -6;
pub const ENGINE_E_LIMIT_EXCEEDED: i32 = -7;
pub const ENGINE_E_NOT_READY: i32 = -8;
pub const ENGINE_E_PANIC: i32 = -9;
pub const ENGINE_E_SOURCE_IO: i32 = -10;
pub const ENGINE_E_DECODE: i32 = -11;
pub const ENGINE_E_SEEK: i32 = -12;

pub const RENDER_STATE_NO_SOURCE: u32 = 0;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelCommandStampV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub command_id: u64,
    pub generation: u64,
    pub queue_revision: u64,
}

#[repr(C)]
pub struct CamelTrackSourceV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub path_utf8: *const u8,
    pub path_length_bytes: u64,
    pub start_frame: u64,
    pub end_frame_exclusive: u64,
    pub has_end_frame: u32,
    pub reserved: u32,
}

const CLOSING: usize = 1usize << (usize::BITS - 1);
const ACTIVE_MASK: usize = !CLOSING;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelAudioFormatV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub stream_kind: u32,
    pub sample_format: u32,
    pub rate_hz: u32,
    pub channel_count: u32,
    pub valid_bits: u32,
    pub container_bits: u32,
    pub channel_mask: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelEngineConfigV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub max_render_units: u64,
    pub format: CamelAudioFormatV1,
}

#[repr(C)]
pub struct CamelRenderRequestV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub units: u64,
    pub output: *mut u8,
    pub output_capacity_bytes: u64,
}

#[repr(C)]
pub struct CamelRenderResultV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub units_rendered: u64,
    pub required_bytes: u64,
    pub state: u32,
    pub reserved: u32,
}

#[repr(C)]
pub struct CamelEngineStateV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub playback_state: u32,
    pub last_error_code: i32,
    pub command: CamelCommandStampV1,
    pub position_frames: u64,
    pub total_frames: u64,
    pub has_source: u32,
    pub reserved: u32,
    pub source_format: CamelAudioFormatV1,
    pub output_format: CamelAudioFormatV1,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EngineStatus {
    Ok = ENGINE_OK,
    End = ENGINE_END,
    Underrun = ENGINE_UNDERRUN,
    InvalidArgument = ENGINE_E_INVALID_ARGUMENT,
    AbiMismatch = ENGINE_E_ABI_MISMATCH,
    StructSize = ENGINE_E_STRUCT_SIZE,
    BufferTooSmall = ENGINE_E_BUFFER_TOO_SMALL,
    InvalidState = ENGINE_E_INVALID_STATE,
    UnsupportedFormat = ENGINE_E_UNSUPPORTED_FORMAT,
    LimitExceeded = ENGINE_E_LIMIT_EXCEEDED,
    NotReady = ENGINE_E_NOT_READY,
    Panic = ENGINE_E_PANIC,
    SourceIo = ENGINE_E_SOURCE_IO,
    Decode = ENGINE_E_DECODE,
    Seek = ENGINE_E_SEEK,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct AudioFormat {
    stream_kind: u32,
    sample_format: u32,
    channel_count: u32,
    valid_bits: u32,
    container_bits: u32,
}

impl AudioFormat {
    fn from_abi(format: &CamelAudioFormatV1) -> Result<Self, EngineStatus> {
        if format.abi_version != ABI_VERSION {
            return Err(EngineStatus::AbiMismatch);
        }
        if format.struct_size < std::mem::size_of::<CamelAudioFormatV1>() as u32 {
            return Err(EngineStatus::StructSize);
        }
        if format.rate_hz == 0 || format.channel_count == 0 || format.channel_count > 32 {
            return Err(EngineStatus::InvalidArgument);
        }

        let result = Self {
            stream_kind: format.stream_kind,
            sample_format: format.sample_format,
            channel_count: format.channel_count,
            valid_bits: format.valid_bits,
            container_bits: format.container_bits,
        };
        result.validate()?;
        Ok(result)
    }

    fn validate(&self) -> Result<(), EngineStatus> {
        let valid = match (self.stream_kind, self.sample_format) {
            (AUDIO_PCM, SAMPLE_S16_LE) => (self.valid_bits, self.container_bits) == (16, 16),
            (AUDIO_PCM, SAMPLE_S24_LE) => (self.valid_bits, self.container_bits) == (24, 24),
            (AUDIO_PCM, SAMPLE_S24_IN_32_LE) => (self.valid_bits, self.container_bits) == (24, 32),
            (AUDIO_PCM, SAMPLE_S32_LE | SAMPLE_F32_LE) => {
                (self.valid_bits, self.container_bits) == (32, 32)
            }
            (AUDIO_DSD, SAMPLE_DSD_U8) => (self.valid_bits, self.container_bits) == (1, 8),
            (AUDIO_DOP, SAMPLE_DOP_S24_LE) => (self.valid_bits, self.container_bits) == (24, 24),
            _ => false,
        };
        if valid {
            Ok(())
        } else {
            Err(EngineStatus::UnsupportedFormat)
        }
    }

    fn required_bytes(&self, units: u64) -> Result<u64, EngineStatus> {
        let bytes = match self.stream_kind {
            AUDIO_PCM => {
                let bytes_per_sample = u64::from(self.container_bits / 8);
                units
                    .checked_mul(u64::from(self.channel_count))
                    .and_then(|value| value.checked_mul(bytes_per_sample))
            }
            AUDIO_DOP => units
                .checked_mul(u64::from(self.channel_count))
                .and_then(|value| value.checked_mul(3)),
            AUDIO_DSD => {
                if !units.is_multiple_of(8) {
                    return Err(EngineStatus::InvalidArgument);
                }
                (units / 8).checked_mul(u64::from(self.channel_count))
            }
            _ => return Err(EngineStatus::UnsupportedFormat),
        };
        bytes.ok_or(EngineStatus::LimitExceeded)
    }
}

#[derive(Default)]
struct RenderGate {
    state: AtomicUsize,
}

struct RenderLease<'a> {
    gate: &'a RenderGate,
}

impl RenderGate {
    fn enter(&self) -> Option<RenderLease<'_>> {
        let mut state = self.state.load(Ordering::Acquire);
        loop {
            if state & CLOSING != 0 {
                return None;
            }
            let active = state & ACTIVE_MASK;
            if active == ACTIVE_MASK {
                return None;
            }
            match self.state.compare_exchange_weak(
                state,
                state + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Some(RenderLease { gate: self }),
                Err(actual) => state = actual,
            }
        }
    }

    fn begin_close(&self) {
        self.state.fetch_or(CLOSING, Ordering::AcqRel);
    }

    fn wait_quiescent(&self) {
        while self.state.load(Ordering::Acquire) & ACTIVE_MASK != 0 {
            thread::yield_now();
        }
    }

    fn quiesce(&self) {
        self.begin_close();
        self.wait_quiescent();
    }
}

impl Drop for RenderLease<'_> {
    fn drop(&mut self) {
        self.gate.state.fetch_sub(1, Ordering::Release);
    }
}

pub struct CamelEngine {
    format: AudioFormat,
    output_format: CamelAudioFormatV1,
    max_render_units: u64,
    render_gate: RenderGate,
    command: Mutex<CamelCommandStampV1>,
    source: Mutex<Option<DecoderSource>>,
}

fn validate_config(config: &CamelEngineConfigV1) -> Result<(AudioFormat, u64), EngineStatus> {
    if config.abi_version != ABI_VERSION || config.format.abi_version != ABI_VERSION {
        return Err(EngineStatus::AbiMismatch);
    }
    if config.struct_size < std::mem::size_of::<CamelEngineConfigV1>() as u32 {
        return Err(EngineStatus::StructSize);
    }
    if config.max_render_units == 0 || config.max_render_units > MAX_RENDER_UNITS {
        return Err(EngineStatus::LimitExceeded);
    }
    Ok((
        AudioFormat::from_abi(&config.format)?,
        config.max_render_units,
    ))
}

pub extern "C" fn camel_engine_abi_version() -> u32 {
    ABI_VERSION
}

/// # Safety
/// `config` must point to a readable V1 configuration and `out_engine` to writable storage.
pub unsafe extern "C" fn camel_engine_create_v1(
    config: *const CamelEngineConfigV1,
    out_engine: *mut *mut CamelEngine,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if config.is_null() || out_engine.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        *out_engine = ptr::null_mut();
        match validate_config(&*config) {
            Ok((format, max_render_units)) => {
                *out_engine = Box::into_raw(Box::new(CamelEngine {
                    format,
                    output_format: (*config).format,
                    max_render_units,
                    render_gate: RenderGate::default(),
                    command: Mutex::new(CamelCommandStampV1 {
                        struct_size: std::mem::size_of::<CamelCommandStampV1>() as u32,
                        abi_version: ABI_VERSION,
                        command_id: 0,
                        generation: 0,
                        queue_revision: 0,
                    }),
                    source: Mutex::new(None),
                }));
                ENGINE_OK
            }
            Err(status) => status as i32,
        }
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

fn validate_command(command: &CamelCommandStampV1) -> Result<(), EngineStatus> {
    if command.abi_version != ABI_VERSION {
        return Err(EngineStatus::AbiMismatch);
    }
    if command.struct_size < std::mem::size_of::<CamelCommandStampV1>() as u32 {
        return Err(EngineStatus::StructSize);
    }
    Ok(())
}

/// # Safety
/// All pointers must reference valid V1 structures. Decoder callbacks and their context must remain valid until the source is replaced or the Engine is destroyed.
pub unsafe extern "C" fn camel_engine_load_flac_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
    source: *const CamelTrackSourceV1,
    decoder_kind: u32,
    decoder_host: *const CamelDecoderHostV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || command.is_null() || source.is_null() || decoder_host.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        let command = &*command;
        let source = &*source;
        let decoder_host = &*decoder_host;
        if let Err(status) = validate_command(command) {
            return status as i32;
        }
        if source.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if source.struct_size < std::mem::size_of::<CamelTrackSourceV1>() as u32
            || decoder_host.abi_version != ABI_VERSION
            || decoder_host.struct_size < std::mem::size_of::<CamelDecoderHostV1>() as u32
        {
            return ENGINE_E_STRUCT_SIZE;
        }
        let Some(host) = DecoderHost::from_abi(decoder_host) else {
            return ENGINE_E_INVALID_ARGUMENT;
        };
        let new_source = match DecoderSource::open(source, decoder_kind, host) {
            Ok(source) => source,
            Err(status) => return status,
        };
        let mut current_command = engine.command.lock().unwrap();
        let mut current_source = engine.source.lock().unwrap();
        *current_source = Some(new_source);
        *current_command = *command;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call.
pub unsafe extern "C" fn camel_engine_seek_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
    frame: u64,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || command.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        let command = &*command;
        if let Err(status) = validate_command(command) {
            return status as i32;
        }
        let mut current_command = engine.command.lock().unwrap();
        let mut source = engine.source.lock().unwrap();
        let Some(source) = source.as_mut() else {
            return ENGINE_E_INVALID_STATE;
        };
        if let Err(status) = source.seek(frame) {
            return status;
        }
        *current_command = *command;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine`, `output`, and `frames_read` must reference valid storage for this call.
pub unsafe extern "C" fn camel_engine_read_decoded_v1(
    engine: *mut CamelEngine,
    output: *mut i32,
    frame_capacity: u64,
    frames_read: *mut u64,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || frames_read.is_null() || (frame_capacity > 0 && output.is_null()) {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        *frames_read = 0;
        if frame_capacity == 0 {
            return ENGINE_OK;
        }
        if frame_capacity > engine.max_render_units {
            return ENGINE_E_LIMIT_EXCEEDED;
        }
        let mut source = engine.source.lock().unwrap();
        let Some(source) = source.as_mut() else {
            return ENGINE_E_INVALID_STATE;
        };
        let sample_capacity = match frame_capacity
            .checked_mul(u64::from(source.info.channel_count))
            .and_then(|samples| usize::try_from(samples).ok())
        {
            Some(samples) => samples,
            None => return ENGINE_E_LIMIT_EXCEEDED,
        };
        let output = if sample_capacity == 0 {
            &mut []
        } else {
            std::slice::from_raw_parts_mut(output, sample_capacity)
        };
        match source.read(output, frame_capacity) {
            Ok(read) => {
                *frames_read = read;
                if read == 0 {
                    ENGINE_END
                } else {
                    ENGINE_OK
                }
            }
            Err(status) => status,
        }
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `state` must reference a live handle and writable V1 state.
pub unsafe extern "C" fn camel_engine_get_state_v1(
    engine: *mut CamelEngine,
    state: *mut CamelEngineStateV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || state.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        let state = &mut *state;
        if state.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if state.struct_size < std::mem::size_of::<CamelEngineStateV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        let command = *engine.command.lock().unwrap();
        let source = engine.source.lock().unwrap();
        state.playback_state = if source.is_some() { 1 } else { 0 };
        state.last_error_code = 0;
        state.command = command;
        state.position_frames = source.as_ref().map_or(0, DecoderSource::position);
        state.total_frames = source.as_ref().map_or(0, |s| s.info.total_frames);
        state.has_source = u32::from(source.is_some());
        state.reserved = 0;
        state.source_format = source
            .as_ref()
            .map_or(empty_format(), |s| source_audio_format(&s.info));
        state.output_format = engine.output_format;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

fn empty_format() -> CamelAudioFormatV1 {
    CamelAudioFormatV1 {
        struct_size: std::mem::size_of::<CamelAudioFormatV1>() as u32,
        abi_version: ABI_VERSION,
        stream_kind: AUDIO_PCM,
        sample_format: 0,
        rate_hz: 0,
        channel_count: 0,
        valid_bits: 0,
        container_bits: 0,
        channel_mask: 0,
    }
}

fn source_audio_format(info: &CamelSourceInfoV1) -> CamelAudioFormatV1 {
    let sample_format = if info.valid_bits == 16 {
        SAMPLE_S16_LE
    } else {
        SAMPLE_S24_LE
    };
    CamelAudioFormatV1 {
        struct_size: std::mem::size_of::<CamelAudioFormatV1>() as u32,
        abi_version: ABI_VERSION,
        stream_kind: AUDIO_PCM,
        sample_format,
        rate_hz: info.sample_rate,
        channel_count: info.channel_count,
        valid_bits: info.valid_bits,
        container_bits: info.valid_bits,
        channel_mask: 0,
    }
}

/// # Safety
/// The handle and structures must remain valid for the call. No new call may begin after quiesce starts.
pub unsafe extern "C" fn camel_engine_render_v1(
    engine: *mut CamelEngine,
    request: *const CamelRenderRequestV1,
    result: *mut CamelRenderResultV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        let Some(_lease) = engine.render_gate.enter() else {
            return ENGINE_E_INVALID_STATE;
        };
        if request.is_null() || result.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let request = &*request;
        let result = &mut *result;
        if result.struct_size < std::mem::size_of::<CamelRenderResultV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        if result.abi_version != ABI_VERSION || request.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if request.struct_size < std::mem::size_of::<CamelRenderRequestV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }

        result.units_rendered = 0;
        result.required_bytes = 0;
        result.state = RENDER_STATE_NO_SOURCE;
        result.reserved = 0;

        if request.units > engine.max_render_units {
            return ENGINE_E_LIMIT_EXCEEDED;
        }
        let required_bytes = match engine.format.required_bytes(request.units) {
            Ok(bytes) => bytes,
            Err(status) => return status as i32,
        };
        result.required_bytes = required_bytes;
        if request.output_capacity_bytes < required_bytes
            || (required_bytes > 0 && request.output.is_null())
        {
            return ENGINE_E_BUFFER_TOO_SMALL;
        }
        ENGINE_E_NOT_READY
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` must point to a live handle. The caller must prevent new render calls before quiesce.
pub unsafe extern "C" fn camel_engine_quiesce_v1(engine: *mut CamelEngine) -> i32 {
    if engine.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| (&*engine).render_gate.quiesce())) {
        Ok(()) => ENGINE_OK,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` must be live, quiesced, and inaccessible to all other calls.
pub unsafe extern "C" fn camel_engine_destroy_v1(engine: *mut CamelEngine) -> i32 {
    if engine.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        (&*engine).render_gate.quiesce();
        drop(Box::from_raw(engine));
    })) {
        Ok(()) => ENGINE_OK,
        Err(_) => ENGINE_E_PANIC,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::c_void;
    use std::sync::mpsc;
    use std::time::Duration;

    struct MockDecoder {
        samples: Vec<i32>,
        position: usize,
    }

    unsafe extern "C" fn mock_open(
        _: *mut c_void,
        _: u32,
        _: *const u8,
        _: u64,
        out_decoder: *mut *mut c_void,
        info: *mut CamelSourceInfoV1,
    ) -> i32 {
        let decoder = Box::new(MockDecoder {
            samples: vec![1, -1, 2, -2, 3, -3, 4, -4],
            position: 0,
        });
        *out_decoder = Box::into_raw(decoder).cast();
        *info = CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: ABI_VERSION,
            sample_rate: 48_000,
            channel_count: 2,
            valid_bits: 16,
            reserved: 0,
            total_frames: 4,
            has_md5: 0,
            reserved2: 0,
            md5: [0; 16],
        };
        0
    }

    unsafe extern "C" fn mock_read(
        _: *mut c_void,
        opaque: *mut c_void,
        output: *mut i32,
        frames: u64,
        frames_read: *mut u64,
    ) -> i32 {
        let decoder = &mut *opaque.cast::<MockDecoder>();
        let available = decoder.samples.len() / 2 - decoder.position;
        let read = available.min(frames as usize);
        std::ptr::copy_nonoverlapping(
            decoder.samples.as_ptr().add(decoder.position * 2),
            output,
            read * 2,
        );
        decoder.position += read;
        *frames_read = read as u64;
        0
    }

    unsafe extern "C" fn mock_seek(_: *mut c_void, opaque: *mut c_void, frame: u64) -> i32 {
        let decoder = &mut *opaque.cast::<MockDecoder>();
        if frame > 4 {
            return -5;
        }
        decoder.position = frame as usize;
        0
    }

    unsafe extern "C" fn mock_close(_: *mut c_void, opaque: *mut c_void) {
        drop(Box::from_raw(opaque.cast::<MockDecoder>()));
    }

    fn mock_host() -> CamelDecoderHostV1 {
        CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: std::ptr::null_mut(),
            open: Some(mock_open),
            read: Some(mock_read),
            seek: Some(mock_seek),
            close: Some(mock_close),
        }
    }

    fn format(
        stream_kind: u32,
        sample_format: u32,
        valid_bits: u32,
        container_bits: u32,
    ) -> CamelAudioFormatV1 {
        CamelAudioFormatV1 {
            struct_size: std::mem::size_of::<CamelAudioFormatV1>() as u32,
            abi_version: ABI_VERSION,
            stream_kind,
            sample_format,
            rate_hz: 96_000,
            channel_count: 2,
            valid_bits,
            container_bits,
            channel_mask: 3,
        }
    }

    fn config(format: CamelAudioFormatV1) -> CamelEngineConfigV1 {
        CamelEngineConfigV1 {
            struct_size: std::mem::size_of::<CamelEngineConfigV1>() as u32,
            abi_version: ABI_VERSION,
            max_render_units: MAX_RENDER_UNITS,
            format,
        }
    }

    #[test]
    fn abi_layout_matches_the_c_header_contract() {
        assert_eq!(camel_engine_abi_version(), ABI_VERSION);
        assert_eq!(std::mem::size_of::<CamelAudioFormatV1>(), 40);
        assert_eq!(std::mem::size_of::<CamelEngineConfigV1>(), 56);
        assert_eq!(std::mem::size_of::<CamelCommandStampV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelTrackSourceV1>(), 48);
        assert_eq!(std::mem::size_of::<CamelSourceInfoV1>(), 56);
        assert_eq!(std::mem::size_of::<CamelDecoderHostV1>(), 48);
        assert_eq!(std::mem::size_of::<CamelRenderRequestV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelRenderResultV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelEngineStateV1>(), 152);
        assert_eq!(std::mem::offset_of!(CamelAudioFormatV1, channel_mask), 32);
        assert_eq!(std::mem::offset_of!(CamelEngineConfigV1, format), 16);
        assert_eq!(std::mem::offset_of!(CamelCommandStampV1, command_id), 8);
        assert_eq!(std::mem::offset_of!(CamelTrackSourceV1, start_frame), 24);
        assert_eq!(std::mem::offset_of!(CamelRenderRequestV1, output), 16);
        assert_eq!(std::mem::offset_of!(CamelRenderResultV1, state), 24);
        assert_eq!(std::mem::offset_of!(CamelEngineStateV1, source_format), 72);
    }

    #[test]
    fn rejects_abi_version_mismatch_and_short_structs() {
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24));
        cfg.abi_version = ABI_VERSION + 1;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_E_ABI_MISMATCH
        );
        assert!(engine.is_null());

        cfg = config(format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24));
        cfg.format.struct_size -= 1;
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_E_STRUCT_SIZE
        );
        assert!(engine.is_null());
    }

    #[test]
    fn computes_capacity_in_pcm_frames_dop_frames_and_dsd_bit_frames() {
        let pcm = AudioFormat::from_abi(&format(AUDIO_PCM, SAMPLE_S24_IN_32_LE, 24, 32)).unwrap();
        assert_eq!(pcm.required_bytes(128), Ok(1024));

        let dop = AudioFormat::from_abi(&format(AUDIO_DOP, SAMPLE_DOP_S24_LE, 24, 24)).unwrap();
        assert_eq!(dop.required_bytes(128), Ok(768));

        let dsd = AudioFormat::from_abi(&format(AUDIO_DSD, SAMPLE_DSD_U8, 1, 8)).unwrap();
        assert_eq!(dsd.required_bytes(1024), Ok(256));
        assert_eq!(dsd.required_bytes(9), Err(EngineStatus::InvalidArgument));
    }

    #[test]
    fn rejects_short_render_buffer_before_accessing_it() {
        let audio_format = format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24);
        let cfg = config(audio_format);
        let mut engine = ptr::null_mut();
        let status = unsafe { camel_engine_create_v1(&cfg, &mut engine) };
        assert_eq!(status, ENGINE_OK);

        let request = CamelRenderRequestV1 {
            struct_size: std::mem::size_of::<CamelRenderRequestV1>() as u32,
            abi_version: ABI_VERSION,
            units: 32,
            output: ptr::dangling_mut::<u8>(),
            output_capacity_bytes: 1,
        };
        let mut result = CamelRenderResultV1 {
            struct_size: std::mem::size_of::<CamelRenderResultV1>() as u32,
            abi_version: ABI_VERSION,
            units_rendered: u64::MAX,
            required_bytes: 0,
            state: 0,
            reserved: 0,
        };
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_E_BUFFER_TOO_SMALL
        );
        assert_eq!(result.required_bytes, 32 * 2 * 3);
        assert_eq!(result.units_rendered, 0);
        assert_eq!(unsafe { camel_engine_quiesce_v1(engine) }, ENGINE_OK);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn quiesce_rejects_new_renders_and_waits_for_active_render_lease() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let engine_ref = unsafe { &*engine };
        let active = engine_ref.render_gate.enter().unwrap();
        engine_ref.render_gate.begin_close();
        let engine_address = engine as usize;
        let (started_tx, started_rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let closer = thread::spawn(move || {
            started_tx.send(()).unwrap();
            let status = unsafe { camel_engine_quiesce_v1(engine_address as *mut CamelEngine) };
            done_tx.send(status).unwrap();
        });

        started_rx.recv().unwrap();
        let request = CamelRenderRequestV1 {
            struct_size: std::mem::size_of::<CamelRenderRequestV1>() as u32,
            abi_version: ABI_VERSION,
            units: 1,
            output: ptr::dangling_mut::<u8>(),
            output_capacity_bytes: 4,
        };
        let mut result = CamelRenderResultV1 {
            struct_size: std::mem::size_of::<CamelRenderResultV1>() as u32,
            abi_version: ABI_VERSION,
            units_rendered: 0,
            required_bytes: 0,
            state: RENDER_STATE_NO_SOURCE,
            reserved: 0,
        };
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_E_INVALID_STATE
        );
        assert!(done_rx.recv_timeout(Duration::from_millis(20)).is_err());
        drop(active);
        assert_eq!(
            done_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
            ENGINE_OK
        );
        closer.join().unwrap();
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn source_host_reads_seeks_and_honors_track_ranges() {
        let path = b"mock.flac";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 1,
            end_frame_exclusive: 3,
            has_end_frame: 1,
            reserved: 0,
        };
        let host_abi = mock_host();
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_FFMPEG, host) }.unwrap();
        let mut output = [0; 8];
        assert_eq!(decoded.read(&mut output, 4), Ok(2));
        assert_eq!(output, [2, -2, 3, -3, 0, 0, 0, 0]);
        assert_eq!(decoded.read(&mut output, 4), Ok(0));
        assert_eq!(decoded.seek(2), Ok(()));
        assert_eq!(decoded.read(&mut output, 1), Ok(1));
        assert_eq!(&output[..2], &[3, -3]);
        assert_eq!(decoded.seek(3), Err(ENGINE_E_SEEK));
    }

    #[test]
    fn source_host_rejects_empty_paths() {
        let path = b"mock.flac";
        let mut source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let host_abi = mock_host();
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        assert!(
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_SYMPHONIA, host) }.is_ok()
        );
        source.path_length_bytes = 0;
        assert_eq!(
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_SYMPHONIA, host) }.err(),
            Some(ENGINE_E_INVALID_ARGUMENT)
        );
    }
}
