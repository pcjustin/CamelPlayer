use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, AtomicUsize, Ordering};
use std::sync::Mutex;
use std::thread;

mod decoder_host;
mod packing;
mod ring;

pub use decoder_host::{CamelDecoderHostV1, CamelSourceInfoV1};
use decoder_host::{DecoderHost, DecoderSource};
use packing::{bytes_per_frame, pack_pcm};
use ring::SpscByteRing;

pub const ABI_VERSION: u32 = 1;
pub const MAX_RENDER_UNITS: u64 = 65_536;
const MAX_PREFILL_FRAMES: u64 = 4_096;

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
pub const RENDER_STATE_ACTIVE: u32 = 1;
pub const RENDER_STATE_END_OF_STREAM: u32 = 2;
pub const RENDER_STATE_FAULT: u32 = 3;
pub const RENDER_STATE_UNDERRUN: u32 = 4;

pub const PLAYBACK_EMPTY: u32 = 0;
pub const PLAYBACK_READY: u32 = 1;
pub const PLAYBACK_PLAYING: u32 = 2;
pub const PLAYBACK_PAUSED: u32 = 3;
pub const PLAYBACK_STOPPED: u32 = 4;
pub const PLAYBACK_FAILED: u32 = 5;
pub use decoder_host::{
    FLAC_INTEGRITY_FAILED, FLAC_INTEGRITY_INCOMPLETE, FLAC_INTEGRITY_UNAVAILABLE,
    FLAC_INTEGRITY_VERIFIED,
};

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

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelFlacIntegrityV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub status: u32,
    pub has_reference_md5: u32,
    pub total_frames: u64,
    pub position_frames: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelEngineTimelineV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub playback_state: u32,
    pub reserved: u32,
    pub command: CamelCommandStampV1,
    pub decoded_position_frames: u64,
    pub submitted_position_frames: u64,
    pub audible_position_frames: u64,
    pub total_frames: u64,
    pub has_source: u32,
    pub reserved2: u32,
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

#[derive(Default)]
struct TryGate {
    active: AtomicBool,
}

struct TryLease<'a> {
    gate: &'a TryGate,
}

impl TryGate {
    fn enter(&self) -> Option<TryLease<'_>> {
        self.active
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .ok()
            .map(|_| TryLease { gate: self })
    }
}

impl Drop for TryLease<'_> {
    fn drop(&mut self) {
        self.gate.active.store(false, Ordering::Release);
    }
}

impl RenderGate {
    fn enter(&self) -> Option<RenderLease<'_>> {
        let state = self.state.load(Ordering::Acquire);
        if state & CLOSING != 0 || state & ACTIVE_MASK == ACTIVE_MASK {
            return None;
        }
        self.state
            .compare_exchange(state, state + 1, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| RenderLease { gate: self })
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
    ring: SpscByteRing,
    render_gate: RenderGate,
    prefill_gate: TryGate,
    consumer_gate: TryGate,
    render_source_state: AtomicUsize,
    render_error: AtomicI32,
    playback_state: AtomicUsize,
    operation_epoch: AtomicU64,
    issued_command_id: AtomicU64,
    submitted_position: AtomicU64,
    audible_position: AtomicU64,
    command: Mutex<CamelCommandStampV1>,
    source: Mutex<Option<DecoderSource>>,
}

