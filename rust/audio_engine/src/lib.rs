use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;

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
    max_render_units: u64,
    render_gate: RenderGate,
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
                    max_render_units,
                    render_gate: RenderGate::default(),
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
    use std::sync::mpsc;
    use std::time::Duration;

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
}