impl CamelEngine {
    fn fail_render(&self, status: i32) {
        self.render_error.store(status, Ordering::Release);
        self.playback_state
            .store(PLAYBACK_FAILED as usize, Ordering::Release);
        self.render_source_state
            .store(RENDER_STATE_FAULT as usize, Ordering::Release);
    }
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
                let ring_units = if format.stream_kind == AUDIO_DSD {
                    max_render_units - max_render_units % 8
                } else {
                    max_render_units
                };
                let ring_capacity = match format
                    .required_bytes(ring_units)
                    .ok()
                    .and_then(|bytes| usize::try_from(bytes.max(1)).ok())
                {
                    Some(capacity) => capacity,
                    None => return ENGINE_E_LIMIT_EXCEEDED,
                };
                let ring = match SpscByteRing::new(ring_capacity) {
                    Ok(ring) => ring,
                    Err(()) => return ENGINE_E_LIMIT_EXCEEDED,
                };
                *out_engine = Box::into_raw(Box::new(CamelEngine {
                    format,
                    output_format: (*config).format,
                    max_render_units,
                    ring,
                    render_gate: RenderGate::default(),
                    prefill_gate: TryGate::default(),
                    consumer_gate: TryGate::default(),
                    render_source_state: AtomicUsize::new(RENDER_STATE_NO_SOURCE as usize),
                    render_error: AtomicI32::new(ENGINE_OK),
                    playback_state: AtomicUsize::new(PLAYBACK_EMPTY as usize),
                    operation_epoch: AtomicU64::new(0),
                    issued_command_id: AtomicU64::new(0),
                    submitted_position: AtomicU64::new(0),
                    audible_position: AtomicU64::new(0),
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

fn begin_command(engine: &CamelEngine, command: &CamelCommandStampV1) -> Result<u64, EngineStatus> {
    validate_command(command)?;
    let mut current = engine.command.lock().unwrap();
    if command.command_id <= engine.issued_command_id.load(Ordering::Acquire) {
        return Err(EngineStatus::NotReady);
    }
    *current = *command;
    engine
        .issued_command_id
        .store(command.command_id, Ordering::Release);
    Ok(engine.operation_epoch.fetch_add(1, Ordering::AcqRel) + 1)
}

fn operation_is_current(engine: &CamelEngine, epoch: u64) -> bool {
    engine.operation_epoch.load(Ordering::Acquire) == epoch
}

/// # Safety
/// All pointers must reference valid V1 structures. Stop and drain render before replacing the source. Decoder callbacks and their context must remain valid until replacement or destruction. Commands must have strictly increasing IDs.
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
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let new_source = match DecoderSource::open(source, decoder_kind, host) {
            Ok(source) => source,
            Err(status) => return status,
        };
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let position = new_source.position();
        let mut current_source = engine.source.lock().unwrap();
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        engine.ring.clear();
        engine.submitted_position.store(position, Ordering::Release);
        engine.audible_position.store(position, Ordering::Release);
        engine.render_error.store(ENGINE_OK, Ordering::Release);
        engine
            .render_source_state
            .store(RENDER_STATE_ACTIVE as usize, Ordering::Release);
        *current_source = Some(new_source);
        engine
            .playback_state
            .store(PLAYBACK_READY as usize, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call. Stop and drain render before seeking. Commands must have strictly increasing IDs.
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
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let mut source = engine.source.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let Some(source) = source.as_mut() else {
            return ENGINE_E_INVALID_STATE;
        };
        if !source.can_seek(frame) {
            return ENGINE_E_SEEK;
        }
        if let Err(status) = source.seek(frame) {
            // The decoder may have moved, so buffered frames and positions no longer describe it.
            if operation_is_current(engine, epoch) {
                engine.fail_render(status);
            }
            return status;
        }
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        engine.ring.clear();
        engine.submitted_position.store(frame, Ordering::Release);
        engine.audible_position.store(frame, Ordering::Release);
        engine.render_error.store(ENGINE_OK, Ordering::Release);
        engine
            .render_source_state
            .store(RENDER_STATE_ACTIVE as usize, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call. Commands must have strictly increasing IDs.
pub unsafe extern "C" fn camel_engine_play_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
) -> i32 {
    if engine.is_null() || command.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let engine = &*engine;
        let command = &*command;
        if let Err(status) = validate_command(command) {
            return status as i32;
        }
        if engine.source.lock().unwrap().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        engine
            .playback_state
            .store(PLAYBACK_PLAYING as usize, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call. Commands must have strictly increasing IDs.
pub unsafe extern "C" fn camel_engine_pause_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
) -> i32 {
    if engine.is_null() || command.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let engine = &*engine;
        let command = &*command;
        if let Err(status) = validate_command(command) {
            return status as i32;
        }
        if engine.source.lock().unwrap().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        engine
            .playback_state
            .store(PLAYBACK_PAUSED as usize, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call. Stop and drain render before calling. Commands must have strictly increasing IDs.
pub unsafe extern "C" fn camel_engine_stop_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
) -> i32 {
    if engine.is_null() || command.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let engine = &*engine;
        let command = &*command;
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let mut source = engine.source.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let start_frame = if let Some(source) = source.as_mut() {
            let start_frame = source.start_frame();
            if let Err(status) = source.seek(start_frame) {
                engine
                    .playback_state
                    .store(PLAYBACK_FAILED as usize, Ordering::Release);
                return status;
            }
            Some(start_frame)
        } else {
            None
        };
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let position = start_frame.unwrap_or(0);
        engine.submitted_position.store(position, Ordering::Release);
        engine.audible_position.store(position, Ordering::Release);
        engine.render_source_state.store(
            if start_frame.is_some() {
                RENDER_STATE_ACTIVE
            } else {
                RENDER_STATE_NO_SOURCE
            } as usize,
            Ordering::Release,
        );
        engine.ring.clear();
        engine.render_error.store(ENGINE_OK, Ordering::Release);
        engine
            .playback_state
            .store(PLAYBACK_STOPPED as usize, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// Call from one decode worker only. Stop and drain render before replacing or seeking the source.
pub unsafe extern "C" fn camel_engine_prefill_v1(
    engine: *mut CamelEngine,
    frame_limit: u64,
    frames_prefilled: *mut u64,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || frames_prefilled.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        *frames_prefilled = 0;
        if frame_limit > engine.max_render_units {
            return ENGINE_E_LIMIT_EXCEEDED;
        }
        if engine.format.stream_kind != AUDIO_PCM {
            return ENGINE_E_UNSUPPORTED_FORMAT;
        }
        let Some(_producer) = engine.prefill_gate.enter() else {
            return ENGINE_E_INVALID_STATE;
        };
        let epoch = engine.operation_epoch.load(Ordering::Acquire);
        let Some(bytes_per_frame) = bytes_per_frame(&engine.format) else {
            return ENGINE_E_UNSUPPORTED_FORMAT;
        };

        let mut source = engine.source.lock().unwrap();
        let Some(source) = source.as_mut() else {
            return ENGINE_E_INVALID_STATE;
        };
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        if source.info.channel_count != engine.format.channel_count
            || pack_pcm(&[], source.info.valid_bits, &engine.format, &mut []).is_err()
        {
            engine.fail_render(ENGINE_E_UNSUPPORTED_FORMAT);
            return ENGINE_E_UNSUPPORTED_FORMAT;
        }

        let writable_frames = engine.ring.writable() / bytes_per_frame;
        let frame_limit = frame_limit
            .min(MAX_PREFILL_FRAMES)
            .min(writable_frames as u64);
        if frame_limit == 0 {
            return ENGINE_OK;
        }
        let sample_count = match frame_limit
            .checked_mul(u64::from(source.info.channel_count))
            .and_then(|count| usize::try_from(count).ok())
        {
            Some(count) => count,
            None => return ENGINE_E_LIMIT_EXCEEDED,
        };
        let mut samples = Vec::new();
        if samples.try_reserve_exact(sample_count).is_err() {
            engine.fail_render(ENGINE_E_LIMIT_EXCEEDED);
            return ENGINE_E_LIMIT_EXCEEDED;
        }
        samples.resize(sample_count, 0);
        let start_position = source.position();
        let frames_read = match source.read(&mut samples, frame_limit) {
            Ok(frames_read) => frames_read,
            Err(status) => {
                if !operation_is_current(engine, epoch) {
                    let _ = source.seek(start_position);
                    return ENGINE_E_NOT_READY;
                }
                engine.fail_render(status);
                return status;
            }
        };
        if !operation_is_current(engine, epoch) {
            let _ = source.seek(start_position);
            return ENGINE_E_NOT_READY;
        }
        if frames_read == 0 {
            let _current_command = engine.command.lock().unwrap();
            if !operation_is_current(engine, epoch) {
                return ENGINE_E_NOT_READY;
            }
            engine
                .render_source_state
                .store(RENDER_STATE_END_OF_STREAM as usize, Ordering::Release);
            return ENGINE_END;
        }

        let sample_count = match frames_read
            .checked_mul(u64::from(source.info.channel_count))
            .and_then(|count| usize::try_from(count).ok())
        {
            Some(count) => count,
            None => return ENGINE_E_LIMIT_EXCEEDED,
        };
        let output_bytes = match frames_read
            .checked_mul(bytes_per_frame as u64)
            .and_then(|bytes| usize::try_from(bytes).ok())
        {
            Some(bytes) => bytes,
            None => return ENGINE_E_LIMIT_EXCEEDED,
        };
        let mut packed = Vec::new();
        if packed.try_reserve_exact(output_bytes).is_err() {
            engine.fail_render(ENGINE_E_LIMIT_EXCEEDED);
            return ENGINE_E_LIMIT_EXCEEDED;
        }
        packed.resize(output_bytes, 0);
        let written = match pack_pcm(
            &samples[..sample_count],
            source.info.valid_bits,
            &engine.format,
            &mut packed,
        ) {
            Ok(written) => written,
            Err(status) => {
                if !operation_is_current(engine, epoch) {
                    let _ = source.seek(start_position);
                    return ENGINE_E_NOT_READY;
                }
                engine.fail_render(status);
                return status;
            }
        };
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            let _ = source.seek(start_position);
            return ENGINE_E_NOT_READY;
        }
        if engine.ring.write(&packed[..written]) != written {
            engine.fail_render(ENGINE_E_INVALID_STATE);
            return ENGINE_E_INVALID_STATE;
        }
        *frames_prefilled = frames_read;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => {
            if !engine.is_null() {
                (&*engine).fail_render(ENGINE_E_PANIC);
            }
            ENGINE_E_PANIC
        }
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
        let epoch = engine.operation_epoch.load(Ordering::Acquire);
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
        let start_position = source.position();
        match source.read(output, frame_capacity) {
            Ok(read) => {
                let _current_command = engine.command.lock().unwrap();
                if !operation_is_current(engine, epoch) {
                    let _ = source.seek(start_position);
                    output.fill(0);
                    return ENGINE_E_NOT_READY;
                }
                *frames_read = read;
                engine
                    .submitted_position
                    .store(source.position(), Ordering::Release);
                engine
                    .audible_position
                    .store(source.position(), Ordering::Release);
                if read == 0 {
                    ENGINE_END
                } else {
                    ENGINE_OK
                }
            }
            Err(status) => {
                if !operation_is_current(engine, epoch) {
                    ENGINE_E_NOT_READY
                } else {
                    status
                }
            }
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
        state.playback_state = engine.playback_state.load(Ordering::Acquire) as u32;
        state.last_error_code = engine.render_error.load(Ordering::Acquire);
        state.command = command;
        state.position_frames = engine.submitted_position.load(Ordering::Acquire);
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

/// # Safety
/// `engine` and `timeline` must reference a live engine and writable V1 timeline.
pub unsafe extern "C" fn camel_engine_get_timeline_v1(
    engine: *mut CamelEngine,
    timeline: *mut CamelEngineTimelineV1,
) -> i32 {
    if engine.is_null() || timeline.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let engine = &*engine;
        let timeline = &mut *timeline;
        if timeline.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if timeline.struct_size < std::mem::size_of::<CamelEngineTimelineV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        let command = *engine.command.lock().unwrap();
        let source = engine.source.lock().unwrap();
        timeline.playback_state = engine.playback_state.load(Ordering::Acquire) as u32;
        timeline.reserved = 0;
        timeline.command = command;
        timeline.decoded_position_frames = source.as_ref().map_or(0, DecoderSource::position);
        timeline.submitted_position_frames = engine.submitted_position.load(Ordering::Acquire);
        timeline.audible_position_frames = engine.audible_position.load(Ordering::Acquire);
        timeline.total_frames = source.as_ref().map_or(0, |s| s.info.total_frames);
        timeline.has_source = u32::from(source.is_some());
        timeline.reserved2 = 0;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid. `audible_frame` is the output bridge's estimated audible track position.
pub unsafe extern "C" fn camel_engine_report_audible_position_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
    audible_frame: u64,
) -> i32 {
    if engine.is_null() || command.is_null() {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    match catch_unwind(AssertUnwindSafe(|| {
        let engine = &*engine;
        let command = &*command;
        if let Err(status) = validate_command(command) {
            return status as i32;
        }
        let current = engine.command.lock().unwrap();
        if current.command_id != command.command_id
            || current.generation != command.generation
            || current.queue_revision != command.queue_revision
        {
            return ENGINE_E_NOT_READY;
        }
        let submitted = engine.submitted_position.load(Ordering::Acquire);
        engine
            .audible_position
            .store(audible_frame.min(submitted), Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `integrity` must reference a live handle and writable V1 state.
pub unsafe extern "C" fn camel_engine_get_flac_integrity_v1(
    engine: *mut CamelEngine,
    integrity: *mut CamelFlacIntegrityV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || integrity.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let integrity = &mut *integrity;
        if integrity.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if integrity.struct_size < std::mem::size_of::<CamelFlacIntegrityV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        let source = (&*engine).source.lock().unwrap();
        let Some(source) = source.as_ref() else {
            integrity.status = FLAC_INTEGRITY_UNAVAILABLE;
            integrity.has_reference_md5 = 0;
            integrity.total_frames = 0;
            integrity.position_frames = 0;
            return ENGINE_OK;
        };
        integrity.status = source.integrity_status();
        integrity.has_reference_md5 = u32::from(source.has_reference_md5());
        integrity.total_frames = source.info.total_frames;
        integrity.position_frames = source.position();
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
        let Some(_consumer) = engine.consumer_gate.enter() else {
            return ENGINE_E_INVALID_STATE;
        };
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

        let source_state = engine.render_source_state.load(Ordering::Acquire) as u32;
        if source_state == RENDER_STATE_NO_SOURCE {
            return ENGINE_E_NOT_READY;
        }
        if source_state == RENDER_STATE_FAULT {
            if engine.format.stream_kind == AUDIO_PCM && required_bytes > 0 {
                let Ok(output_bytes) = usize::try_from(required_bytes) else {
                    return ENGINE_E_LIMIT_EXCEEDED;
                };
                ptr::write_bytes(request.output, 0, output_bytes);
            }
            result.state = RENDER_STATE_FAULT;
            return engine.render_error.load(Ordering::Acquire);
        }
        if engine.format.stream_kind != AUDIO_PCM {
            return ENGINE_E_UNSUPPORTED_FORMAT;
        }
        let Some(bytes_per_frame) = bytes_per_frame(&engine.format) else {
            return ENGINE_E_UNSUPPORTED_FORMAT;
        };
        let output_bytes = match usize::try_from(required_bytes) {
            Ok(bytes) => bytes,
            Err(_) => return ENGINE_E_LIMIT_EXCEEDED,
        };
        if output_bytes > 0 {
            ptr::write_bytes(request.output, 0, output_bytes);
        }
        let available = engine.ring.readable().min(output_bytes);
        let rendered_bytes = if available > 0 {
            let output = std::slice::from_raw_parts_mut(request.output, output_bytes);
            engine.ring.read(&mut output[..available])
        } else {
            0
        };
        let rendered_frames = rendered_bytes / bytes_per_frame;
        result.units_rendered = rendered_frames as u64;
        engine
            .submitted_position
            .fetch_add(rendered_frames as u64, Ordering::AcqRel);

        let source_state = engine.render_source_state.load(Ordering::Acquire) as u32;
        if source_state == RENDER_STATE_END_OF_STREAM && engine.ring.readable() == 0 {
            result.state = RENDER_STATE_END_OF_STREAM;
            engine
                .playback_state
                .store(PLAYBACK_STOPPED as usize, Ordering::Release);
            if rendered_frames < request.units as usize {
                return ENGINE_END;
            }
        } else if rendered_frames < request.units as usize {
            result.state = RENDER_STATE_UNDERRUN;
            return ENGINE_UNDERRUN;
        } else {
            result.state = RENDER_STATE_ACTIVE;
        }
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => {
            if !engine.is_null() {
                (&*engine).fail_render(ENGINE_E_PANIC);
            }
            ENGINE_E_PANIC
        }
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

    struct BlockingReadContext {
        read_started: AtomicBool,
        release_read: AtomicBool,
    }

    struct BlockingOpenContext {
        open_started: AtomicBool,
        release_open: AtomicBool,
    }

    #[derive(Clone)]
    struct MockMetadata {
        total_frames: u64,
        has_md5: u32,
        md5: [u8; 16],
    }

    unsafe extern "C" fn mock_open(
        context: *mut c_void,
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
        let metadata = if context.is_null() {
            MockMetadata {
                total_frames: 4,
                has_md5: 0,
                md5: [0; 16],
            }
        } else {
            (&*context.cast::<MockMetadata>()).to_owned()
        };
        *info = CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: ABI_VERSION,
            sample_rate: 48_000,
            channel_count: 2,
            valid_bits: 16,
            reserved: 0,
            total_frames: metadata.total_frames,
            has_md5: metadata.has_md5,
            reserved2: 0,
            md5: metadata.md5,
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

    unsafe extern "C" fn blocking_read(
        context: *mut c_void,
        opaque: *mut c_void,
        output: *mut i32,
        frames: u64,
        frames_read: *mut u64,
    ) -> i32 {
        let context = &*context.cast::<BlockingReadContext>();
        context.read_started.store(true, Ordering::Release);
        while !context.release_read.load(Ordering::Acquire) {
            thread::yield_now();
        }
        mock_read(ptr::null_mut(), opaque, output, frames, frames_read)
    }

    unsafe extern "C" fn blocking_read_open(
        _: *mut c_void,
        decoder_kind: u32,
        path: *const u8,
        path_length: u64,
        out_decoder: *mut *mut c_void,
        info: *mut CamelSourceInfoV1,
    ) -> i32 {
        mock_open(
            ptr::null_mut(),
            decoder_kind,
            path,
            path_length,
            out_decoder,
            info,
        )
    }

    unsafe extern "C" fn blocking_load_open(
        context: *mut c_void,
        decoder_kind: u32,
        path: *const u8,
        path_length: u64,
        out_decoder: *mut *mut c_void,
        info: *mut CamelSourceInfoV1,
    ) -> i32 {
        let context = &*context.cast::<BlockingOpenContext>();
        context.open_started.store(true, Ordering::Release);
        while !context.release_open.load(Ordering::Acquire) {
            thread::yield_now();
        }
        mock_open(
            ptr::null_mut(),
            decoder_kind,
            path,
            path_length,
            out_decoder,
            info,
        )
    }

    unsafe extern "C" fn blocking_seek(
        context: *mut c_void,
        opaque: *mut c_void,
        frame: u64,
    ) -> i32 {
        mock_seek(context, opaque, frame)
    }

    unsafe extern "C" fn blocking_close(context: *mut c_void, opaque: *mut c_void) {
        mock_close(context, opaque)
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

    fn mock_host_with_metadata(metadata: &mut MockMetadata) -> CamelDecoderHostV1 {
        CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: (metadata as *mut MockMetadata).cast(),
            open: Some(mock_open),
            read: Some(mock_read),
            seek: Some(mock_seek),
            close: Some(mock_close),
        }
    }

    fn blocking_host(context: &mut BlockingReadContext) -> CamelDecoderHostV1 {
        CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: (context as *mut BlockingReadContext).cast(),
            open: Some(blocking_read_open),
            read: Some(blocking_read),
            seek: Some(blocking_seek),
            close: Some(blocking_close),
        }
    }

    fn blocking_open_host(context: &mut BlockingOpenContext) -> CamelDecoderHostV1 {
        CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: (context as *mut BlockingOpenContext).cast(),
            open: Some(blocking_load_open),
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
    fn spsc_endpoint_gate_rejects_a_second_owner_without_waiting() {
        let gate = TryGate::default();
        let first = gate.enter().unwrap();
        assert!(gate.enter().is_none());
        drop(first);
        assert!(gate.enter().is_some());
    }

    fn load_mock_source(engine: *mut CamelEngine) -> i32 {
        let path = b"mock.flac";
        let command = CamelCommandStampV1 {
            struct_size: std::mem::size_of::<CamelCommandStampV1>() as u32,
            abi_version: ABI_VERSION,
            command_id: 1,
            generation: 1,
            queue_revision: 1,
        };
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let host = mock_host();
        unsafe {
            camel_engine_load_flac_v1(
                engine,
                &command,
                &source,
                decoder_host::DECODER_FFMPEG,
                &host,
            )
        }
    }

    fn command_stamp(command_id: u64) -> CamelCommandStampV1 {
        CamelCommandStampV1 {
            struct_size: std::mem::size_of::<CamelCommandStampV1>() as u32,
            abi_version: ABI_VERSION,
            command_id,
            generation: command_id,
            queue_revision: 1,
        }
    }

    fn render_request(units: u64, output: &mut [u8]) -> CamelRenderRequestV1 {
        CamelRenderRequestV1 {
            struct_size: std::mem::size_of::<CamelRenderRequestV1>() as u32,
            abi_version: ABI_VERSION,
            units,
            output: output.as_mut_ptr(),
            output_capacity_bytes: output.len() as u64,
        }
    }

    fn render_result() -> CamelRenderResultV1 {
        CamelRenderResultV1 {
            struct_size: std::mem::size_of::<CamelRenderResultV1>() as u32,
            abi_version: ABI_VERSION,
            units_rendered: 0,
            required_bytes: 0,
            state: RENDER_STATE_NO_SOURCE,
            reserved: 0,
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
        assert_eq!(std::mem::size_of::<CamelFlacIntegrityV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelEngineTimelineV1>(), 88);
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
    fn render_packs_prefilled_pcm_and_drains_the_final_tail() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);

        let mut frames_prefilled = 0;
        assert_eq!(
            unsafe { camel_engine_prefill_v1(engine, 4, &mut frames_prefilled) },
            ENGINE_OK
        );
        assert_eq!(frames_prefilled, 4);
        assert_eq!(
            unsafe { camel_engine_prefill_v1(engine, 4, &mut frames_prefilled) },
            ENGINE_END
        );
        assert_eq!(frames_prefilled, 0);

        let mut output = [0xA5; 30];
        let request = render_request(5, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_END
        );
        assert_eq!(result.units_rendered, 4);
        assert_eq!(result.required_bytes, 30);
        assert_eq!(result.state, RENDER_STATE_END_OF_STREAM);
        assert_eq!(
            output,
            [
                0, 1, 0, 0, 255, 255, 0, 2, 0, 0, 254, 255, 0, 3, 0, 0, 253, 255, 0, 4, 0, 0, 252,
                255, 0, 0, 0, 0, 0, 0
            ]
        );

        let mut state = CamelEngineStateV1 {
            struct_size: std::mem::size_of::<CamelEngineStateV1>() as u32,
            abi_version: ABI_VERSION,
            playback_state: 0,
            last_error_code: 0,
            command: CamelCommandStampV1 {
                struct_size: std::mem::size_of::<CamelCommandStampV1>() as u32,
                abi_version: ABI_VERSION,
                command_id: 0,
                generation: 0,
                queue_revision: 0,
            },
            position_frames: 0,
            total_frames: 0,
            has_source: 0,
            reserved: 0,
            source_format: empty_format(),
            output_format: empty_format(),
        };
        assert_eq!(
            unsafe { camel_engine_get_state_v1(engine, &mut state) },
            ENGINE_OK
        );
        assert_eq!(state.position_frames, 4);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn playback_state_and_timeline_separate_decoded_submitted_and_audible_positions() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);

        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);
        let mut frames_prefilled = 0;
        assert_eq!(
            unsafe { camel_engine_prefill_v1(engine, 3, &mut frames_prefilled) },
            ENGINE_OK
        );
        assert_eq!(frames_prefilled, 3);

        let mut output = [0; 8];
        let request = render_request(2, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.units_rendered, 2);
        assert_eq!(
            unsafe { camel_engine_report_audible_position_v1(engine, &play, 1) },
            ENGINE_OK
        );

        let mut timeline = CamelEngineTimelineV1 {
            struct_size: std::mem::size_of::<CamelEngineTimelineV1>() as u32,
            abi_version: ABI_VERSION,
            playback_state: PLAYBACK_EMPTY,
            reserved: 0,
            command: command_stamp(0),
            decoded_position_frames: 0,
            submitted_position_frames: 0,
            audible_position_frames: 0,
            total_frames: 0,
            has_source: 0,
            reserved2: 0,
        };
        assert_eq!(
            unsafe { camel_engine_get_timeline_v1(engine, &mut timeline) },
            ENGINE_OK
        );
        assert_eq!(timeline.playback_state, PLAYBACK_PLAYING);
        assert_eq!(timeline.decoded_position_frames, 3);
        assert_eq!(timeline.submitted_position_frames, 2);
        assert_eq!(timeline.audible_position_frames, 1);
        assert_eq!(timeline.total_frames, 4);

        let pause = command_stamp(3);
        assert_eq!(unsafe { camel_engine_pause_v1(engine, &pause) }, ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_report_audible_position_v1(engine, &play, 2) },
            ENGINE_E_NOT_READY
        );
        assert_eq!(
            unsafe { camel_engine_get_timeline_v1(engine, &mut timeline) },
            ENGINE_OK
        );
        assert_eq!(timeline.playback_state, PLAYBACK_PAUSED);
        assert_eq!(timeline.audible_position_frames, 1);

        let stop = command_stamp(4);
        assert_eq!(unsafe { camel_engine_stop_v1(engine, &stop) }, ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_get_timeline_v1(engine, &mut timeline) },
            ENGINE_OK
        );
        assert_eq!(timeline.playback_state, PLAYBACK_STOPPED);
        assert_eq!(timeline.decoded_position_frames, 0);
        assert_eq!(timeline.submitted_position_frames, 0);
        assert_eq!(timeline.audible_position_frames, 0);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn stop_cancels_a_prefill_in_progress_and_discards_its_decoded_frames() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );

        let path = b"blocking.flac";
        let load = command_stamp(1);
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let mut context = BlockingReadContext {
            read_started: AtomicBool::new(false),
            release_read: AtomicBool::new(false),
        };
        let host = blocking_host(&mut context);
        assert_eq!(
            unsafe {
                camel_engine_load_flac_v1(
                    engine,
                    &load,
                    &source,
                    decoder_host::DECODER_FFMPEG,
                    &host,
                )
            },
            ENGINE_OK
        );

        let engine_address = engine as usize;
        let prefill = thread::spawn(move || {
            let mut frames_prefilled = 0;
            let status = unsafe {
                camel_engine_prefill_v1(
                    engine_address as *mut CamelEngine,
                    4,
                    &mut frames_prefilled,
                )
            };
            (status, frames_prefilled)
        });
        while !context.read_started.load(Ordering::Acquire) {
            thread::yield_now();
        }

        let engine_address = engine as usize;
        let stop = command_stamp(2);
        let stop_thread = thread::spawn(move || unsafe {
            camel_engine_stop_v1(engine_address as *mut CamelEngine, &stop)
        });
        while unsafe { &*(engine as *const CamelEngine) }
            .operation_epoch
            .load(Ordering::Acquire)
            < 2
        {
            thread::yield_now();
        }
        context.release_read.store(true, Ordering::Release);

        assert_eq!(prefill.join().unwrap(), (ENGINE_E_NOT_READY, 0));
        assert_eq!(stop_thread.join().unwrap(), ENGINE_OK);
        assert_eq!(unsafe { &*engine }.ring.readable(), 0);
        assert_eq!(
            unsafe { &*engine }
                .source
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .position(),
            0
        );
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn newer_stop_discards_a_source_that_finishes_loading_late() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let mut context = BlockingOpenContext {
            open_started: AtomicBool::new(false),
            release_open: AtomicBool::new(false),
        };
        let context_address = (&mut context as *mut BlockingOpenContext) as usize;
        let engine_address = engine as usize;
        let load = thread::spawn(move || {
            let path = b"slow.flac";
            let command = command_stamp(1);
            let source = CamelTrackSourceV1 {
                struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
                abi_version: ABI_VERSION,
                path_utf8: path.as_ptr(),
                path_length_bytes: path.len() as u64,
                start_frame: 0,
                end_frame_exclusive: 0,
                has_end_frame: 0,
                reserved: 0,
            };
            let host =
                unsafe { blocking_open_host(&mut *(context_address as *mut BlockingOpenContext)) };
            unsafe {
                camel_engine_load_flac_v1(
                    engine_address as *mut CamelEngine,
                    &command,
                    &source,
                    decoder_host::DECODER_FFMPEG,
                    &host,
                )
            }
        });
        while !context.open_started.load(Ordering::Acquire) {
            thread::yield_now();
        }

        let stop = command_stamp(2);
        assert_eq!(unsafe { camel_engine_stop_v1(engine, &stop) }, ENGINE_OK);
        context.release_open.store(true, Ordering::Release);
        assert_eq!(load.join().unwrap(), ENGINE_E_NOT_READY);

        let mut timeline = CamelEngineTimelineV1 {
            struct_size: std::mem::size_of::<CamelEngineTimelineV1>() as u32,
            abi_version: ABI_VERSION,
            playback_state: PLAYBACK_EMPTY,
            reserved: 0,
            command: command_stamp(0),
            decoded_position_frames: 0,
            submitted_position_frames: 0,
            audible_position_frames: 0,
            total_frames: 0,
            has_source: 0,
            reserved2: 0,
        };
        assert_eq!(
            unsafe { camel_engine_get_timeline_v1(engine, &mut timeline) },
            ENGINE_OK
        );
        assert_eq!(timeline.command.command_id, 2);
        assert_eq!(timeline.playback_state, PLAYBACK_STOPPED);
        assert_eq!(timeline.has_source, 0);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn seek_cancels_prefill_and_discards_frames_from_the_old_position() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let path = b"blocking.flac";
        let load = command_stamp(1);
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let mut context = BlockingReadContext {
            read_started: AtomicBool::new(false),
            release_read: AtomicBool::new(false),
        };
        let host = blocking_host(&mut context);
        assert_eq!(
            unsafe {
                camel_engine_load_flac_v1(
                    engine,
                    &load,
                    &source,
                    decoder_host::DECODER_FFMPEG,
                    &host,
                )
            },
            ENGINE_OK
        );

        let engine_address = engine as usize;
        let prefill = thread::spawn(move || {
            let mut frames_prefilled = 0;
            let status = unsafe {
                camel_engine_prefill_v1(
                    engine_address as *mut CamelEngine,
                    4,
                    &mut frames_prefilled,
                )
            };
            (status, frames_prefilled)
        });
        while !context.read_started.load(Ordering::Acquire) {
            thread::yield_now();
        }

        let engine_address = engine as usize;
        let seek = command_stamp(2);
        let seek_thread = thread::spawn(move || unsafe {
            camel_engine_seek_v1(engine_address as *mut CamelEngine, &seek, 2)
        });
        while unsafe { &*(engine as *const CamelEngine) }
            .operation_epoch
            .load(Ordering::Acquire)
            < 2
        {
            thread::yield_now();
        }
        context.release_read.store(true, Ordering::Release);

        assert_eq!(prefill.join().unwrap(), (ENGINE_E_NOT_READY, 0));
        assert_eq!(seek_thread.join().unwrap(), ENGINE_OK);
        assert_eq!(unsafe { &*engine }.ring.readable(), 0);
        assert_eq!(
            unsafe { &*engine }
                .source
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .position(),
            2
        );
        assert_eq!(
            unsafe { &*engine }
                .submitted_position
                .load(Ordering::Acquire),
            2
        );
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn render_returns_silent_underrun_without_waiting_for_source_lock() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);

        let mut output = [0xA5; 8];
        let request = render_request(2, &mut output);
        let mut result = render_result();
        let source = unsafe { &*engine }.source.lock().unwrap();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_UNDERRUN
        );
        assert_eq!(result.units_rendered, 0);
        assert_eq!(result.state, RENDER_STATE_UNDERRUN);
        assert_eq!(output, [0; 8]);
        drop(source);

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
    fn unknown_total_is_verified_only_after_complete_eof() {
        let path = b"mock.flac";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let mut metadata = MockMetadata {
            total_frames: 0,
            has_md5: 1,
            md5: [0xA5; 16],
        };
        let host_abi = mock_host_with_metadata(&mut metadata);
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_FFMPEG, host) }.unwrap();
        let mut output = [0; 8];
        assert_eq!(decoded.integrity_status(), FLAC_INTEGRITY_INCOMPLETE);
        assert_eq!(decoded.read(&mut output, 4), Ok(4));
        assert_eq!(decoded.integrity_status(), FLAC_INTEGRITY_INCOMPLETE);
        assert_eq!(decoded.read(&mut output, 1), Ok(0));
        assert_eq!(decoded.integrity_status(), FLAC_INTEGRITY_VERIFIED);
    }

    #[test]
    fn known_total_mismatch_marks_integrity_failed() {
        let path = b"mock.flac";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
        };
        let mut metadata = MockMetadata {
            total_frames: 5,
            has_md5: 1,
            md5: [0xA5; 16],
        };
        let host_abi = mock_host_with_metadata(&mut metadata);
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_FFMPEG, host) }.unwrap();
        let mut output = [0; 8];
        assert_eq!(decoded.read(&mut output, 4), Ok(4));
        assert_eq!(decoded.read(&mut output, 1), Err(ENGINE_E_DECODE));
        assert_eq!(decoded.integrity_status(), FLAC_INTEGRITY_FAILED);
        assert_eq!(decoded.seek(0), Ok(()));
        assert_eq!(decoded.integrity_status(), FLAC_INTEGRITY_FAILED);
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
