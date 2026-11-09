// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

mod convert;
mod convolver;
mod crossfeed;
mod decoder_host;
mod dsd;
mod dsd2pcm;
mod dsd_pcm_abi;
mod dsd_remod;
mod dsp;
mod dts_abi;
mod eq;
mod fft;
mod packing;
mod resample;
mod ring;

use convert::Converter;
// Links the Symphonia decoders into this static library, which exports them with the Engine.
use symphonia_ffi as _;

// Counts the heap operations of the DSP stage and the render callback in tests: both must have none (docs/Plan.md
// §5.2).
#[cfg(test)]
mod stage_heap {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;

    thread_local! {
        static INSIDE: Cell<u32> = const { Cell::new(0) };
        static COUNT: Cell<usize> = const { Cell::new(0) };
    }

    pub(crate) struct Counting;

    fn note() {
        let _ = INSIDE.try_with(|inside| {
            if inside.get() > 0 {
                let _ = COUNT.try_with(|count| count.set(count.get() + 1));
            }
        });
    }

    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            note();
            unsafe { System.alloc(layout) }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            note();
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            note();
            unsafe { System.dealloc(ptr, layout) }
        }
        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            note();
            unsafe { System.realloc(ptr, layout, size) }
        }
    }

    // The DSP stage or the render callback runs on this thread while one lives.
    pub(crate) struct Inside;

    impl Inside {
        pub(crate) fn enter() -> Self {
            INSIDE.with(|inside| inside.set(inside.get() + 1));
            Inside
        }
    }

    impl Drop for Inside {
        fn drop(&mut self) {
            INSIDE.with(|inside| inside.set(inside.get() - 1));
        }
    }

    // The heap operations counted on this thread since the last call.
    pub(crate) fn take() -> usize {
        COUNT.with(|count| count.replace(0))
    }
}

// Under Miri, System runs std's own Windows code for over-aligned blocks, which Stacked Borrows rejects.
#[cfg(all(test, not(miri)))]
#[global_allocator]
static HEAP: stage_heap::Counting = stage_heap::Counting;
pub use decoder_host::{
    CamelDecoderHostV1, CamelSourceInfoV1, SAMPLE_ENCODING_DSD, SAMPLE_ENCODING_FLOAT32,
    SAMPLE_ENCODING_INTEGER,
};
use decoder_host::{DecoderHost, DecoderSource};
use dsd::{fill_silence, stamp_markers, units_per_frame};
use packing::bytes_per_frame;
pub use resample::{QUALITY_HIGH, QUALITY_QUICK, QUALITY_VERY_HIGH};
use ring::SpscByteRing;

pub const ABI_VERSION: u32 = 1;
pub const MAX_RENDER_UNITS: u64 = 65_536;
const MAX_PREFILL_FRAMES: u64 = 4_096;
// The ring holds this much predecoded audio unless the configuration sets another amount, within a fixed memory
// limit.
const DEFAULT_PREBUFFER_MS: u64 = 10_000;
const MAX_RING_BYTES: u64 = 64 << 20;
// A configuration asking for more than the default buffers ahead of a source read slower than it plays.
const MAX_BUFFERING_RING_BYTES: u64 = 1 << 30;
// How far device bytes are prepared ahead of the output, which is how soon a DSP change is heard. The device ring
// also holds at least MIN_DEVICE_RING_FRAMES, the largest period an output requests at once.
const DSP_LEAD_MS: u64 = 250;
const MIN_DEVICE_RING_FRAMES: u64 = 8_192;
// The widest source a frame of the decoded ring must hold: eight channels of 32-bit sample words.
const MAX_SOURCE_FRAME_BYTES: u64 = 8 * 4;
// The longest crossfade a track may ask for (docs/Plan.md §5.4).
const MAX_CROSSFADE_MS: u32 = 10_000;
// A manual change keeps this much more of the playing track than its fade takes, so it plays on while the chosen one
// starts decoding.
const CUT_MARGIN_MS: u32 = 1_000;

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
pub const SAMPLE_DOP_S24_IN_32_LE: u32 = 8;

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
pub const RENDER_STATE_PAUSED: u32 = 5;

pub const PLAYBACK_EMPTY: u32 = 0;
pub const PLAYBACK_READY: u32 = 1;
pub const PLAYBACK_PLAYING: u32 = 2;
pub const PLAYBACK_PAUSED: u32 = 3;
pub const PLAYBACK_STOPPED: u32 = 4;
pub const PLAYBACK_FAILED: u32 = 5;

/// `CamelTrackSourceV1::flags`: the track may be converted to the open device format, DSD to PCM included (crossfade
/// permits it).
pub const TRACK_CONVERT: u32 = 1;
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
    /// The track's ReplayGain in dB, applied before the DSP chain; 0 leaves the track bit-perfect.
    pub gain_db: f64,
    /// The crossfade into this track in milliseconds, up to 10,000; 0 follows the previous track gaplessly.
    pub crossfade_ms: u32,
    /// TRACK_CONVERT or 0.
    pub flags: u32,
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
    /// Nonzero permits resampling, downmix and bit reduction ("convert when unsupported").
    pub allow_conversion: u32,
    /// QUALITY_QUICK, QUALITY_HIGH or QUALITY_VERY_HIGH.
    pub resample_quality: u32,
    /// Nonzero permits DSD to PCM; without it a DSD source plays only as native DSD or DoP.
    pub allow_dsd_to_pcm: u32,
    /// Predecoded audio the ring holds, in milliseconds; 0 means 10 s. Capped at 64 MiB.
    pub prebuffer_ms: u32,
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

/// One parametric EQ band (docs/Plan.md §5.9). `kind`: 1 peaking, 2 low shelf, 3 high shelf, 4 low-pass,
/// 5 high-pass, 6 band-pass, 7 notch, 8 all-pass. `channels`: 0 all, 1 left, 2 right. `slope`: low- and high-pass
/// dB per octave, 6 to 48 in steps of 6 (12 uses `q`). A band with `enabled` 0 is skipped.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelEqBandV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub kind: u32,
    pub channels: u32,
    pub slope: u32,
    pub enabled: u32,
    pub frequency_hz: f64,
    pub gain_db: f64,
    pub q: f64,
}

/// Headphone crossfeed (bs2b): the cut-off of the feed to the other ear, 300 to 2000 Hz, and how much lower it is
/// at low frequencies, 1 to 15 dB.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelCrossfeedV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub cutoff_hz: u32,
    pub reserved: u32,
    pub feed_db: f64,
}

/// An impulse response at one rate: `frames` interleaved frames of `channels` 64-bit float samples, 1 channel for
/// both outputs, 2 for left and right, or 4 for true stereo (left to left, left to right, right to left, right to
/// right).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelImpulseResponseV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub channels: u32,
    pub rate_hz: u32,
    pub frames: u64,
    pub samples: *const f64,
}

/// A prepared impulse response, shared by every engine and analysis that uses it.
pub struct CamelImpulse(Arc<convolver::ImpulseResponse>);

pub const DSP_NODE_EQ: u32 = dsp::NODE_EQ;
pub const DSP_NODE_CROSSFEED: u32 = dsp::NODE_CROSSFEED;
pub const DSP_NODE_CONVOLUTION: u32 = dsp::NODE_CONVOLUTION;

/// The DSP chain (docs/Plan.md §5.9): `order` names each node once; `band_count` EQ bands at `bands`; crossfeed and
/// convolution are off while their pointers are null.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelDspV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub order: [u32; 3],
    pub band_count: u32,
    pub bands: *const CamelEqBandV1,
    pub crossfeed: *const CamelCrossfeedV1,
    pub impulse: *const CamelImpulse,
    /// The software volume in dB, 0 or less; negative infinity mutes.
    pub volume_db: f64,
}

/// What a DSP setting does at a rate: `active` 1 when it changes the signal, and the chain's preamp (0 or less).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CamelDspSummaryV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub active: u32,
    pub reserved: u32,
    pub preamp_db: f64,
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
            (AUDIO_DOP, SAMPLE_DOP_S24_IN_32_LE) => {
                (self.valid_bits, self.container_bits) == (24, 32)
            }
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
                .and_then(|value| value.checked_mul(u64::from(self.container_bits / 8))),
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
    allow_conversion: bool,
    allow_dsd_to_pcm: bool,
    resample_quality: u32,
    max_render_units: u64,
    // Decoded source samples, up to the prebuffer (docs/Plan.md §5.2). Prefill writes them; the DSP stage converts
    // them into `ring`, which holds only about DSP_LEAD_MS of device bytes so DSP changes are heard quickly.
    source_ring: SpscByteRing,
    ring: SpscByteRing,
    stage: Mutex<Stage>,
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
    source_view: Mutex<Option<SourceView>>,
    // The view of the source decoding left at the pending boundary: integrity queries report the track still heard.
    finished_view: Mutex<Option<SourceView>>,
    // Gapless: prefill swaps `next_source` in at the current source's end without clearing the ring. Frames are
    // counted since the last ring reset; `boundary_at` is the frame where the next track starts, or NO_BOUNDARY.
    next_source: Mutex<Option<DecoderSource>>,
    written_frames: AtomicU64,
    read_frames: AtomicU64,
    boundary_at: AtomicU64,
    boundary_start_frame: AtomicU64,
    // Decoding moved on to the queued track and render has not reached it yet.
    pending_boundary: AtomicBool,
    transitions: AtomicU64,
    next_occupied: AtomicBool,
    // DoP: the marker the next rendered frame carries. Only render touches it.
    dop_phase: AtomicUsize,
}

const NO_BOUNDARY: u64 = u64::MAX;

// The source frames of one track in `source_ring` and the converter that takes them to the device format.
struct Segment {
    converter: Converter,
    channels: usize,
    // 1 for raw DSD bytes, 4 for the i32 sample words of every other source.
    sample_bytes: usize,
    frames: u64,
    read: u64,
    // Decoding reached the track's end.
    ended: bool,
    // A track following gaplessly: the output frame its position restarts at, until the stage reaches it.
    boundary_start: Option<u64>,
    rate: u32,
    // Source frames the track was expected to hold when it started, 0 when its length is unknown.
    expected: u64,
    // Output frames of the crossfade into this track; 0 follows the previous one gaplessly.
    fade_in: u64,
    // A manual change cut the track short: it fades out as soon as the next one has audio.
    cut: bool,
    // Room for the outgoing track's tail when this track fades in, reserved off the DSP stage.
    held: Vec<u8>,
}

impl Segment {
    fn new(
        converter: Converter,
        source: &DecoderSource,
        boundary_start: Option<u64>,
        fade_in: u64,
    ) -> Self {
        Segment {
            converter,
            channels: source.info.channel_count as usize,
            sample_bytes: Self::sample_bytes(&source.info),
            frames: 0,
            read: 0,
            ended: false,
            boundary_start,
            rate: source.info.sample_rate,
            expected: source.remaining_frames(),
            fade_in,
            cut: false,
            held: Vec::new(),
        }
    }

    // The track's length in source frames as far as it is known: decoded so far, or more if it is expected to be.
    fn length(&self) -> u64 {
        if self.ended {
            self.frames
        } else {
            self.expected.max(self.frames)
        }
    }

    fn sample_bytes(info: &CamelSourceInfoV1) -> usize {
        if info.sample_encoding == SAMPLE_ENCODING_DSD {
            1
        } else {
            4
        }
    }

    fn frame_bytes(&self) -> usize {
        self.channels * self.sample_bytes
    }
}

struct Stage {
    segments: std::collections::VecDeque<Segment>,
    // Decoding reached the end of the last track with nothing queued.
    stream_ended: bool,
    // Global for the device: its nodes keep their state across tracks, so gapless playback stays seamless.
    dsp: dsp::Dsp,
    // Frames of the chain's tail still to play after the last track, once it is reached.
    tail_left: Option<usize>,
    // The crossfade from the first segment into the second, while it plays.
    fade: Option<Fade>,
    // The DSP stage's working buffers, sized when the engine is made so it needs no heap (docs/Plan.md §5.2).
    buffers: StageBuffers,
    // Tracks the stage has finished with, freed by the decode worker instead.
    retired: Vec<Segment>,
}

const RETIRED_SEGMENTS: usize = 8;

struct StageBuffers {
    bytes: Vec<u8>,
    words: Vec<i32>,
    // What a crossfade mixes, lent to each fade and given back when it ends.
    outgoing: Vec<f64>,
    incoming: Vec<f64>,
    mixed: Vec<f64>,
}

impl StageBuffers {
    fn new(channels: usize) -> Self {
        let block = MAX_PREFILL_FRAMES as usize;
        // A fade gathers a block from either track, plus what a resampler adds and flushes.
        let fade = (3 * block + 8192) * channels;
        StageBuffers {
            bytes: Vec::with_capacity(block * MAX_SOURCE_FRAME_BYTES as usize),
            words: Vec::with_capacity(block * MAX_SOURCE_FRAME_BYTES as usize / 4),
            outgoing: Vec::with_capacity(fade),
            incoming: Vec::with_capacity(fade),
            mixed: Vec::with_capacity(fade),
        }
    }

    fn lend(&mut self) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
        let take = |buffer: &mut Vec<f64>| {
            let mut lent = std::mem::take(buffer);
            lent.clear();
            lent
        };
        (
            take(&mut self.outgoing),
            take(&mut self.incoming),
            take(&mut self.mixed),
        )
    }

    // Takes back what a fade borrowed; its held tail goes to `keeper`, which the decode worker frees in time.
    fn give_back(&mut self, fade: Fade, keeper: &mut Segment) {
        self.outgoing = fade.outgoing;
        self.incoming = fade.incoming;
        self.mixed = fade.mixed;
        keeper.held = fade.held;
    }
}

// A finished segment leaves the DSP stage without being freed there.
fn retire(retired: &mut Vec<Segment>, segment: Option<Segment>) {
    if let Some(segment) = segment {
        retired.push(segment);
    }
}

// A crossfade (docs/Plan.md §5.4). When it begins, the outgoing track's last source frames leave the source ring for
// `held`, so the incoming track's frames behind them can be read; both are converted to float at the device rate, mixed
// with equal-power gains, and go through the DSP chain and the final quantization once, in the incoming track's
// converter. A change during a crossfade renders what that one would still play into `outgoing`, which then fades out as
// one track.
struct Fade {
    // Output frames.
    length: u64,
    done: u64,
    held: Vec<u8>,
    held_read: usize,
    outgoing: Vec<f64>,
    outgoing_done: bool,
    incoming: Vec<f64>,
    mixed: Vec<f64>,
    // The fade has not begun: `outgoing` plays on at its level until the incoming track has audio.
    waiting: bool,
}

impl Fade {
    // `frames` mixed frames of what both sides have into `mixed`, moving the fade on; past its end the incoming side plays
    // at its own level. Frames a side no longer has are silent.
    fn mix(&mut self, frames: usize, channels: usize) {
        self.mixed.clear();
        self.mixed.resize(frames * channels, 0.0);
        for (i, frame) in self.mixed.chunks_exact_mut(channels).enumerate() {
            let progress = (self.done as f64 + i as f64 + 0.5) / self.length as f64;
            let (out_gain, in_gain) = if progress >= 1.0 {
                (0.0, 1.0)
            } else {
                let angle = progress * std::f64::consts::FRAC_PI_2;
                (angle.cos(), angle.sin())
            };
            for (c, sample) in frame.iter_mut().enumerate() {
                let k = i * channels + c;
                *sample = self.outgoing.get(k).map_or(0.0, |v| v * out_gain)
                    + self.incoming.get(k).map_or(0.0, |v| v * in_gain);
            }
        }
        let used = frames * channels;
        self.outgoing.drain(..used.min(self.outgoing.len()));
        self.incoming.drain(..used.min(self.incoming.len()));
        self.done += frames as u64;
    }
}

enum FadeStep {
    Wrote(u64),
    Mixed,
    Wait,
    Done,
}

// Interleaved sample words from decoded ring bytes into `words`: one per byte for raw DSD, little-endian i32 otherwise.
fn sample_words(bytes: &[u8], sample_bytes: usize, words: &mut Vec<i32>) {
    words.clear();
    if sample_bytes == 1 {
        words.extend(bytes.iter().map(|&b| i32::from(b)));
    } else {
        words.extend(
            bytes
                .as_chunks::<4>()
                .0
                .iter()
                .map(|b| i32::from_le_bytes(*b)),
        );
    }
}

// What queries report about the source. Decoder I/O holds `source` for its whole duration, so queries read this
// copy, which every source change refreshes while still holding `source`.
#[derive(Clone, Copy)]
struct SourceView {
    info: CamelSourceInfoV1,
    decoded_position: u64,
    integrity_status: u32,
    has_reference_md5: bool,
}

impl SourceView {
    fn of(source: &DecoderSource) -> Self {
        SourceView {
            info: source.info,
            decoded_position: source.position(),
            integrity_status: source.integrity_status(),
            has_reference_md5: source.has_reference_md5(),
        }
    }
}

impl CamelEngine {
    fn fail_render(&self, status: i32) {
        self.render_error.store(status, Ordering::Release);
        self.playback_state
            .store(PLAYBACK_FAILED as usize, Ordering::Release);
        self.render_source_state
            .store(RENDER_STATE_FAULT as usize, Ordering::Release);
    }

    fn publish_source(&self, source: Option<&DecoderSource>) {
        *self.source_view.lock().unwrap() = source.map(SourceView::of);
    }

    fn view(&self) -> Option<SourceView> {
        *self.source_view.lock().unwrap()
    }

    // Caller holds `source`; render must be drained. The source's converter starts a new segment at the next prefill.
    fn reset_stream(&self) {
        let mut stage = self.stage.lock().unwrap();
        stage.segments.clear();
        stage.stream_ended = false;
        stage.dsp.reset();
        stage.tail_left = None;
        let Stage {
            buffers,
            fade,
            retired,
            ..
        } = &mut *stage;
        if let Some(fade) = fade.take() {
            buffers.outgoing = fade.outgoing;
            buffers.incoming = fade.incoming;
            buffers.mixed = fade.mixed;
        }
        retired.clear();
        self.source_ring.clear();
        self.ring.clear();
        self.written_frames.store(0, Ordering::Release);
        self.read_frames.store(0, Ordering::Release);
        self.boundary_at.store(NO_BOUNDARY, Ordering::Release);
        self.pending_boundary.store(false, Ordering::Release);
    }

    fn boundary_pending(&self) -> bool {
        self.pending_boundary.load(Ordering::Acquire)
    }

    // The source being heard: the one decoding has moved past while a boundary is still to be rendered.
    fn heard_view(&self) -> Option<SourceView> {
        let finished = if self.boundary_pending() {
            *self.finished_view.lock().unwrap()
        } else {
            None
        };
        finished.or_else(|| self.view())
    }

    // The DSP stage: converts decoded frames into device bytes until `ring` is full or no decoded frames wait.
    // Returns the device frames written.
    fn pump(&self) -> Result<u64, i32> {
        #[cfg(test)]
        let _inside = stage_heap::Inside::enter();
        let bytes_per_frame = bytes_per_frame(&self.format).ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?;
        let mut stage = self.stage.lock().unwrap();
        let Stage {
            segments,
            stream_ended,
            dsp,
            tail_left,
            fade,
            buffers,
            retired,
        } = &mut *stage;
        let mut written = 0;
        let status = loop {
            if let Some(active) = fade.as_mut() {
                match self.crossfade(segments, active, dsp, buffers, bytes_per_frame) {
                    Ok(FadeStep::Wrote(frames)) => written += frames,
                    Ok(FadeStep::Mixed) => {}
                    Ok(FadeStep::Wait) => break Ok(()),
                    Ok(FadeStep::Done) => {
                        let ended = fade.take().expect("a fade plays");
                        let outgoing = segments.pop_front();
                        buffers.give_back(ended, &mut segments[0]);
                        retire(retired, outgoing);
                    }
                    Err(status) => break Err(status),
                }
                continue;
            }
            let fade_frames = self.fade_after_first(segments);
            let next_ready = segments.get(1).map(|next| next.frames > next.read);
            let last = segments.len() == 1;
            let Some(segment) = segments.front_mut() else {
                break Ok(());
            };
            let space = (self.ring.writable() / bytes_per_frame) as u64;
            let pending = segment.converter.pending_frames();
            if pending > 0 {
                let frames = pending.min(space);
                if frames == 0 {
                    break Ok(());
                }
                let bytes = frames as usize * bytes_per_frame;
                if self.ring.write(&segment.converter.pending()[..bytes]) != bytes {
                    break Err(ENGINE_E_INVALID_STATE);
                }
                segment.converter.consume(frames);
                self.written_frames.fetch_add(frames, Ordering::AcqRel);
                written += frames;
                continue;
            }
            if let Some(start) = segment.boundary_start {
                // One boundary at a time: render reaches the previous one first.
                if self.boundary_at.load(Ordering::Acquire) != NO_BOUNDARY {
                    break Ok(());
                }
                segment.boundary_start = None;
                self.boundary_start_frame.store(start, Ordering::Release);
                self.boundary_at.store(
                    self.written_frames.load(Ordering::Acquire),
                    Ordering::Release,
                );
            }
            let available = segment.frames - segment.read;
            // Before a crossfade the track's last frames wait in the source ring until the fade takes them.
            let hold = if fade_frames > 0 {
                segment
                    .converter
                    .input_frames_for(fade_frames)
                    .min(self.max_hold(segment))
            } else {
                0
            };
            let convertible = available.min(
                segment
                    .length()
                    .saturating_sub(hold)
                    .saturating_sub(segment.read),
            );
            if hold > 0
                && segment.ended
                && (convertible == 0 || segment.cut && next_ready == Some(true))
            {
                // The next track starts at the fade, which waits for it to be staged and for the previous boundary.
                if next_ready.is_none() || self.boundary_at.load(Ordering::Acquire) != NO_BOUNDARY {
                    break Ok(());
                }
                let length = fade_frames.min(self.output_frames(available, segment.rate));
                if length == 0 {
                    segments[1].fade_in = 0;
                    continue;
                }
                let held_bytes = available as usize * segment.frame_bytes();
                segment.read = segment.frames;
                let mut held = std::mem::take(&mut segments[1].held);
                held.clear();
                held.resize(held_bytes, 0);
                if self.source_ring.read(&mut held) != held.len() {
                    break Err(ENGINE_E_INVALID_STATE);
                }
                if let Some(start) = segments[1].boundary_start.take() {
                    self.boundary_start_frame.store(start, Ordering::Release);
                    self.boundary_at.store(
                        self.written_frames.load(Ordering::Acquire),
                        Ordering::Release,
                    );
                }
                let (outgoing, incoming, mixed) = buffers.lend();
                *fade = Some(Fade {
                    length,
                    done: 0,
                    held,
                    held_read: 0,
                    outgoing,
                    outgoing_done: false,
                    incoming,
                    mixed,
                    waiting: false,
                });
                continue;
            }
            if convertible > 0 {
                if space == 0 {
                    break Ok(());
                }
                let frames = segment
                    .converter
                    .input_frames_for(space)
                    .min(convertible)
                    .min(MAX_PREFILL_FRAMES);
                buffers.bytes.clear();
                buffers
                    .bytes
                    .resize(frames as usize * segment.frame_bytes(), 0);
                if self.source_ring.read(&mut buffers.bytes) != buffers.bytes.len() {
                    break Err(ENGINE_E_INVALID_STATE);
                }
                sample_words(&buffers.bytes, segment.sample_bytes, &mut buffers.words);
                if let Err(status) = segment
                    .converter
                    .convert(&buffers.words, frames as usize, dsp)
                {
                    break Err(status);
                }
                segment.read += frames;
                continue;
            }
            if available > 0 {
                // Held for the crossfade until decoding reaches the track's end.
                break Ok(());
            }
            if segment.ended {
                if !segment.converter.finished() {
                    // The resampler's tail belongs to this track and plays before any queued one.
                    if let Err(status) = segment.converter.finish(dsp) {
                        break Err(status);
                    }
                    continue;
                }
                if *stream_ended && last {
                    // After the last track the chain's tail plays out, such as a convolution's reverberation.
                    let left =
                        *tail_left.get_or_insert(if dsp.active() { dsp.tail_frames() } else { 0 });
                    if left > 0 {
                        if space == 0 {
                            break Ok(());
                        }
                        let frames = left.min(space as usize).min(MAX_PREFILL_FRAMES as usize);
                        if let Err(status) = segment.converter.drain(frames, dsp) {
                            break Err(status);
                        }
                        *tail_left = Some(left - frames);
                        continue;
                    }
                }
                retire(retired, segments.pop_front());
                continue;
            }
            break Ok(());
        };
        if let Err(status) = status {
            self.fail_render(status);
            return Err(status);
        }
        if segments.is_empty() && *stream_ended {
            let _ = self.render_source_state.compare_exchange(
                RENDER_STATE_ACTIVE as usize,
                RENDER_STATE_END_OF_STREAM as usize,
                Ordering::AcqRel,
                Ordering::Acquire,
            );
        }
        Ok(written)
    }

    // Output frames of the crossfade from the first segment into the track after it: what that track asks for, at most
    // half of either track; 0 when it follows gaplessly, either is raw DSD or nothing follows yet.
    fn fade_after_first(&self, segments: &std::collections::VecDeque<Segment>) -> u64 {
        let Some(first) = segments.front() else {
            return 0;
        };
        if !first.converter.mixes() {
            return 0;
        }
        let (requested, next_length) = if let Some(next) = segments.get(1) {
            if !next.converter.mixes() {
                return 0;
            }
            (next.fade_in, self.output_frames(next.expected, next.rate))
        } else {
            let slot = self.next_source.lock().unwrap();
            let Some(next) = slot.as_ref() else {
                return 0;
            };
            if !next.converter.as_ref().is_some_and(Converter::mixes) {
                return 0;
            }
            (
                self.fade_frames(next.crossfade_ms),
                self.output_frames(next.remaining_frames(), next.info.sample_rate),
            )
        };
        let frames = requested.min(self.output_frames(first.length(), first.rate) / 2);
        if next_length > 0 {
            frames.min(next_length / 2)
        } else {
            frames
        }
    }

    fn fade_frames(&self, milliseconds: u32) -> u64 {
        u64::from(self.output_format.rate_hz) * u64::from(milliseconds) / 1000
    }

    // The most source frames of `segment` a crossfade may hold back: decoding must keep room for a prefill.
    fn max_hold(&self, segment: &Segment) -> u64 {
        ((self.source_ring.capacity() / segment.frame_bytes()) as u64)
            .saturating_sub(MAX_PREFILL_FRAMES)
    }

    // One step of the crossfade from the first segment into the second: writes either track's pending output to the ring,
    // or mixes what both tracks have into the incoming one's.
    fn crossfade(
        &self,
        segments: &mut std::collections::VecDeque<Segment>,
        fade: &mut Fade,
        dsp: &mut dsp::Dsp,
        buffers: &mut StageBuffers,
        bytes_per_frame: usize,
    ) -> Result<FadeStep, i32> {
        let [outgoing, incoming, ..] = segments.make_contiguous() else {
            return Err(ENGINE_E_INVALID_STATE);
        };
        let space = (self.ring.writable() / bytes_per_frame) as u64;
        // The outgoing track has output of its own left only after a change during a crossfade; it plays first.
        for converter in [&mut outgoing.converter, &mut incoming.converter] {
            let pending = converter.pending_frames();
            if pending > 0 {
                let frames = pending.min(space);
                if frames == 0 {
                    return Ok(FadeStep::Wait);
                }
                let bytes = frames as usize * bytes_per_frame;
                if self.ring.write(&converter.pending()[..bytes]) != bytes {
                    return Err(ENGINE_E_INVALID_STATE);
                }
                converter.consume(frames);
                self.written_frames.fetch_add(frames, Ordering::AcqRel);
                return Ok(FadeStep::Wrote(frames));
            }
        }
        if fade.done >= fade.length {
            return Ok(FadeStep::Done);
        }
        let channels = self.format.channel_count as usize;
        if fade.waiting {
            let spare = ((fade.outgoing.len() / channels) as u64).saturating_sub(fade.length);
            if incoming.frames > incoming.read || incoming.ended || spare == 0 {
                // The incoming track starts at the fade, after the previous boundary.
                if self.boundary_at.load(Ordering::Acquire) != NO_BOUNDARY {
                    return Ok(FadeStep::Wait);
                }
                if let Some(start) = incoming.boundary_start.take() {
                    self.boundary_start_frame.store(start, Ordering::Release);
                    self.boundary_at.store(
                        self.written_frames.load(Ordering::Acquire),
                        Ordering::Release,
                    );
                }
                fade.waiting = false;
            } else {
                let frames = spare.min(space).min(MAX_PREFILL_FRAMES) as usize;
                if frames == 0 {
                    return Ok(FadeStep::Wait);
                }
                fade.mixed.clear();
                fade.mixed.extend(fade.outgoing.drain(..frames * channels));
                incoming.converter.emit_mixed(&mut fade.mixed, dsp)?;
                return Ok(FadeStep::Mixed);
            }
        }
        let wanted = space.min(MAX_PREFILL_FRAMES).min(fade.length - fade.done) as usize;
        if wanted == 0 {
            return Ok(FadeStep::Wait);
        }
        self.fill_outgoing(outgoing, fade, wanted, buffers)?;
        self.fill_incoming(incoming, fade, wanted, buffers)?;
        // A track with nothing more to give is silent for the rest of the fade.
        let incoming_over =
            incoming.ended && incoming.read == incoming.frames && incoming.converter.finished();
        if fade.outgoing_done
            && fade.outgoing.is_empty()
            && incoming_over
            && fade.incoming.is_empty()
        {
            fade.done = fade.length;
            return Ok(FadeStep::Done);
        }
        let ready = |floats: &Vec<f64>, over: bool| {
            if over {
                wanted
            } else {
                floats.len() / channels
            }
        };
        let frames = wanted
            .min(ready(&fade.outgoing, fade.outgoing_done))
            .min(ready(&fade.incoming, incoming_over));
        if frames == 0 {
            return Ok(FadeStep::Wait);
        }
        fade.mix(frames, channels);
        incoming.converter.emit_mixed(&mut fade.mixed, dsp)?;
        if fade.done >= fade.length && !fade.incoming.is_empty() {
            // What the incoming track converted past the fade plays at its own level.
            incoming.converter.emit_mixed(&mut fade.incoming, dsp)?;
            fade.incoming.clear();
        }
        Ok(FadeStep::Mixed)
    }

    // Converts the outgoing track's held frames until `fade` has `frames` of them or the track is over.
    fn fill_outgoing(
        &self,
        outgoing: &mut Segment,
        fade: &mut Fade,
        frames: usize,
        buffers: &mut StageBuffers,
    ) -> Result<(), i32> {
        let channels = self.format.channel_count as usize;
        while fade.outgoing.len() < frames * channels && !fade.outgoing_done {
            if fade.held_read < fade.held.len() {
                let frame_bytes = outgoing.frame_bytes();
                let left = ((fade.held.len() - fade.held_read) / frame_bytes) as u64;
                let count = outgoing
                    .converter
                    .input_frames_for(frames as u64)
                    .min(left)
                    .min(MAX_PREFILL_FRAMES) as usize;
                let end = fade.held_read + count * frame_bytes;
                sample_words(
                    &fade.held[fade.held_read..end],
                    outgoing.sample_bytes,
                    &mut buffers.words,
                );
                fade.held_read = end;
                outgoing
                    .converter
                    .floats(&buffers.words, count, &mut fade.outgoing)?;
            } else {
                outgoing.converter.finish_floats(&mut fade.outgoing)?;
                fade.outgoing_done = true;
            }
        }
        Ok(())
    }

    // Converts the incoming track's decoded frames until `fade` has `frames` of them or no more wait in the source ring.
    fn fill_incoming(
        &self,
        incoming: &mut Segment,
        fade: &mut Fade,
        frames: usize,
        buffers: &mut StageBuffers,
    ) -> Result<(), i32> {
        let channels = self.format.channel_count as usize;
        while fade.incoming.len() < frames * channels {
            let available = incoming.frames - incoming.read;
            if available > 0 {
                let count = incoming
                    .converter
                    .input_frames_for(frames as u64)
                    .min(available)
                    .min(MAX_PREFILL_FRAMES);
                buffers.bytes.clear();
                buffers
                    .bytes
                    .resize(count as usize * incoming.frame_bytes(), 0);
                if self.source_ring.read(&mut buffers.bytes) != buffers.bytes.len() {
                    return Err(ENGINE_E_INVALID_STATE);
                }
                sample_words(&buffers.bytes, incoming.sample_bytes, &mut buffers.words);
                incoming
                    .converter
                    .floats(&buffers.words, count as usize, &mut fade.incoming)?;
                incoming.read += count;
            } else if incoming.ended && !incoming.converter.finished() {
                incoming.converter.finish_floats(&mut fade.incoming)?;
            } else {
                break;
            }
        }
        Ok(())
    }

    // What the crossfade in `fade` would play next, up to `frames` frames as far as the tracks have them, as floats before
    // the DSP chain; past its end the incoming track goes on at its own level. A fade still waiting plays its outgoing
    // side.
    fn render_fade(
        &self,
        outgoing: &mut Segment,
        incoming: &mut Segment,
        fade: &mut Fade,
        frames: usize,
        buffers: &mut StageBuffers,
    ) -> Result<Vec<f64>, i32> {
        let channels = self.format.channel_count as usize;
        if fade.waiting {
            let mut rendered = std::mem::take(&mut fade.outgoing);
            rendered.truncate(frames * channels);
            return Ok(rendered);
        }
        let mut rendered = Vec::with_capacity(frames * channels);
        while rendered.len() < frames * channels {
            let step = (frames - rendered.len() / channels).min(MAX_PREFILL_FRAMES as usize);
            self.fill_outgoing(outgoing, fade, step, buffers)?;
            self.fill_incoming(incoming, fade, step, buffers)?;
            let count = step.min(fade.incoming.len() / channels);
            if count == 0 {
                break;
            }
            fade.mix(count, channels);
            rendered.extend_from_slice(&fade.mixed);
        }
        Ok(rendered)
    }

    // A converter for `source` to this engine's output format, or the reason none exists. A crossfade may permit the
    // conversion for its track.
    fn converter(&self, source: &DecoderSource) -> Result<Converter, i32> {
        Converter::new(
            &source.info,
            &self.format,
            self.output_format.rate_hz,
            self.allow_conversion || source.convert,
            self.allow_dsd_to_pcm || source.convert,
            self.resample_quality,
        )
        .map(|converter| converter.with_gain(source.gain))
    }

    // Positions are counted in output frames; a source frame maps through the rate ratio.
    fn output_frames(&self, frames: u64, source_rate: u32) -> u64 {
        let frame_rate = u64::from(self.output_format.rate_hz) / units_per_frame(&self.format);
        (u128::from(frames) * u128::from(frame_rate) / u128::from(source_rate.max(1))) as u64
    }

    // Silence the request has not been given yet is already in place; DoP frames get their markers here.
    fn seal(&self, output: &mut [u8]) {
        if self.format.stream_kind == AUDIO_DOP {
            let phase = stamp_markers(&self.format, output, self.dop_phase.load(Ordering::Relaxed));
            self.dop_phase.store(phase, Ordering::Relaxed);
        }
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

fn whole_units(format: &AudioFormat, units: u64) -> u64 {
    if format.stream_kind == AUDIO_DSD {
        units - units % 8
    } else {
        units
    }
}

// The decoded ring: `prebuffer_ms` (0: DEFAULT_PREBUFFER_MS) of source frames at the device rate and channel count,
// as 32-bit sample words for PCM and as bytes for raw DSD, at least one prefill of the widest source and at most
// MAX_RING_BYTES, or MAX_BUFFERING_RING_BYTES when more than the default is asked for. A source with more channels or a
// higher rate than the device fits fewer milliseconds.
fn source_ring_capacity(format: &AudioFormat, rate_hz: u32, prebuffer_ms: u32) -> Option<usize> {
    let milliseconds = match u64::from(prebuffer_ms) {
        0 => DEFAULT_PREBUFFER_MS,
        ms => ms,
    };
    let units = u64::from(rate_hz).saturating_mul(milliseconds) / 1000;
    let prebuffer = if format.stream_kind == AUDIO_PCM {
        units.saturating_mul(u64::from(format.channel_count) * 4)
    } else {
        // A raw DSD source byte never takes more room than the device bytes carrying it.
        format
            .required_bytes(whole_units(format, units))
            .unwrap_or(u64::MAX)
    };
    let minimum = MAX_PREFILL_FRAMES * MAX_SOURCE_FRAME_BYTES;
    let limit = if milliseconds > DEFAULT_PREBUFFER_MS {
        MAX_BUFFERING_RING_BYTES
    } else {
        MAX_RING_BYTES
    };
    usize::try_from(prebuffer.min(limit).max(minimum)).ok()
}

// The device ring: DSP_LEAD_MS of device frames, at least MIN_DEVICE_RING_FRAMES.
fn device_ring_capacity(format: &AudioFormat, rate_hz: u32) -> Option<usize> {
    let lead = u64::from(rate_hz).saturating_mul(DSP_LEAD_MS) / 1000;
    let minimum = MIN_DEVICE_RING_FRAMES * units_per_frame(format);
    let bytes = format
        .required_bytes(whole_units(format, lead.max(minimum)))
        .ok()?;
    usize::try_from(bytes.clamp(1, MAX_RING_BYTES)).ok()
}

#[no_mangle]
pub extern "C" fn camel_engine_abi_version() -> u32 {
    ABI_VERSION
}

/// # Safety
/// `config` must point to a readable V1 configuration and `out_engine` to writable storage.
#[no_mangle]
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
                let rate_hz = (*config).format.rate_hz;
                let rings = source_ring_capacity(&format, rate_hz, (*config).prebuffer_ms)
                    .zip(device_ring_capacity(&format, rate_hz))
                    .and_then(|(source, device)| {
                        SpscByteRing::new(source)
                            .ok()
                            .zip(SpscByteRing::new(device).ok())
                    });
                let Some((source_ring, ring)) = rings else {
                    return ENGINE_E_LIMIT_EXCEEDED;
                };
                *out_engine = Box::into_raw(Box::new(CamelEngine {
                    format,
                    output_format: (*config).format,
                    allow_conversion: (*config).allow_conversion != 0,
                    allow_dsd_to_pcm: (*config).allow_dsd_to_pcm != 0,
                    resample_quality: (*config).resample_quality,
                    max_render_units,
                    source_ring,
                    ring,
                    stage: Mutex::new(Stage {
                        segments: std::collections::VecDeque::new(),
                        stream_ended: false,
                        dsp: dsp::Dsp::new(rate_hz, format.channel_count as usize),
                        tail_left: None,
                        fade: None,
                        buffers: StageBuffers::new(format.channel_count as usize),
                        retired: Vec::with_capacity(RETIRED_SEGMENTS),
                    }),
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
                    source_view: Mutex::new(None),
                    finished_view: Mutex::new(None),
                    next_source: Mutex::new(None),
                    written_frames: AtomicU64::new(0),
                    read_frames: AtomicU64::new(0),
                    boundary_at: AtomicU64::new(NO_BOUNDARY),
                    boundary_start_frame: AtomicU64::new(0),
                    pending_boundary: AtomicBool::new(false),
                    transitions: AtomicU64::new(0),
                    next_occupied: AtomicBool::new(false),
                    dop_phase: AtomicUsize::new(0),
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

// Accepts a newer command without cancelling in-flight decoder work.
fn record_command(engine: &CamelEngine, command: &CamelCommandStampV1) -> Result<(), EngineStatus> {
    validate_command(command)?;
    let mut current = engine.command.lock().unwrap();
    if command.command_id <= engine.issued_command_id.load(Ordering::Acquire) {
        return Err(EngineStatus::NotReady);
    }
    *current = *command;
    engine
        .issued_command_id
        .store(command.command_id, Ordering::Release);
    Ok(())
}

fn begin_command(engine: &CamelEngine, command: &CamelCommandStampV1) -> Result<u64, EngineStatus> {
    record_command(engine, command)?;
    Ok(engine.operation_epoch.fetch_add(1, Ordering::AcqRel) + 1)
}

unsafe fn track_host(
    source: &CamelTrackSourceV1,
    decoder_host: &CamelDecoderHostV1,
) -> Result<DecoderHost, i32> {
    if source.abi_version != ABI_VERSION {
        return Err(ENGINE_E_ABI_MISMATCH);
    }
    if source.struct_size < std::mem::size_of::<CamelTrackSourceV1>() as u32
        || decoder_host.abi_version != ABI_VERSION
        || decoder_host.struct_size < std::mem::size_of::<CamelDecoderHostV1>() as u32
    {
        return Err(ENGINE_E_STRUCT_SIZE);
    }
    DecoderHost::from_abi(decoder_host).ok_or(ENGINE_E_INVALID_ARGUMENT)
}

fn operation_is_current(engine: &CamelEngine, epoch: u64) -> bool {
    engine.operation_epoch.load(Ordering::Acquire) == epoch
}

/// # Safety
/// All pointers must reference valid V1 structures. Stop and drain render before replacing the source. Decoder callbacks and their context must remain valid until replacement or destruction. Commands must have strictly increasing IDs.
#[no_mangle]
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
        let host = match track_host(source, &*decoder_host) {
            Ok(host) => host,
            Err(status) => return status,
        };
        let epoch = match begin_command(engine, command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let mut new_source = match DecoderSource::open(source, decoder_kind, host) {
            Ok(source) => source,
            Err(status) => return status,
        };
        match engine.converter(&new_source) {
            Ok(converter) => new_source.converter = Some(converter),
            Err(status) => return status,
        }
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let position = engine.output_frames(new_source.position(), new_source.info.sample_rate);
        let mut current_source = engine.source.lock().unwrap();
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        engine.reset_stream();
        *engine.next_source.lock().unwrap() = None;
        engine.next_occupied.store(false, Ordering::Release);
        engine.submitted_position.store(position, Ordering::Release);
        engine.audible_position.store(position, Ordering::Release);
        engine.render_error.store(ENGINE_OK, Ordering::Release);
        engine
            .render_source_state
            .store(RENDER_STATE_ACTIVE as usize, Ordering::Release);
        *current_source = Some(new_source);
        engine.publish_source(current_source.as_ref());
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
#[no_mangle]
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
        // After a gapless swap the decoder already reads the next track while render still plays this one.
        if engine.boundary_pending() {
            return ENGINE_E_INVALID_STATE;
        }
        if !source.can_seek(frame) {
            return ENGINE_E_SEEK;
        }
        // Resampler state and pending output belong to the old position.
        let converter = match engine.converter(source) {
            Ok(converter) => converter,
            Err(status) => return status,
        };
        let seeked = source.seek(frame);
        source.converter = Some(converter);
        let output_frame = engine.output_frames(frame, source.info.sample_rate);
        engine.publish_source(Some(source));
        if let Err(status) = seeked {
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
        engine.reset_stream();
        engine
            .submitted_position
            .store(output_frame, Ordering::Release);
        engine
            .audible_position
            .store(output_frame, Ordering::Release);
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
#[no_mangle]
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
        if engine.view().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        // Only the state changes: decoding in progress stays valid, so the source keeps its full MD5 coverage.
        if let Err(status) = record_command(engine, command) {
            return status as i32;
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
#[no_mangle]
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
        if engine.view().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        // Only the state changes: decoding in progress stays valid, so the source keeps its full MD5 coverage.
        if let Err(status) = record_command(engine, command) {
            return status as i32;
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
#[no_mangle]
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
            let seeked = source.seek(start_frame);
            if let Ok(converter) = engine.converter(source) {
                source.converter = Some(converter);
            }
            engine.publish_source(Some(source));
            if let Err(status) = seeked {
                engine
                    .playback_state
                    .store(PLAYBACK_FAILED as usize, Ordering::Release);
                return status;
            }
            Some(engine.output_frames(start_frame, source.info.sample_rate))
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
        engine.reset_stream();
        *engine.next_source.lock().unwrap() = None;
        engine.next_occupied.store(false, Ordering::Release);
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
#[no_mangle]
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
        let Some(_producer) = engine.prefill_gate.enter() else {
            return ENGINE_E_INVALID_STATE;
        };
        let epoch = engine.operation_epoch.load(Ordering::Acquire);

        let mut guard = engine.source.lock().unwrap();
        let status = (|| {
            let Some(source) = guard.as_mut() else {
                return ENGINE_E_INVALID_STATE;
            };
            if !operation_is_current(engine, epoch) {
                return ENGINE_E_NOT_READY;
            }
            let frame_bytes = {
                let mut stage = engine.stage.lock().unwrap();
                stage.retired.clear();
                // A loaded, seeked or stopped source starts its segment with the converter it was given.
                if let Some(converter) = source.converter.take() {
                    stage
                        .segments
                        .push_back(Segment::new(converter, source, None, 0));
                }
                // With no segment the track has ended and was played out; reading only finds its end.
                stage.segments.back().map_or_else(
                    || Segment::sample_bytes(&source.info) * source.info.channel_count as usize,
                    Segment::frame_bytes,
                )
            };
            let source_frames = frame_limit
                .min(MAX_PREFILL_FRAMES)
                .min((engine.source_ring.writable() / frame_bytes) as u64);
            if source_frames == 0 {
                return ENGINE_OK;
            }
            let sample_count = match source_frames
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
            let frames_read = match source.read(&mut samples, source_frames) {
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
            let _current_command = engine.command.lock().unwrap();
            if !operation_is_current(engine, epoch) {
                let _ = source.seek(start_position);
                return ENGINE_E_NOT_READY; // seek, stop and load replace the stream
            }
            let mut stage = engine.stage.lock().unwrap();
            if frames_read == 0 {
                if let Some(segment) = stage.segments.back_mut() {
                    segment.ended = true;
                }
                let mut next = engine.next_source.lock().unwrap();
                if next.is_some() {
                    // One boundary at a time: a second queued track waits until render reaches the first.
                    if !engine.boundary_pending() {
                        if let Some(mut next) = next.take() {
                            *engine.finished_view.lock().unwrap() = Some(SourceView::of(source));
                            let start =
                                engine.output_frames(next.start_frame(), next.info.sample_rate);
                            let converter = next
                                .converter
                                .take()
                                .expect("queued sources have a converter");
                            let fade_in = engine.fade_frames(next.crossfade_ms);
                            let mut segment = Segment::new(converter, &next, Some(start), fade_in);
                            segment.held = std::mem::take(&mut next.held);
                            stage.segments.push_back(segment);
                            *source = next;
                        }
                        engine.next_occupied.store(false, Ordering::Release);
                        engine.pending_boundary.store(true, Ordering::Release);
                        stage.stream_ended = false;
                        // A track queued after the previous end was reached keeps the stream going.
                        engine
                            .render_source_state
                            .store(RENDER_STATE_ACTIVE as usize, Ordering::Release);
                    }
                    return ENGINE_OK;
                }
                stage.stream_ended = true;
                return ENGINE_END;
            }
            let used = &samples[..frames_read as usize * source.info.channel_count as usize];
            let mut bytes = Vec::with_capacity(frames_read as usize * frame_bytes);
            if frame_bytes == source.info.channel_count as usize {
                bytes.extend(used.iter().map(|&s| s as u8));
            } else {
                bytes.extend(used.iter().flat_map(|s| s.to_le_bytes()));
            }
            if engine.source_ring.write(&bytes) != bytes.len() {
                drop(stage);
                engine.fail_render(ENGINE_E_INVALID_STATE);
                return ENGINE_E_INVALID_STATE;
            }
            if let Some(segment) = stage.segments.back_mut() {
                segment.frames += frames_read;
                // A length the decoder learned while playing: the crossfade out of the track can now start in time.
                if segment.expected == 0 && source.info.total_frames != 0 {
                    segment.expected = segment.frames + source.remaining_frames();
                }
            }
            // Counted in device frames, like every position.
            *frames_prefilled = engine
                .output_frames(frames_read, source.info.sample_rate)
                .max(1);
            ENGINE_OK
        })();
        // The decode worker keeps the device ring filled too; the DSP worker does it while decoding waits for I/O.
        let status = match engine.pump() {
            Err(error) if status >= 0 => error,
            _ => status,
        };
        engine.publish_source(guard.as_ref());
        status
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
/// `engine` and `frames_processed` must reference a live handle and writable storage.
///
/// The DSP stage: turns decoded frames into device bytes until the device ring is full. Prefill runs it too; a
/// separate worker calls this so the device ring stays filled while decoding waits for slow storage.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_process_v1(
    engine: *mut CamelEngine,
    frames_processed: *mut u64,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || frames_processed.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        *frames_processed = 0;
        match (&*engine).pump() {
            Ok(frames) => {
                *frames_processed = frames;
                ENGINE_OK
            }
            Err(status) => status,
        }
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
#[no_mangle]
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
        let mut guard = engine.source.lock().unwrap();
        let status = (|| {
            let Some(source) = guard.as_mut() else {
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
        })();
        engine.publish_source(guard.as_ref());
        status
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `state` must reference a live handle and writable V1 state.
#[no_mangle]
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
        let source = engine.view();
        state.playback_state = engine.playback_state.load(Ordering::Acquire) as u32;
        state.last_error_code = engine.render_error.load(Ordering::Acquire);
        state.command = command;
        state.position_frames = engine.submitted_position.load(Ordering::Acquire);
        state.total_frames = engine.heard_view().map_or(0, |s| s.info.total_frames);
        state.has_source = u32::from(source.is_some());
        state.reserved = 0;
        state.source_format = source.map_or(empty_format(), |s| source_audio_format(&s.info));
        state.output_format = engine.output_format;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `timeline` must reference a live engine and writable V1 timeline.
#[no_mangle]
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
        let source = engine.view();
        timeline.playback_state = engine.playback_state.load(Ordering::Acquire) as u32;
        timeline.reserved = 0;
        timeline.command = command;
        timeline.decoded_position_frames = source.map_or(0, |s| s.decoded_position);
        timeline.submitted_position_frames = engine.submitted_position.load(Ordering::Acquire);
        timeline.audible_position_frames = engine.audible_position.load(Ordering::Acquire);
        timeline.total_frames = engine.heard_view().map_or(0, |s| s.info.total_frames);
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
#[no_mangle]
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
#[no_mangle]
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
        let engine = &*engine;
        let Some(source) = engine.heard_view() else {
            integrity.status = FLAC_INTEGRITY_UNAVAILABLE;
            integrity.has_reference_md5 = 0;
            integrity.total_frames = 0;
            integrity.position_frames = 0;
            return ENGINE_OK;
        };
        integrity.status = source.integrity_status;
        integrity.has_reference_md5 = u32::from(source.has_reference_md5);
        integrity.total_frames = source.info.total_frames;
        integrity.position_frames = source.decoded_position;
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// All pointers must reference valid V1 structures for the call; decoder callbacks and their context must remain
/// valid until the queued source is replaced, discarded or the Engine is destroyed. Commands must have strictly
/// increasing IDs.
///
/// Queues the track that follows the current one gaplessly. It must decode to the current output format; otherwise
/// the call fails and playback ends normally at the current track. Queuing again replaces a track not yet started.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_queue_next_flac_v1(
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
        let source = &*source;
        let host = match track_host(source, &*decoder_host) {
            Ok(host) => host,
            Err(status) => return status,
        };
        if engine.view().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        if let Err(status) = record_command(engine, &*command) {
            return status as i32;
        }
        let mut next = match DecoderSource::open(source, decoder_kind, host) {
            Ok(next) => next,
            Err(status) => return status,
        };
        // Room for the playing track's tail while it fades into this one, so the DSP stage needs no heap for it.
        if next.crossfade_ms > 0 {
            if let Some(view) = engine.view() {
                let frames = u64::from(view.info.sample_rate) * u64::from(next.crossfade_ms) / 1000
                    + MAX_PREFILL_FRAMES;
                let bytes = frames
                    * u64::from(view.info.channel_count)
                    * Segment::sample_bytes(&view.info) as u64;
                next.held = Vec::with_capacity((bytes as usize).min(engine.source_ring.capacity()));
            }
        }
        // The queued track plays into the same device format; a converter takes it there or it is refused.
        match engine.converter(&next) {
            Ok(converter) => next.converter = Some(converter),
            Err(status) => return status,
        }
        let mut slot = engine.next_source.lock().unwrap();
        *slot = Some(next);
        engine.next_occupied.store(true, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` and `command` must remain valid for the call. Commands must have strictly increasing IDs.
///
/// Discards a queued next track. Fails with `CAMEL_ENGINE_E_INVALID_STATE` once decoding has already moved on to
/// it; that track then becomes current at its boundary as queued.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_clear_next_v1(
    engine: *mut CamelEngine,
    command: *const CamelCommandStampV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || command.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let engine = &*engine;
        if let Err(status) = record_command(engine, &*command) {
            return status as i32;
        }
        let mut next = engine.next_source.lock().unwrap();
        if next.is_none() && engine.boundary_pending() {
            return ENGINE_E_INVALID_STATE;
        }
        *next = None;
        engine.next_occupied.store(false, Ordering::Release);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// All pointers must reference valid V1 structures. Decoder callbacks and their context must remain valid until
/// replacement or destruction. Commands must have strictly increasing IDs.
///
/// A manual change during playback (docs/Plan.md §5.4): keeps the next `crossfade_ms` of the playing track and a second
/// more while `source` starts decoding, discards the rest and any queued track, and crossfades into `source`, which
/// becomes current at the fade. During a crossfade what it would still play fades out as one track. A boundary still on
/// its way to the device is heard first, so the new track's is the next transition. Fails with
/// `CAMEL_ENGINE_E_INVALID_STATE` when nothing plays that can be mixed with it, or that boundary is not heard within a
/// second; the caller then loads the track instead.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_crossfade_to_v1(
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
        let source = &*source;
        let host = match track_host(source, &*decoder_host) {
            Ok(host) => host,
            Err(status) => return status,
        };
        if source.crossfade_ms == 0 {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        if engine.view().is_none() {
            return ENGINE_E_INVALID_STATE;
        }
        let epoch = match begin_command(engine, &*command) {
            Ok(epoch) => epoch,
            Err(status) => return status as i32,
        };
        let mut next = match DecoderSource::open(source, decoder_kind, host) {
            Ok(next) => next,
            Err(status) => return status,
        };
        let converter = match engine.converter(&next) {
            Ok(converter) => converter,
            Err(status) => return status,
        };
        if !converter.mixes() {
            return ENGINE_E_INVALID_STATE;
        }
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(1);
        while engine.boundary_at.load(Ordering::Acquire) != NO_BOUNDARY
            && std::time::Instant::now() < deadline
        {
            thread::sleep(std::time::Duration::from_millis(2));
        }
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let mut current_source = engine.source.lock().unwrap();
        let _current_command = engine.command.lock().unwrap();
        if !operation_is_current(engine, epoch) {
            return ENGINE_E_NOT_READY;
        }
        let mut stage = engine.stage.lock().unwrap();
        if engine.boundary_at.load(Ordering::Acquire) != NO_BOUNDARY {
            return ENGINE_E_INVALID_STATE;
        }
        let fade_in = engine.fade_frames(next.crossfade_ms);
        let keep_frames = fade_in + engine.fade_frames(CUT_MARGIN_MS);
        let next_length = engine.output_frames(next.remaining_frames(), next.info.sample_rate);
        let Stage {
            segments,
            stream_ended,
            tail_left,
            fade,
            buffers,
            ..
        } = &mut *stage;
        let mut held_bytes = 0;
        if let Some(mut playing) = fade.take() {
            let [outgoing, incoming, ..] = segments.make_contiguous() else {
                return ENGINE_E_INVALID_STATE;
            };
            let rendered = engine.render_fade(
                outgoing,
                incoming,
                &mut playing,
                keep_frames as usize,
                buffers,
            );
            buffers.outgoing = playing.outgoing;
            buffers.incoming = playing.incoming;
            buffers.mixed = playing.mixed;
            let mix = match rendered {
                Ok(mix) => mix,
                Err(status) => {
                    drop(stage);
                    engine.fail_render(status);
                    return status;
                }
            };
            // The track that was fading in stays first only to play out what it has already put out.
            segments.pop_front();
            segments.truncate(1);
            let held = &mut segments[0];
            held.frames = held.read;
            held.ended = true;
            engine.source_ring.clear();
            let mut length = fade_in.min((mix.len() / engine.format.channel_count as usize) as u64);
            if next_length > 0 {
                length = length.min(next_length / 2);
            }
            // The mix takes the place of the lent outgoing buffer, which goes here rather than in the DSP stage.
            let (_, incoming, mixed) = buffers.lend();
            *fade = Some(Fade {
                length,
                done: 0,
                held: Vec::new(),
                held_read: 0,
                outgoing: mix,
                outgoing_done: true,
                incoming,
                mixed,
                waiting: true,
            });
        } else {
            let Some(playing) = segments.front_mut() else {
                return ENGINE_E_INVALID_STATE;
            };
            if !playing.converter.mixes() {
                return ENGINE_E_INVALID_STATE;
            }
            let keep = playing
                .converter
                .input_frames_for(keep_frames)
                .min(playing.frames - playing.read);
            let mut kept = vec![0u8; keep as usize * playing.frame_bytes()];
            if engine.source_ring.read(&mut kept) != kept.len() {
                return ENGINE_E_INVALID_STATE;
            }
            engine.source_ring.clear();
            if engine.source_ring.write(&kept) != kept.len() {
                drop(stage);
                engine.fail_render(ENGINE_E_INVALID_STATE);
                return ENGINE_E_INVALID_STATE;
            }
            playing.frames = playing.read + keep;
            playing.ended = true;
            playing.cut = true;
            segments.truncate(1);
            held_bytes = kept.len();
        }
        *stream_ended = false;
        *tail_left = None;
        let start = engine.output_frames(next.start_frame(), next.info.sample_rate);
        let mut segment = Segment::new(converter, &next, Some(start), fade_in);
        segment.held = Vec::with_capacity(held_bytes);
        segments.push_back(segment);
        drop(stage);
        *engine.next_source.lock().unwrap() = None;
        engine.next_occupied.store(false, Ordering::Release);
        *engine.finished_view.lock().unwrap() = current_source.as_ref().map(SourceView::of);
        engine.pending_boundary.store(true, Ordering::Release);
        engine
            .render_source_state
            .store(RENDER_STATE_ACTIVE as usize, Ordering::Release);
        next.converter = None;
        *current_source = Some(next);
        engine.publish_source(current_source.as_ref());
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` must be live; the output pointers must be writable. Safe on the output callback thread: it only reads
/// atomics.
///
/// Reports how many queued tracks have become audible, the current track position (submitted frames) and whether
/// a queued track still waits in the slot.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_get_transitions_v1(
    engine: *mut CamelEngine,
    transitions: *mut u64,
    track_position: *mut u64,
    next_queued: *mut u32,
) -> i32 {
    if engine.is_null()
        || transitions.is_null()
        || track_position.is_null()
        || next_queued.is_null()
    {
        return ENGINE_E_INVALID_ARGUMENT;
    }
    let engine = &*engine;
    *transitions = engine.transitions.load(Ordering::Acquire);
    *track_position = engine.submitted_position.load(Ordering::Acquire);
    *next_queued = u32::from(engine.next_occupied.load(Ordering::Acquire));
    ENGINE_OK
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
#[no_mangle]
pub unsafe extern "C" fn camel_engine_render_v1(
    engine: *mut CamelEngine,
    request: *const CamelRenderRequestV1,
    result: *mut CamelRenderResultV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        #[cfg(test)]
        let _inside = stage_heap::Inside::enter();
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

        let Some(bytes_per_frame) = bytes_per_frame(&engine.format) else {
            return ENGINE_E_UNSUPPORTED_FORMAT;
        };
        let output_bytes = match usize::try_from(required_bytes) {
            Ok(bytes) => bytes,
            Err(_) => return ENGINE_E_LIMIT_EXCEEDED,
        };
        let output: &mut [u8] = if output_bytes > 0 {
            std::slice::from_raw_parts_mut(request.output, output_bytes)
        } else {
            &mut []
        };
        let request_frames = request.units / units_per_frame(&engine.format);
        // A raw DSD stream never hands out zero bytes: every request is filled with silence first, so the device
        // always receives the idle pattern (with continuing DoP markers) where there is no audio.
        let raw = engine.format.stream_kind != AUDIO_PCM;
        if raw {
            fill_silence(&engine.format, output);
        }
        let source_state = engine.render_source_state.load(Ordering::Acquire) as u32;
        if source_state == RENDER_STATE_NO_SOURCE {
            engine.seal(output);
            return ENGINE_E_NOT_READY;
        }
        if source_state == RENDER_STATE_FAULT {
            if !raw {
                output.fill(0);
            }
            engine.seal(output);
            result.state = RENDER_STATE_FAULT;
            return engine.render_error.load(Ordering::Acquire);
        }
        if !raw {
            output.fill(0);
        }
        if engine.playback_state.load(Ordering::Acquire) as u32 != PLAYBACK_PLAYING {
            // Only playback consumes audio; otherwise hand out silence and keep the buffered frames.
            engine.seal(output);
            let drained = source_state == RENDER_STATE_END_OF_STREAM && engine.ring.readable() == 0;
            if drained {
                result.state = RENDER_STATE_END_OF_STREAM;
                return if request.units > 0 {
                    ENGINE_END
                } else {
                    ENGINE_OK
                };
            }
            result.state = RENDER_STATE_PAUSED;
            return ENGINE_OK;
        }
        let available = engine.ring.readable().min(output_bytes);
        let rendered_bytes = if available > 0 {
            engine.ring.read(&mut output[..available])
        } else {
            0
        };
        engine.seal(output);
        let rendered_frames = rendered_bytes / bytes_per_frame;
        result.units_rendered = rendered_frames as u64 * units_per_frame(&engine.format);
        let before = engine
            .read_frames
            .fetch_add(rendered_frames as u64, Ordering::AcqRel);
        let after = before + rendered_frames as u64;
        let boundary = engine.boundary_at.load(Ordering::Acquire);
        if boundary != NO_BOUNDARY && boundary <= after {
            // The queued track is now audible: its position restarts at the boundary.
            engine.boundary_at.store(NO_BOUNDARY, Ordering::Release);
            engine.pending_boundary.store(false, Ordering::Release);
            let start = engine.boundary_start_frame.load(Ordering::Acquire);
            engine
                .submitted_position
                .store(start + (after - boundary), Ordering::Release);
            engine.audible_position.store(start, Ordering::Release);
            engine.transitions.fetch_add(1, Ordering::AcqRel);
        } else {
            engine
                .submitted_position
                .fetch_add(rendered_frames as u64, Ordering::AcqRel);
        }

        let source_state = engine.render_source_state.load(Ordering::Acquire) as u32;
        if source_state == RENDER_STATE_END_OF_STREAM && engine.ring.readable() == 0 {
            result.state = RENDER_STATE_END_OF_STREAM;
            engine
                .playback_state
                .store(PLAYBACK_STOPPED as usize, Ordering::Release);
            if (rendered_frames as u64) < request_frames {
                return ENGINE_END;
            }
        } else if (rendered_frames as u64) < request_frames {
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

// The enabled bands of `count` V1 bands at `bands`, or the reason they cannot be read.
unsafe fn eq_bands(bands: *const CamelEqBandV1, count: u32) -> Result<Vec<eq::Band>, i32> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if bands.is_null() {
        return Err(ENGINE_E_INVALID_ARGUMENT);
    }
    let mut out = Vec::new();
    for band in std::slice::from_raw_parts(bands, count as usize) {
        if band.abi_version != ABI_VERSION {
            return Err(ENGINE_E_ABI_MISMATCH);
        }
        if band.struct_size < std::mem::size_of::<CamelEqBandV1>() as u32 {
            return Err(ENGINE_E_STRUCT_SIZE);
        }
        if band.enabled != 0 {
            out.push(eq::Band {
                kind: band.kind,
                channels: band.channels,
                slope: band.slope,
                frequency: band.frequency_hz,
                gain_db: band.gain_db,
                q: band.q,
            });
        }
    }
    Ok(out)
}

// The DSP settings at `dsp`, or the reason they cannot be read.
unsafe fn dsp_settings(dsp: *const CamelDspV1) -> Result<dsp::Settings, i32> {
    if dsp.is_null() {
        return Err(ENGINE_E_INVALID_ARGUMENT);
    }
    let dsp = &*dsp;
    if dsp.abi_version != ABI_VERSION {
        return Err(ENGINE_E_ABI_MISMATCH);
    }
    if dsp.struct_size < std::mem::size_of::<CamelDspV1>() as u32 {
        return Err(ENGINE_E_STRUCT_SIZE);
    }
    let crossfeed = if dsp.crossfeed.is_null() {
        None
    } else {
        let c = &*dsp.crossfeed;
        if c.abi_version != ABI_VERSION {
            return Err(ENGINE_E_ABI_MISMATCH);
        }
        if c.struct_size < std::mem::size_of::<CamelCrossfeedV1>() as u32 {
            return Err(ENGINE_E_STRUCT_SIZE);
        }
        Some(crossfeed::Params {
            cutoff_hz: c.cutoff_hz,
            feed_db: c.feed_db,
        })
    };
    Ok(dsp::Settings {
        order: dsp.order,
        bands: eq_bands(dsp.bands, dsp.band_count)?,
        crossfeed,
        impulse: (!dsp.impulse.is_null()).then(|| (*dsp.impulse).0.clone()),
        volume_db: dsp.volume_db,
    })
}

/// # Safety
/// `impulse` must point to a readable V1 impulse response whose samples are readable, and `out` must be writable.
///
/// Prepares an impulse response for convolution at its rate; the work of splitting it is done here, once. The handle
/// is released with `camel_impulse_destroy_v1`, and engines using it keep what they need.
#[no_mangle]
pub unsafe extern "C" fn camel_impulse_create_v1(
    impulse: *const CamelImpulseResponseV1,
    out: *mut *mut CamelImpulse,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if impulse.is_null() || out.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        *out = ptr::null_mut();
        let ir = &*impulse;
        if ir.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if ir.struct_size < std::mem::size_of::<CamelImpulseResponseV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        let Some(values) = (ir.frames as usize).checked_mul(ir.channels as usize) else {
            return ENGINE_E_INVALID_ARGUMENT;
        };
        if ir.samples.is_null() || values == 0 {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let samples = std::slice::from_raw_parts(ir.samples, values);
        match convolver::ImpulseResponse::new(samples, ir.channels as usize, ir.rate_hz) {
            Some(prepared) => {
                *out = Box::into_raw(Box::new(CamelImpulse(Arc::new(prepared))));
                ENGINE_OK
            }
            None => ENGINE_E_INVALID_ARGUMENT,
        }
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `impulse` must be null or a handle from `camel_impulse_create_v1` not yet destroyed.
#[no_mangle]
pub unsafe extern "C" fn camel_impulse_destroy_v1(impulse: *mut CamelImpulse) -> i32 {
    if !impulse.is_null() {
        drop(Box::from_raw(impulse));
    }
    ENGINE_OK
}

/// # Safety
/// `engine` must be live and `dsp` must point to a readable V1 setting whose pointers are null or readable.
///
/// Sets the DSP chain. Bands that cannot change the signal are skipped, and a chain left without anything that changes
/// it is bypassed, keeping playback bit-perfect. An impulse response must be at the engine's rate. The change fades in
/// and is heard within the DSP lead.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_set_dsp_v1(
    engine: *mut CamelEngine,
    dsp: *const CamelDspV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let settings = match dsp_settings(dsp) {
            Ok(settings) => settings,
            Err(status) => return status,
        };
        let engine = &*engine;
        if !settings.valid(engine.output_format.rate_hz) {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        engine.stage.lock().unwrap().dsp.set(settings);
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` must be live and `clipped` writable.
///
/// The samples the DSP chain has sent above full scale since the engine was created; the final quantization clips
/// them.
#[no_mangle]
pub unsafe extern "C" fn camel_engine_get_dsp_clipped_v1(
    engine: *mut CamelEngine,
    clipped: *mut u64,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if engine.is_null() || clipped.is_null() {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        *clipped = (&*engine).stage.lock().unwrap().dsp.clipped();
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `dsp` must point to a readable V1 setting whose pointers are null or readable; `frequencies`, `left_db` and
/// `right_db` to `points` values each (any pointers when `points` is 0); `summary` to a writable V1 summary.
///
/// Analyzes a DSP setting at `rate_hz` on a stereo device without an engine: the level the EQ's bands give each
/// channel at each frequency, for its response curve, whether the chain changes the signal and the preamp it needs.
#[no_mangle]
pub unsafe extern "C" fn camel_dsp_analyze_v1(
    dsp: *const CamelDspV1,
    rate_hz: u32,
    frequencies: *const f64,
    points: u32,
    left_db: *mut f64,
    right_db: *mut f64,
    summary: *mut CamelDspSummaryV1,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(|| {
        if summary.is_null()
            || rate_hz == 0
            || (points > 0 && (frequencies.is_null() || left_db.is_null() || right_db.is_null()))
        {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let summary = &mut *summary;
        if summary.abi_version != ABI_VERSION {
            return ENGINE_E_ABI_MISMATCH;
        }
        if summary.struct_size < std::mem::size_of::<CamelDspSummaryV1>() as u32 {
            return ENGINE_E_STRUCT_SIZE;
        }
        let settings = match dsp_settings(dsp) {
            Ok(settings) => settings,
            Err(status) => return status,
        };
        if !settings.valid(rate_hz) {
            return ENGINE_E_INVALID_ARGUMENT;
        }
        let analysis = dsp::analyze(&settings, rate_hz, 2);
        summary.active = u32::from(analysis.active);
        summary.reserved = 0;
        summary.preamp_db = analysis.preamp_db;
        let points = points as usize;
        if points > 0 {
            let rate = f64::from(rate_hz);
            let frequencies = std::slice::from_raw_parts(frequencies, points);
            let left = std::slice::from_raw_parts_mut(left_db, points);
            let right = std::slice::from_raw_parts_mut(right_db, points);
            for (i, &f) in frequencies.iter().enumerate() {
                left[i] = eq::response_db(&settings.bands, rate, 0, f);
                right[i] = eq::response_db(&settings.bands, rate, 1, f);
            }
        }
        ENGINE_OK
    })) {
        Ok(status) => status,
        Err(_) => ENGINE_E_PANIC,
    }
}

/// # Safety
/// `engine` must point to a live handle. The caller must prevent new render calls before quiesce.
#[no_mangle]
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
#[no_mangle]
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
            sample_encoding: 0,
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
            length: None,
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
            length: None,
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
            length: None,
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
            length: None,
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
            rate_hz: 48_000,
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
            allow_conversion: 0,
            resample_quality: QUALITY_HIGH,
            allow_dsd_to_pcm: 0,
            prebuffer_ms: 0,
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
        load_mock_source_with_gain(engine, 0.0)
    }

    fn load_mock_source_with_gain(engine: *mut CamelEngine, gain_db: f64) -> i32 {
        load_source_with_host(engine, &mock_host(), gain_db)
    }

    fn load_source_with_host(
        engine: *mut CamelEngine,
        host: &CamelDecoderHostV1,
        gain_db: f64,
    ) -> i32 {
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
            gain_db,
            crossfade_ms: 0,
            flags: 0,
        };
        unsafe {
            camel_engine_load_flac_v1(
                engine,
                &command,
                &source,
                decoder_host::DECODER_SYMPHONIA,
                host,
            )
        }
    }

    fn queue_mock_next(engine: *mut CamelEngine, command_id: u64) -> i32 {
        let path = b"next.flac";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        let host = mock_host();
        unsafe {
            camel_engine_queue_next_flac_v1(
                engine,
                &command_stamp(command_id),
                &source,
                decoder_host::DECODER_SYMPHONIA,
                &host,
            )
        }
    }

    fn transitions(engine: *mut CamelEngine) -> (u64, u64, u32) {
        let (mut count, mut position, mut queued) = (0, 0, 0);
        assert_eq!(
            unsafe {
                camel_engine_get_transitions_v1(engine, &mut count, &mut position, &mut queued)
            },
            ENGINE_OK
        );
        (count, position, queued)
    }

    fn mock_engine_at_48k() -> *mut CamelEngine {
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        cfg.format.rate_hz = 48_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);
        engine
    }

    fn prefill_until_end(engine: *mut CamelEngine) -> u64 {
        let mut total = 0;
        loop {
            let mut frames = 0;
            match unsafe { camel_engine_prefill_v1(engine, 4, &mut frames) } {
                ENGINE_OK => total += frames,
                ENGINE_END => return total,
                status => panic!("prefill failed: {status}"),
            }
        }
    }

    fn eq_band(kind: u32, frequency_hz: f64, gain_db: f64, enabled: bool) -> CamelEqBandV1 {
        CamelEqBandV1 {
            struct_size: std::mem::size_of::<CamelEqBandV1>() as u32,
            abi_version: ABI_VERSION,
            kind,
            channels: eq::CHANNELS_ALL,
            slope: 12,
            enabled: u32::from(enabled),
            frequency_hz,
            gain_db,
            q: 0.71,
        }
    }

    fn dsp_setting(
        bands: &[CamelEqBandV1],
        crossfeed: Option<&CamelCrossfeedV1>,
        impulse: *const CamelImpulse,
    ) -> CamelDspV1 {
        CamelDspV1 {
            struct_size: std::mem::size_of::<CamelDspV1>() as u32,
            abi_version: ABI_VERSION,
            order: dsp::DEFAULT_ORDER,
            band_count: bands.len() as u32,
            bands: bands.as_ptr(),
            crossfeed: crossfeed.map_or(ptr::null(), |c| c as *const _),
            impulse,
            volume_db: 0.0,
        }
    }

    fn impulse(samples: &[f64], channels: u32, rate_hz: u32) -> *mut CamelImpulse {
        let ir = CamelImpulseResponseV1 {
            struct_size: std::mem::size_of::<CamelImpulseResponseV1>() as u32,
            abi_version: ABI_VERSION,
            channels,
            rate_hz,
            frames: samples.len() as u64 / u64::from(channels),
            samples: samples.as_ptr(),
        };
        let mut out = ptr::null_mut();
        assert_eq!(unsafe { camel_impulse_create_v1(&ir, &mut out) }, ENGINE_OK);
        out
    }

    // The mock track played through a 24-bit 48 kHz device with `setting`, `frames` frames as 32-bit containers.
    fn play_with_dsp(setting: &CamelDspV1, frames: usize) -> Vec<i32> {
        play_with(setting, 0.0, frames)
    }

    fn play_with(setting: &CamelDspV1, gain_db: f64, frames: usize) -> Vec<i32> {
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S24_IN_32_LE, 24, 32));
        cfg.format.rate_hz = 48_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_set_dsp_v1(engine, setting) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source_with_gain(engine, gain_db), ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        prefill_until_end(engine);
        let mut output = vec![0u8; frames * 8];
        let request = render_request(frames as u64, &mut output);
        let mut result = render_result();
        unsafe { camel_engine_render_v1(engine, &request, &mut result) };
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
        output[..result.units_rendered as usize * 8]
            .as_chunks::<4>()
            .0
            .iter()
            .map(|b| i32::from_le_bytes(*b) >> 8)
            .collect()
    }

    fn play_with_eq(bands: &[CamelEqBandV1]) -> Vec<i32> {
        play_with_dsp(&dsp_setting(bands, None, ptr::null()), 4)
    }

    #[test]
    fn an_eq_that_is_off_or_flat_keeps_playback_bit_perfect() {
        // The 16-bit mock samples zero-padded into 24 bits, untouched.
        let exact = vec![256, -256, 512, -512, 768, -768, 1024, -1024];
        assert_eq!(play_with_eq(&[]), exact);
        assert_eq!(
            play_with_eq(&[
                eq_band(eq::KIND_PEAKING, 1000.0, 0.0, true),
                eq_band(eq::KIND_LOW_SHELF, 100.0, 9.0, false),
                eq_band(eq::KIND_HIGH_SHELF, 30_000.0, 6.0, true), // above Nyquist at 48 kHz
            ]),
            exact
        );
    }

    #[test]
    fn a_track_gain_and_the_volume_scale_the_samples_and_0_db_stays_bit_perfect() {
        let exact = vec![256, -256, 512, -512, 768, -768, 1024, -1024];
        let flat = dsp_setting(&[], None, ptr::null());
        assert_eq!(play_with(&flat, 0.0, 4), exact);
        // Half the level: -6.02 dB, within a step of the 24-bit dither.
        let half = -20.0 * 2f64.log10();
        let close = |got: Vec<i32>, want: Vec<i32>| {
            assert!(
                got.iter().zip(&want).all(|(g, w)| (g - w).abs() <= 1),
                "{got:?} vs {want:?}"
            );
        };
        let halved: Vec<i32> = exact.iter().map(|v| v / 2).collect();
        close(play_with(&flat, half, 4), halved.clone());
        let quiet = CamelDspV1 {
            volume_db: half,
            ..flat
        };
        close(play_with(&quiet, 0.0, 4), halved);
        let muted = CamelDspV1 {
            volume_db: f64::NEG_INFINITY,
            ..flat
        };
        close(play_with(&muted, 0.0, 4), vec![0; 8]);
        // A loud volume and an unusable gain are refused.
        let loud = CamelDspV1 {
            volume_db: 3.0,
            ..flat
        };
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S24_IN_32_LE, 24, 32));
        cfg.format.rate_hz = 48_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_set_dsp_v1(engine, &loud) },
            ENGINE_E_INVALID_ARGUMENT
        );
        assert_eq!(
            load_mock_source_with_gain(engine, f64::NAN),
            ENGINE_E_INVALID_ARGUMENT
        );
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn an_acting_eq_processes_at_the_device_depth() {
        let processed = play_with_eq(&[eq_band(eq::KIND_HIGH_SHELF, 1000.0, -6.0, true)]);
        assert_ne!(
            processed,
            vec![256, -256, 512, -512, 768, -768, 1024, -1024]
        );
        // Quantized once at 24 bits, not at the source's 16.
        assert!(processed.iter().any(|s| s % 256 != 0), "{processed:?}");
    }

    #[test]
    fn the_analysis_gives_the_bands_curve_and_the_chains_preamp() {
        let mut summary = CamelDspSummaryV1 {
            struct_size: std::mem::size_of::<CamelDspSummaryV1>() as u32,
            abi_version: ABI_VERSION,
            active: 0,
            reserved: 0,
            preamp_db: 1.0,
        };
        let band = [eq_band(eq::KIND_PEAKING, 1000.0, 6.0, true)];
        let frequencies = [1000.0, 20.0];
        let (mut left, mut right) = ([0.0; 2], [0.0; 2]);
        let analyze = |setting: &CamelDspV1,
                       summary: &mut CamelDspSummaryV1,
                       left: &mut [f64; 2],
                       right: &mut [f64; 2]| unsafe {
            camel_dsp_analyze_v1(
                setting,
                48_000,
                frequencies.as_ptr(),
                2,
                left.as_mut_ptr(),
                right.as_mut_ptr(),
                summary,
            )
        };
        let setting = dsp_setting(&band, None, ptr::null());
        assert_eq!(
            analyze(&setting, &mut summary, &mut left, &mut right),
            ENGINE_OK
        );
        assert_eq!(summary.active, 1);
        assert!((summary.preamp_db + 6.0).abs() < 1e-6);
        assert!((left[0] - 6.0).abs() < 1e-6 && right[1].abs() < 0.01);

        // A response that doubles the level adds 6 dB of headroom to the chain's, not to the curve.
        let ir = impulse(&[2.0], 1, 48_000);
        let setting = dsp_setting(&band, None, ir);
        assert_eq!(
            analyze(&setting, &mut summary, &mut left, &mut right),
            ENGINE_OK
        );
        // With a response the headroom is searched on its frequency grid, which passes close to the band's centre.
        assert!(
            (summary.preamp_db + 6.0 + 20.0 * 2f64.log10()).abs() < 1e-3,
            "{}",
            summary.preamp_db
        );
        assert!((left[0] - 6.0).abs() < 1e-6);
        // At another rate the response does not apply.
        let other = unsafe {
            camel_dsp_analyze_v1(
                &setting,
                44_100,
                ptr::null(),
                0,
                ptr::null_mut(),
                ptr::null_mut(),
                &mut summary,
            )
        };
        assert_eq!(other, ENGINE_E_INVALID_ARGUMENT);
        assert_eq!(unsafe { camel_impulse_destroy_v1(ir) }, ENGINE_OK);
    }

    #[test]
    fn crossfeed_and_convolution_process_and_the_tail_plays_after_the_last_track() {
        let crossfeed = CamelCrossfeedV1 {
            struct_size: std::mem::size_of::<CamelCrossfeedV1>() as u32,
            abi_version: ABI_VERSION,
            cutoff_hz: 700,
            reserved: 0,
            feed_db: 4.5,
        };
        let crossfed = play_with_dsp(&dsp_setting(&[], Some(&crossfeed), ptr::null()), 4);
        assert_ne!(crossfed, vec![256, -256, 512, -512, 768, -768, 1024, -1024]);

        // A half-level echo one frame later: the track's 4 frames plus 1 frame of tail.
        let ir = impulse(&[0.5, 0.25], 1, 48_000);
        let convolved = play_with_dsp(&dsp_setting(&[], None, ir), 8);
        assert_eq!(convolved.len(), 10, "{convolved:?}");
        let expect = [128, -128, 320, -320, 512, -512, 704, -704, 256, -256];
        for (got, want) in convolved.iter().zip(expect) {
            assert!((got - want).abs() <= 1, "{convolved:?}");
        }
        assert_eq!(unsafe { camel_impulse_destroy_v1(ir) }, ENGINE_OK);

        // A response at another rate, a repeated node and a crossfeed out of range are refused.
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S24_IN_32_LE, 24, 32));
        cfg.format.rate_hz = 48_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let other = impulse(&[1.0], 1, 44_100);
        assert_eq!(
            unsafe { camel_engine_set_dsp_v1(engine, &dsp_setting(&[], None, other)) },
            ENGINE_E_INVALID_ARGUMENT
        );
        let mut repeated = dsp_setting(&[], None, ptr::null());
        repeated.order = [dsp::NODE_EQ, dsp::NODE_EQ, dsp::NODE_CROSSFEED];
        assert_eq!(
            unsafe { camel_engine_set_dsp_v1(engine, &repeated) },
            ENGINE_E_INVALID_ARGUMENT
        );
        let wide = CamelCrossfeedV1 {
            cutoff_hz: 5_000,
            ..crossfeed
        };
        assert_eq!(
            unsafe { camel_engine_set_dsp_v1(engine, &dsp_setting(&[], Some(&wide), ptr::null())) },
            ENGINE_E_INVALID_ARGUMENT
        );
        assert_eq!(unsafe { camel_impulse_destroy_v1(other) }, ENGINE_OK);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn queued_track_follows_without_a_gap_and_restarts_track_position() {
        let engine = mock_engine_at_48k();
        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);
        assert_eq!(queue_mock_next(engine, 3), ENGINE_OK);
        assert_eq!(prefill_until_end(engine), 8);

        let mut output = [0u8; 24];
        let request = render_request(6, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.units_rendered, 6);
        let samples: Vec<i16> = output
            .as_chunks::<2>()
            .0
            .iter()
            .map(|b| i16::from_le_bytes(*b))
            .collect();
        assert_eq!(samples, [1, -1, 2, -2, 3, -3, 4, -4, 1, -1, 2, -2]);
        assert_eq!(transitions(engine), (1, 2, 0));

        let mut tail = [0u8; 16];
        let request = render_request(4, &mut tail);
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_END
        );
        assert_eq!(result.units_rendered, 2);
        assert_eq!(transitions(engine), (1, 4, 0));
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    struct ToneDecoder {
        level: i32,
        frames: u64,
        position: u64,
    }

    // A track named "level:frames:rate": `frames` stereo 16-bit frames of the constant `level` at `rate` Hz; with
    // ":dsd" after it, DSD byte frames of the byte `level` at the byte rate `rate`.
    unsafe extern "C" fn tone_open(
        _: *mut c_void,
        _: u32,
        path: *const u8,
        length: u64,
        out_decoder: *mut *mut c_void,
        info: *mut CamelSourceInfoV1,
    ) -> i32 {
        let name = std::str::from_utf8(std::slice::from_raw_parts(path, length as usize)).unwrap();
        let dsd = name.ends_with(":dsd");
        let fields: Vec<i64> = name
            .trim_end_matches(":dsd")
            .split(':')
            .map(|f| f.parse().unwrap())
            .collect();
        let decoder = Box::new(ToneDecoder {
            level: fields[0] as i32,
            frames: fields[1] as u64,
            position: 0,
        });
        *info = CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: ABI_VERSION,
            sample_rate: fields[2] as u32,
            channel_count: 2,
            valid_bits: if dsd { 1 } else { 16 },
            sample_encoding: if dsd { SAMPLE_ENCODING_DSD } else { 0 },
            total_frames: decoder.frames,
            has_md5: 0,
            reserved2: 0,
            md5: [0; 16],
        };
        *out_decoder = Box::into_raw(decoder).cast();
        0
    }

    unsafe extern "C" fn tone_read(
        _: *mut c_void,
        opaque: *mut c_void,
        output: *mut i32,
        frames: u64,
        frames_read: *mut u64,
    ) -> i32 {
        let decoder = &mut *opaque.cast::<ToneDecoder>();
        let read = (decoder.frames - decoder.position).min(frames);
        std::slice::from_raw_parts_mut(output, read as usize * 2).fill(decoder.level);
        decoder.position += read;
        *frames_read = read;
        0
    }

    unsafe extern "C" fn tone_seek(_: *mut c_void, opaque: *mut c_void, frame: u64) -> i32 {
        (&mut *opaque.cast::<ToneDecoder>()).position = frame;
        0
    }

    unsafe extern "C" fn tone_close(_: *mut c_void, opaque: *mut c_void) {
        drop(Box::from_raw(opaque.cast::<ToneDecoder>()));
    }

    fn tone_host() -> CamelDecoderHostV1 {
        CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: std::ptr::null_mut(),
            open: Some(tone_open),
            read: Some(tone_read),
            seek: Some(tone_seek),
            close: Some(tone_close),
            length: None,
        }
    }

    fn tone_source(name: &str, crossfade_ms: u32, flags: u32) -> CamelTrackSourceV1 {
        CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: name.as_ptr(),
            path_length_bytes: name.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
            gain_db: 0.0,
            crossfade_ms,
            flags,
        }
    }

    // Plays the tone `first` on a 16-bit 48 kHz device 480 frames at a time until the stream ends. `step` runs before
    // each render with the frames rendered so far, and may queue or change tracks. Returns the left channel.
    fn play_tones(first: &str, mut step: impl FnMut(*mut CamelEngine, usize)) -> Vec<i16> {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let host = tone_host();
        let source = tone_source(first, 0, 0);
        assert_eq!(
            unsafe {
                camel_engine_load_flac_v1(
                    engine,
                    &command_stamp(1),
                    &source,
                    decoder_host::DECODER_SYMPHONIA,
                    &host,
                )
            },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        let mut left = Vec::new();
        for _ in 0..10_000 {
            step(engine, left.len());
            loop {
                let mut frames = 0;
                let status = unsafe { camel_engine_prefill_v1(engine, 4096, &mut frames) };
                if status != ENGINE_OK || frames == 0 {
                    break;
                }
            }
            let mut processed = 0;
            unsafe { camel_engine_process_v1(engine, &mut processed) };
            let mut output = [0u8; 480 * 4];
            let request = render_request(480, &mut output);
            let mut result = render_result();
            let status = unsafe { camel_engine_render_v1(engine, &request, &mut result) };
            left.extend(
                output[..result.units_rendered as usize * 4]
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .map(|b| i16::from_le_bytes([b[0], b[1]])),
            );
            if status == ENGINE_END {
                assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
                return left;
            }
        }
        panic!("the stream did not end");
    }

    fn queue_tone(
        engine: *mut CamelEngine,
        command: u64,
        name: &str,
        crossfade_ms: u32,
        flags: u32,
    ) -> i32 {
        let host = tone_host();
        let source = tone_source(name, crossfade_ms, flags);
        unsafe {
            camel_engine_queue_next_flac_v1(
                engine,
                &command_stamp(command),
                &source,
                decoder_host::DECODER_SYMPHONIA,
                &host,
            )
        }
    }

    // The equal-power mix `fade_frame` frames into a fade of `length` frames from `from` to `to`.
    fn faded(from: f64, to: f64, fade_frame: usize, length: usize) -> f64 {
        let angle = (fade_frame as f64 + 0.5) / length as f64 * std::f64::consts::FRAC_PI_2;
        from * angle.cos() + to * angle.sin()
    }

    #[test]
    fn a_crossfade_mixes_the_end_of_a_track_into_the_next_with_equal_power() {
        let mut transitions_at_end = (0, 0, 0);
        let left = play_tones("8000:48000:48000", |engine, rendered| {
            if rendered == 0 {
                assert_eq!(
                    queue_tone(engine, 3, "-4000:48000:48000", 100, 0),
                    ENGINE_OK
                );
            }
            transitions_at_end = transitions(engine);
        });
        // The fade takes the last 4,800 frames of the first track and the first 4,800 of the second.
        assert_eq!(left.len(), 48_000 + 48_000 - 4_800);
        assert!(
            left[..43_200].iter().all(|&v| v == 8000),
            "the first track stays bit-perfect"
        );
        for (i, &v) in left[43_200..48_000].iter().enumerate() {
            let expected = faded(8000.0, -4000.0, i, 4_800);
            assert!(
                (f64::from(v) - expected).abs() <= 2.0,
                "{i}: {v} vs {expected}"
            );
        }
        assert!(
            left[48_000..].iter().all(|&v| v == -4000),
            "the second track is bit-perfect after the fade"
        );
        // The second track became current when its fade began.
        assert_eq!(transitions_at_end.0, 1);
    }

    #[test]
    fn a_crossfade_takes_at_most_half_of_a_short_track() {
        let left = play_tones("8000:48000:48000", |engine, rendered| {
            if rendered == 0 {
                assert_eq!(queue_tone(engine, 3, "-4000:2000:48000", 100, 0), ENGINE_OK);
            }
        });
        assert_eq!(left.len(), 48_000 + 2_000 - 1_000);
        assert!(left[..47_000].iter().all(|&v| v == 8000));
        assert!(left[48_000..].iter().all(|&v| v == -4000));
    }

    #[test]
    fn no_crossfade_follows_gaplessly() {
        let left = play_tones("8000:4800:48000", |engine, rendered| {
            if rendered == 0 {
                assert_eq!(queue_tone(engine, 3, "-4000:4800:48000", 0, 0), ENGINE_OK);
            }
        });
        assert_eq!(left.len(), 9_600);
        assert!(left[..4_800].iter().all(|&v| v == 8000));
        assert!(left[4_800..].iter().all(|&v| v == -4000));
    }

    #[test]
    fn a_crossfade_may_convert_a_track_in_another_format_to_the_device_format() {
        let left = play_tones("8000:48000:48000", |engine, rendered| {
            if rendered == 0 {
                assert_eq!(
                    queue_tone(engine, 3, "-4000:44100:44100", 100, 0),
                    ENGINE_E_UNSUPPORTED_FORMAT
                );
                assert_eq!(
                    queue_tone(engine, 4, "-4000:44100:44100", 100, TRACK_CONVERT),
                    ENGINE_OK
                );
            }
        });
        // One second at 44.1 kHz is about 48,000 frames at the device rate.
        assert!(
            left.len().abs_diff(48_000 + 48_000 - 4_800) < 100,
            "{}",
            left.len()
        );
        assert!(left[..43_200].iter().all(|&v| v == 8000));
        let settled = &left[50_000..left.len() - 1_000];
        assert!(settled.iter().all(|&v| (i32::from(v) + 4000).abs() <= 2));
    }

    #[test]
    fn a_manual_change_crossfades_from_where_playback_is() {
        let mut changed = false;
        let mut boundary = None;
        let left = play_tones("8000:480000:48000", |engine, rendered| {
            let (count, position, _) = transitions(engine);
            if count == 1 && boundary.is_none() {
                boundary = Some(rendered - position as usize);
            }
            if rendered == 9_600 && !changed {
                changed = true;
                let host = tone_host();
                let source = tone_source("-4000:48000:48000", 100, 0);
                assert_eq!(
                    unsafe {
                        camel_engine_crossfade_to_v1(
                            engine,
                            &command_stamp(3),
                            &source,
                            decoder_host::DECODER_SYMPHONIA,
                            &host,
                        )
                    },
                    ENGINE_OK
                );
            }
        });
        // The new track became current at the fade, after the quarter second the device ring still held.
        let start = boundary.unwrap();
        assert!(start > 9_600 && start <= 9_600 + 12_480, "{start}");
        for (i, &v) in left[start..start + 4_800].iter().enumerate() {
            let expected = faded(8000.0, -4000.0, i, 4_800);
            assert!(
                (f64::from(v) - expected).abs() <= 2.0,
                "{i}: {v} vs {expected}"
            );
        }
        assert!(left[start + 4_800..].iter().all(|&v| v == -4000));
        assert_eq!(left.len(), start + 48_000);
    }

    #[test]
    fn a_manual_change_during_a_crossfade_fades_out_what_it_would_play() {
        let mut changed = false;
        let mut boundaries = Vec::new();
        let left = play_tones("8000:192000:48000", |engine, rendered| {
            if rendered == 0 {
                assert_eq!(
                    queue_tone(engine, 3, "-4000:192000:48000", 1000, 0),
                    ENGINE_OK
                );
            }
            let (count, position, _) = transitions(engine);
            if count as usize > boundaries.len() {
                boundaries.push(rendered - position as usize);
            }
            // The second track's fade has begun and plays on when the third one is chosen.
            if count == 1 && !changed {
                changed = true;
                let host = tone_host();
                let source = tone_source("2000:96000:48000", 1000, 0);
                let status = unsafe {
                    camel_engine_crossfade_to_v1(
                        engine,
                        &command_stamp(4),
                        &source,
                        decoder_host::DECODER_SYMPHONIA,
                        &host,
                    )
                };
                assert_eq!(status, ENGINE_OK);
            }
        });
        // No cut anywhere: a step between neighbouring samples is a fade's and the dither's, never a jump.
        let largest = left
            .windows(2)
            .map(|w| (i32::from(w[1]) - i32::from(w[0])).abs())
            .max()
            .unwrap();
        assert!(largest < 16, "{largest}");
        assert_eq!(boundaries.len(), 2);
        let start = boundaries[1];
        assert_eq!(left.len(), start + 96_000);
        assert!(left[start + 48_000..].iter().all(|&v| v == 2000));
    }

    #[test]
    fn a_manual_change_needs_a_playing_track_to_fade_from() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let host = tone_host();
        let source = tone_source("-4000:48000:48000", 100, 0);
        let change = |engine, command| unsafe {
            camel_engine_crossfade_to_v1(
                engine,
                &command_stamp(command),
                &source,
                decoder_host::DECODER_SYMPHONIA,
                &host,
            )
        };
        assert_eq!(change(engine, 1), ENGINE_E_INVALID_STATE);
        let none = tone_source("-4000:48000:48000", 0, 0);
        assert_eq!(
            unsafe {
                camel_engine_crossfade_to_v1(
                    engine,
                    &command_stamp(2),
                    &none,
                    decoder_host::DECODER_SYMPHONIA,
                    &host,
                )
            },
            ENGINE_E_INVALID_ARGUMENT
        );
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    // Plays `first` through a device in `cfg` until the stream ends, after `setup` (queuing, DSP), running `step` with
    // the frames rendered before each render; returns the heap operations of the DSP stage and render meanwhile.
    fn stage_heap_playing(
        cfg: CamelEngineConfigV1,
        first: &CamelTrackSourceV1,
        setup: impl FnOnce(*mut CamelEngine),
        mut step: impl FnMut(*mut CamelEngine, u64),
    ) -> usize {
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let host = tone_host();
        assert_eq!(
            unsafe {
                camel_engine_load_flac_v1(
                    engine,
                    &command_stamp(1),
                    first,
                    decoder_host::DECODER_SYMPHONIA,
                    &host,
                )
            },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        setup(engine);
        let mut output = vec![0u8; 480 * 64];
        let mut rendered = 0;
        stage_heap::take();
        for _ in 0..20_000 {
            step(engine, rendered);
            loop {
                let mut frames = 0;
                let status = unsafe { camel_engine_prefill_v1(engine, 4096, &mut frames) };
                if status != ENGINE_OK || frames == 0 {
                    break;
                }
            }
            let mut processed = 0;
            unsafe { camel_engine_process_v1(engine, &mut processed) };
            let request = render_request(480, &mut output);
            let mut result = render_result();
            let status = unsafe { camel_engine_render_v1(engine, &request, &mut result) };
            rendered += result.units_rendered;
            if status == ENGINE_END {
                let heap = stage_heap::take();
                assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
                return heap;
            }
        }
        panic!("the stream did not end");
    }

    #[test]
    fn the_dsp_stage_and_render_use_no_heap() {
        let device = |sample_format, valid_bits, container_bits| {
            let mut cfg = config(format(AUDIO_PCM, sample_format, valid_bits, container_bits));
            cfg.allow_conversion = 1;
            cfg
        };
        let pcm16 = device(SAMPLE_S16_LE, 16, 16);
        let pcm24 = device(SAMPLE_S24_IN_32_LE, 24, 32);
        let float = device(SAMPLE_F32_LE, 32, 32);
        let tone = |name: &'static str, gain_db: f64| {
            let mut source = tone_source(name, 0, 0);
            source.gain_db = gain_db;
            source
        };
        let none = |_: *mut CamelEngine| {};
        let mut heap = Vec::new();
        heap.push((
            "bypass",
            stage_heap_playing(pcm16, &tone("8000:96000:48000", 0.0), none, |_, _| {}),
        ));
        heap.push((
            "track gain",
            stage_heap_playing(pcm24, &tone("8000:96000:48000", -6.0), none, |_, _| {}),
        ));
        heap.push((
            "float device",
            stage_heap_playing(float, &tone("8000:96000:48000", 0.0), none, |_, _| {}),
        ));
        heap.push((
            "resampled",
            stage_heap_playing(pcm24, &tone("8000:88200:44100", 0.0), none, |_, _| {}),
        ));
        let bands = [eq_band(eq::KIND_PEAKING, 1000.0, 6.0, true)];
        let crossfeed = CamelCrossfeedV1 {
            struct_size: std::mem::size_of::<CamelCrossfeedV1>() as u32,
            abi_version: ABI_VERSION,
            cutoff_hz: 700,
            reserved: 0,
            feed_db: 4.5,
        };
        let response: Vec<f64> = (0..3000).map(|i| 0.5f64.powi(i / 100)).collect();
        let ir = impulse(&response, 1, 48_000);
        let chain = dsp_setting(&bands, Some(&crossfeed), ir);
        heap.push((
            "EQ, crossfeed and convolution",
            stage_heap_playing(
                pcm24,
                &tone("8000:96000:48000", 0.0),
                |engine| {
                    assert_eq!(
                        unsafe { camel_engine_set_dsp_v1(engine, &chain) },
                        ENGINE_OK
                    )
                },
                |_, _| {},
            ),
        ));
        unsafe { camel_impulse_destroy_v1(ir) };
        heap.push((
            "gapless and crossfaded tracks",
            stage_heap_playing(
                pcm24,
                &tone("8000:96000:48000", 0.0),
                |engine| assert_eq!(queue_tone(engine, 3, "-4000:96000:48000", 0, 0), ENGINE_OK),
                |engine, rendered| {
                    if rendered == 96_000 {
                        assert_eq!(
                            queue_tone(engine, 4, "2000:96000:44100", 500, TRACK_CONVERT),
                            ENGINE_OK
                        );
                    }
                },
            ),
        ));
        let mut changes = 0;
        heap.push((
            "manual changes, one during a crossfade",
            stage_heap_playing(
                pcm24,
                &tone("8000:192000:48000", 0.0),
                none,
                |engine, rendered| {
                    // The second change comes once the first one's fade is heard, while it plays.
                    if rendered == 48_000 && changes == 0
                        || changes == 1 && transitions(engine).0 == 1
                    {
                        changes += 1;
                        let host = tone_host();
                        let source = tone_source(
                            if changes == 1 {
                                "-4000:96000:48000"
                            } else {
                                "2000:96000:48000"
                            },
                            1000,
                            0,
                        );
                        let status = unsafe {
                            camel_engine_crossfade_to_v1(
                                engine,
                                &command_stamp(10 + changes),
                                &source,
                                decoder_host::DECODER_SYMPHONIA,
                                &host,
                            )
                        };
                        assert_eq!(status, ENGINE_OK);
                    }
                },
            ),
        ));
        let used: Vec<_> = heap.iter().filter(|(_, count)| *count > 0).collect();
        assert!(used.is_empty(), "{used:?}");
    }

    #[test]
    fn raw_dsd_and_dsd_to_pcm_use_no_heap_in_the_dsp_stage() {
        let mut native = config(format(AUDIO_DSD, SAMPLE_DSD_U8, 1, 8));
        native.format.rate_hz = 2_822_400;
        let dop = config(dop_format(SAMPLE_DOP_S24_IN_32_LE, 32, 176_400));
        let mut pcm = config(format(AUDIO_PCM, SAMPLE_S24_IN_32_LE, 24, 32));
        pcm.format.rate_hz = 88_200;
        pcm.allow_dsd_to_pcm = 1;
        let one_second = tone_source("105:352800:352800:dsd", 0, 0);
        let none = |_: *mut CamelEngine| {};
        let mut heap: Vec<_> = [("native DSD", native), ("DoP", dop), ("DSD to PCM", pcm)]
            .into_iter()
            .map(|(case, cfg)| (case, stage_heap_playing(cfg, &one_second, none, |_, _| {})))
            .filter(|(_, count)| *count > 0)
            .collect();
        let dsd128 = tone_source("105:705600:705600:dsd", 0, 0);
        for (case, mut cfg) in [
            ("DSD128 to native DSD64", native),
            ("DSD128 to DoP DSD64", dop),
        ] {
            cfg.allow_dsd_to_pcm = 1;
            let count = stage_heap_playing(cfg, &dsd128, none, |_, _| {});
            if count > 0 {
                heap.push((case, count));
            }
        }
        assert!(heap.is_empty(), "{heap:?}");
    }

    fn integrity(engine: *mut CamelEngine) -> CamelFlacIntegrityV1 {
        let mut integrity = CamelFlacIntegrityV1 {
            struct_size: std::mem::size_of::<CamelFlacIntegrityV1>() as u32,
            abi_version: ABI_VERSION,
            status: u32::MAX,
            has_reference_md5: u32::MAX,
            total_frames: u64::MAX,
            position_frames: u64::MAX,
        };
        assert_eq!(
            unsafe { camel_engine_get_flac_integrity_v1(engine, &mut integrity) },
            ENGINE_OK
        );
        integrity
    }

    #[test]
    fn integrity_follows_the_audible_track_across_a_decoded_boundary() {
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        cfg.format.rate_hz = 48_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let mut metadata = MockMetadata {
            total_frames: 0,
            has_md5: 1,
            md5: [0xA5; 16],
        };
        let host = mock_host_with_metadata(&mut metadata);
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        assert_eq!(
            unsafe {
                camel_engine_load_flac_v1(
                    engine,
                    &command_stamp(1),
                    &source,
                    decoder_host::DECODER_SYMPHONIA,
                    &host,
                )
            },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        assert_eq!(queue_mock_next(engine, 3), ENGINE_OK);
        assert_eq!(prefill_until_end(engine), 8);

        let heard = integrity(engine);
        assert_eq!(heard.status, FLAC_INTEGRITY_VERIFIED);
        assert_eq!(heard.has_reference_md5, 1);

        let mut output = [0u8; 24];
        let request = render_request(6, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(transitions(engine).0, 1);
        let next = integrity(engine);
        assert_eq!(next.status, FLAC_INTEGRITY_UNAVAILABLE);
        assert_eq!(next.has_reference_md5, 0);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn track_queued_after_the_end_was_decoded_continues_the_stream() {
        let engine = mock_engine_at_48k();
        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);
        assert_eq!(prefill_until_end(engine), 4);
        assert_eq!(queue_mock_next(engine, 3), ENGINE_OK);
        assert_eq!(prefill_until_end(engine), 4);

        let mut output = [0u8; 32];
        let request = render_request(8, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.units_rendered, 8);
        assert_eq!(transitions(engine), (1, 4, 0));
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn queued_track_can_be_cleared_until_decoding_reaches_it() {
        let engine = mock_engine_at_48k();
        assert_eq!(queue_mock_next(engine, 2), ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_clear_next_v1(engine, &command_stamp(3)) },
            ENGINE_OK
        );
        assert_eq!(prefill_until_end(engine), 4);
        assert_eq!(transitions(engine).0, 0);

        let engine2 = mock_engine_at_48k();
        assert_eq!(queue_mock_next(engine2, 2), ENGINE_OK);
        assert_eq!(transitions(engine2).2, 1);
        let mut frames = 0;
        assert_eq!(
            unsafe { camel_engine_prefill_v1(engine2, 4, &mut frames) },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_prefill_v1(engine2, 4, &mut frames) },
            ENGINE_OK
        );
        assert_eq!(frames, 0); // swapped to the queued track
        assert_eq!(transitions(engine2).2, 0);
        assert_eq!(
            unsafe { camel_engine_clear_next_v1(engine2, &command_stamp(3)) },
            ENGINE_E_INVALID_STATE
        );
        assert_eq!(
            unsafe { camel_engine_seek_v1(engine2, &command_stamp(4), 1) },
            ENGINE_E_INVALID_STATE
        );
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine2) }, ENGINE_OK);
    }

    #[test]
    fn another_rate_needs_conversion_permission() {
        let mut cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        cfg.format.rate_hz = 96_000;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(queue_mock_next(engine, 1), ENGINE_E_INVALID_STATE);
        // The 48 kHz mock does not match this 96 kHz output, and nothing converts it silently.
        assert_eq!(load_mock_source(engine), ENGINE_E_UNSUPPORTED_FORMAT);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);

        cfg.allow_conversion = 1;
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);
        let first = prefill_until_end(engine);
        // Queued after the first track's end was decoded: the stream continues with it.
        assert_eq!(queue_mock_next(engine, 2), ENGINE_OK);
        let second = prefill_until_end(engine);
        assert_eq!((first, second), (8, 8)); // four 48 kHz frames each, at 96 kHz
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
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
        assert_eq!(std::mem::size_of::<CamelEngineConfigV1>(), 72);
        assert_eq!(std::mem::size_of::<CamelCommandStampV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelTrackSourceV1>(), 64);
        assert_eq!(std::mem::size_of::<CamelSourceInfoV1>(), 56);
        assert_eq!(std::mem::size_of::<CamelDecoderHostV1>(), 56);
        assert_eq!(std::mem::size_of::<CamelRenderRequestV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelRenderResultV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelEngineStateV1>(), 152);
        assert_eq!(std::mem::size_of::<CamelFlacIntegrityV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelEngineTimelineV1>(), 88);
        assert_eq!(std::mem::size_of::<CamelEqBandV1>(), 48);
        assert_eq!(std::mem::size_of::<CamelDspSummaryV1>(), 24);
        assert_eq!(std::mem::size_of::<CamelCrossfeedV1>(), 24);
        assert_eq!(std::mem::size_of::<CamelImpulseResponseV1>(), 32);
        assert_eq!(std::mem::size_of::<CamelDspV1>(), 56);
        assert_eq!(std::mem::offset_of!(CamelDspV1, bands), 24);
        assert_eq!(std::mem::offset_of!(CamelEqBandV1, frequency_hz), 24);
        assert_eq!(std::mem::offset_of!(CamelAudioFormatV1, channel_mask), 32);
        assert_eq!(std::mem::offset_of!(CamelEngineConfigV1, format), 16);
        assert_eq!(std::mem::offset_of!(CamelCommandStampV1, command_id), 8);
        assert_eq!(std::mem::offset_of!(CamelTrackSourceV1, start_frame), 24);
        assert_eq!(std::mem::offset_of!(CamelTrackSourceV1, crossfade_ms), 56);
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
        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);

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

    // The mock's four frames, told once a read has passed `LATE_LENGTH_AFTER` frames, as mpg123 does when its scan of an
    // MP3 ends; `LATE_LENGTH_TOLD` is what it tells.
    static LATE_LENGTH_AFTER: AtomicU64 = AtomicU64::new(0);
    static LATE_LENGTH_TOLD: AtomicU64 = AtomicU64::new(0);

    unsafe extern "C" fn late_length(
        _: *mut c_void,
        opaque: *mut c_void,
        total_frames: *mut u64,
    ) -> i32 {
        let decoder = &*opaque.cast::<MockDecoder>();
        *total_frames = if decoder.position as u64 > LATE_LENGTH_AFTER.load(Ordering::Relaxed) {
            LATE_LENGTH_TOLD.load(Ordering::Relaxed)
        } else {
            0
        };
        0
    }

    fn total_frames(engine: *mut CamelEngine) -> u64 {
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
        timeline.total_frames
    }

    // Opened without its length, the source learns it while it plays and still ends cleanly; a length short of what
    // was already read is not believed.
    #[test]
    fn a_length_learned_while_playing_becomes_the_sources() {
        for (told, expected) in [(4, 4), (1, 0)] {
            LATE_LENGTH_AFTER.store(1, Ordering::Relaxed);
            LATE_LENGTH_TOLD.store(told, Ordering::Relaxed);
            let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
            let mut engine = ptr::null_mut();
            assert_eq!(
                unsafe { camel_engine_create_v1(&cfg, &mut engine) },
                ENGINE_OK
            );
            let mut metadata = MockMetadata {
                total_frames: 0,
                has_md5: 0,
                md5: [0; 16],
            };
            let mut host = mock_host_with_metadata(&mut metadata);
            host.length = Some(late_length);
            assert_eq!(load_source_with_host(engine, &host, 0.0), ENGINE_OK);
            assert_eq!(total_frames(engine), 0);

            let play = command_stamp(2);
            assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);
            let mut frames_prefilled = 0;
            assert_eq!(
                unsafe { camel_engine_prefill_v1(engine, 2, &mut frames_prefilled) },
                ENGINE_OK
            );
            assert_eq!(total_frames(engine), expected, "told {told}");
            let mut status = ENGINE_OK;
            for _ in 0..8 {
                status = unsafe { camel_engine_prefill_v1(engine, 4, &mut frames_prefilled) };
                if status != ENGINE_OK {
                    break;
                }
            }
            assert_eq!(status, ENGINE_END, "told {told}");
            assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
        }
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
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
                    decoder_host::DECODER_SYMPHONIA,
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
    fn play_and_pause_keep_a_prefill_in_progress() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        let path = b"blocking.flac";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
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
                    &command_stamp(1),
                    &source,
                    decoder_host::DECODER_SYMPHONIA,
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
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        assert_eq!(
            unsafe { camel_engine_pause_v1(engine, &command_stamp(3)) },
            ENGINE_OK
        );
        context.release_read.store(true, Ordering::Release);

        assert_eq!(prefill.join().unwrap(), (ENGINE_OK, 4));
        assert_eq!(
            unsafe { &*engine }
                .source
                .lock()
                .unwrap()
                .as_ref()
                .unwrap()
                .position(),
            4
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
                gain_db: 0.0,
                crossfade_ms: 0,
                flags: 0,
            };
            let host =
                unsafe { blocking_open_host(&mut *(context_address as *mut BlockingOpenContext)) };
            unsafe {
                camel_engine_load_flac_v1(
                    engine_address as *mut CamelEngine,
                    &command,
                    &source,
                    decoder_host::DECODER_SYMPHONIA,
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
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
                    decoder_host::DECODER_SYMPHONIA,
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
        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);

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

    fn timeline(engine: *mut CamelEngine) -> CamelEngineTimelineV1 {
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
        timeline
    }

    #[test]
    fn render_outside_playback_returns_silence_without_consuming() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
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

        let mut output = [0xA5; 8];
        let request = render_request(2, &mut output);
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.state, RENDER_STATE_PAUSED);
        assert_eq!(result.units_rendered, 0);
        assert_eq!(output, [0; 8]);

        let play = command_stamp(2);
        assert_eq!(unsafe { camel_engine_play_v1(engine, &play) }, ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.units_rendered, 2);
        let pause = command_stamp(3);
        assert_eq!(unsafe { camel_engine_pause_v1(engine, &pause) }, ENGINE_OK);
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_OK
        );
        assert_eq!(result.state, RENDER_STATE_PAUSED);
        assert_eq!(timeline(engine).submitted_position_frames, 2);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn queries_do_not_wait_for_decoder_io() {
        let cfg = config(format(AUDIO_PCM, SAMPLE_S16_LE, 16, 16));
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&cfg, &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_OK);
        let source = unsafe { &*engine }.source.lock().unwrap();
        let current = timeline(engine);
        assert_eq!(current.has_source, 1);
        assert_eq!(current.total_frames, 4);
        drop(source);
        assert_eq!(unsafe { camel_engine_destroy_v1(engine) }, ENGINE_OK);
    }

    #[test]
    fn decoded_ring_holds_the_prebuffer_within_the_memory_limit() {
        let pcm = AudioFormat::from_abi(&format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24)).unwrap();
        // Two channels of 32-bit sample words per frame.
        assert_eq!(source_ring_capacity(&pcm, 96_000, 0), Some(96_000 * 10 * 8));
        assert_eq!(
            source_ring_capacity(&pcm, 96_000, 30_000),
            Some(96_000 * 30 * 8)
        );
        assert_eq!(source_ring_capacity(&pcm, 96_000, 1_000), Some(96_000 * 8));
        assert_eq!(
            source_ring_capacity(&pcm, 768_000 * 4, 0),
            Some(MAX_RING_BYTES as usize)
        );
        // Buffering ahead of a slow source: past 64 MiB, up to 1 GiB.
        assert_eq!(
            source_ring_capacity(&pcm, 176_400, 120_000),
            Some(176_400 * 120 * 8)
        );
        assert_eq!(
            source_ring_capacity(&pcm, 768_000 * 4, 600_000),
            Some(MAX_BUFFERING_RING_BYTES as usize)
        );
        assert_eq!(
            source_ring_capacity(&pcm, 1, 0),
            Some((MAX_PREFILL_FRAMES * MAX_SOURCE_FRAME_BYTES) as usize)
        );
    }

    #[test]
    fn device_ring_holds_the_dsp_lead_and_at_least_one_long_period() {
        let pcm = AudioFormat::from_abi(&format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24)).unwrap();
        // 250 ms of 6-byte frames at 96 kHz; a 44.1 kHz device still holds 8192 frames.
        assert_eq!(device_ring_capacity(&pcm, 96_000), Some(24_000 * 6));
        assert_eq!(
            device_ring_capacity(&pcm, 22_050),
            Some(MIN_DEVICE_RING_FRAMES as usize * 6)
        );
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        let host_abi = mock_host();
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_SYMPHONIA, host) }.unwrap();
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        let mut metadata = MockMetadata {
            total_frames: 0,
            has_md5: 1,
            md5: [0xA5; 16],
        };
        let host_abi = mock_host_with_metadata(&mut metadata);
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_SYMPHONIA, host) }.unwrap();
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        let mut metadata = MockMetadata {
            total_frames: 5,
            has_md5: 1,
            md5: [0xA5; 16],
        };
        let host_abi = mock_host_with_metadata(&mut metadata);
        let host = unsafe { DecoderHost::from_abi(&host_abi).unwrap() };
        let mut decoded =
            unsafe { DecoderSource::open(&source, decoder_host::DECODER_SYMPHONIA, host) }.unwrap();
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
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
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

    // A raw DSD source: interleaved byte frames at `rate` byte frames per second.
    struct DsdMock {
        bytes: Vec<u8>,
        channels: u32,
        rate: u32,
        position: usize,
    }

    unsafe extern "C" fn dsd_open(
        context: *mut c_void,
        _: u32,
        _: *const u8,
        _: u64,
        out_decoder: *mut *mut c_void,
        info: *mut CamelSourceInfoV1,
    ) -> i32 {
        let mock = &*context.cast::<DsdMock>();
        *out_decoder = Box::into_raw(Box::new(DsdMock {
            bytes: mock.bytes.clone(),
            channels: mock.channels,
            rate: mock.rate,
            position: 0,
        }))
        .cast();
        *info = CamelSourceInfoV1 {
            struct_size: std::mem::size_of::<CamelSourceInfoV1>() as u32,
            abi_version: ABI_VERSION,
            sample_rate: mock.rate,
            channel_count: mock.channels,
            valid_bits: 1,
            sample_encoding: SAMPLE_ENCODING_DSD,
            total_frames: (mock.bytes.len() / mock.channels as usize) as u64,
            has_md5: 0,
            reserved2: 0,
            md5: [0; 16],
        };
        0
    }

    unsafe extern "C" fn dsd_read(
        _: *mut c_void,
        opaque: *mut c_void,
        output: *mut i32,
        frames: u64,
        frames_read: *mut u64,
    ) -> i32 {
        let mock = &mut *opaque.cast::<DsdMock>();
        let channels = mock.channels as usize;
        let read = (mock.bytes.len() / channels - mock.position).min(frames as usize);
        for i in 0..read * channels {
            *output.add(i) = i32::from(mock.bytes[mock.position * channels + i]);
        }
        mock.position += read;
        *frames_read = read as u64;
        0
    }

    unsafe extern "C" fn dsd_seek(_: *mut c_void, opaque: *mut c_void, frame: u64) -> i32 {
        (*opaque.cast::<DsdMock>()).position = frame as usize;
        0
    }

    unsafe extern "C" fn dsd_close(_: *mut c_void, opaque: *mut c_void) {
        drop(Box::from_raw(opaque.cast::<DsdMock>()));
    }

    fn dsd_bytes(frames: usize, channels: usize) -> Vec<u8> {
        (0..frames * channels)
            .map(|i| (i * 37 + 11) as u8)
            .collect()
    }

    fn dsd_mock(frames: usize, channels: u32, rate: u32) -> DsdMock {
        DsdMock {
            bytes: dsd_bytes(frames, channels as usize),
            channels,
            rate,
            position: 0,
        }
    }

    // An engine for `output` playing `mock` from the start, fully prefilled.
    fn dsd_engine(output: CamelAudioFormatV1, mock: &mut DsdMock) -> Result<*mut CamelEngine, i32> {
        dsd_engine_from(config(output), mock)
    }

    fn dsd_engine_from(
        config: CamelEngineConfigV1,
        mock: &mut DsdMock,
    ) -> Result<*mut CamelEngine, i32> {
        let mut engine = ptr::null_mut();
        assert_eq!(
            unsafe { camel_engine_create_v1(&config, &mut engine) },
            ENGINE_OK
        );
        let host = CamelDecoderHostV1 {
            struct_size: std::mem::size_of::<CamelDecoderHostV1>() as u32,
            abi_version: ABI_VERSION,
            context: (mock as *mut DsdMock).cast(),
            open: Some(dsd_open),
            read: Some(dsd_read),
            seek: Some(dsd_seek),
            close: Some(dsd_close),
            length: None,
        };
        let path = b"mock.dsf";
        let source = CamelTrackSourceV1 {
            struct_size: std::mem::size_of::<CamelTrackSourceV1>() as u32,
            abi_version: ABI_VERSION,
            path_utf8: path.as_ptr(),
            path_length_bytes: path.len() as u64,
            start_frame: 0,
            end_frame_exclusive: 0,
            has_end_frame: 0,
            reserved: 0,
            gain_db: 0.0,
            crossfade_ms: 0,
            flags: 0,
        };
        let status = unsafe {
            camel_engine_load_flac_v1(
                engine,
                &command_stamp(1),
                &source,
                decoder_host::DECODER_DSD_NEXUS,
                &host,
            )
        };
        if status != ENGINE_OK {
            unsafe { camel_engine_destroy_v1(engine) };
            return Err(status);
        }
        prefill_until_end(engine);
        Ok(engine)
    }

    fn dop_format(sample_format: u32, container_bits: u32, rate_hz: u32) -> CamelAudioFormatV1 {
        let mut f = format(AUDIO_DOP, sample_format, 24, container_bits);
        f.rate_hz = rate_hz;
        f
    }

    #[test]
    fn dop_carries_raw_dsd_with_markers_continuing_through_pause_and_end() {
        for (sample_format, container_bits) in
            [(SAMPLE_DOP_S24_LE, 24), (SAMPLE_DOP_S24_IN_32_LE, 32)]
        {
            // 201 byte frames: the last DoP frame is completed with idle pattern.
            let mut mock = dsd_mock(201, 2, 352_800);
            let raw = mock.bytes.clone();
            let engine = dsd_engine(
                dop_format(sample_format, container_bits, 176_400),
                &mut mock,
            )
            .unwrap();
            let bytes_per_sample = container_bits as usize / 8;
            let mut out = Vec::new();
            let mut render = |units: u64, expect: i32| {
                let mut buf = vec![0xEE; units as usize * 2 * bytes_per_sample];
                let mut request = render_request(units, &mut buf);
                request.output_capacity_bytes = buf.len() as u64;
                let mut result = render_result();
                assert_eq!(
                    unsafe { camel_engine_render_v1(engine, &request, &mut result) },
                    expect
                );
                out.extend_from_slice(&buf);
                result.units_rendered
            };
            assert_eq!(render(7, ENGINE_OK), 0); // not playing yet: idle pattern
            assert_eq!(
                unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
                ENGINE_OK
            );
            assert_eq!(render(33, ENGINE_OK), 33);
            assert_eq!(render(100, ENGINE_END), 68); // 101 DoP frames in all, then silence
            assert_eq!(render(5, ENGINE_END), 0);

            let samples: Vec<&[u8]> = out.chunks_exact(bytes_per_sample).collect();
            let markers = [0x05, 0xFA];
            for (i, frame) in samples.as_chunks::<2>().0.iter().enumerate() {
                for sample in frame {
                    assert_eq!(sample[bytes_per_sample - 1], markers[i % 2], "frame {i}");
                    assert!(sample[..bytes_per_sample - 3].iter().all(|&b| b == 0));
                }
            }
            let payload = |frame: usize, channel: usize| {
                let s = samples[frame * 2 + channel];
                [s[bytes_per_sample - 2], s[bytes_per_sample - 3]]
            };
            let silent = |frame: usize| (0..2).all(|c| payload(frame, c) == [0x69, 0x69]);
            assert!((0..7).all(silent));
            for d in 0..101 {
                for c in 0..2 {
                    let first = raw[2 * d * 2 + c];
                    let second = raw.get((2 * d + 1) * 2 + c).copied().unwrap_or(0x69);
                    assert_eq!(payload(7 + d, c), [first, second], "DoP frame {d}");
                }
            }
            assert!((7 + 101..samples.len() / 2).all(silent));
            unsafe { camel_engine_destroy_v1(engine) };
        }
    }

    #[test]
    fn native_dsd_counts_dsd_samples_and_pads_with_the_idle_pattern() {
        let mut mock = dsd_mock(64, 1, 352_800); // mono is duplicated to both channels
        let raw = mock.bytes.clone();
        let mut output = format(AUDIO_DSD, SAMPLE_DSD_U8, 1, 8);
        output.rate_hz = 2_822_400;
        let engine = dsd_engine(output, &mut mock).unwrap();
        assert_eq!(
            unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
            ENGINE_OK
        );
        let mut buf = vec![0; 80 * 2];
        let mut request = render_request(80 * 8, &mut buf);
        request.output_capacity_bytes = buf.len() as u64;
        let mut result = render_result();
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut result) },
            ENGINE_END
        );
        assert_eq!(result.units_rendered, 64 * 8);
        let expected: Vec<u8> = raw.iter().flat_map(|&b| [b, b]).chain([0x69; 32]).collect();
        assert_eq!(buf, expected);

        let mut odd = vec![0; 2];
        let mut request = render_request(3, &mut odd);
        request.output_capacity_bytes = 2;
        assert_eq!(
            unsafe { camel_engine_render_v1(engine, &request, &mut render_result()) },
            ENGINE_E_INVALID_ARGUMENT
        );
        unsafe { camel_engine_destroy_v1(engine) };
    }

    #[test]
    fn raw_dsd_never_becomes_pcm_and_carrier_rates_must_match() {
        let mut engine = ptr::null_mut();
        let dop48 = dop_format(SAMPLE_DOP_S24_LE, 24, 48_000);
        assert_eq!(
            unsafe { camel_engine_create_v1(&config(dop48), &mut engine) },
            ENGINE_OK
        );
        assert_eq!(load_mock_source(engine), ENGINE_E_UNSUPPORTED_FORMAT);
        unsafe { camel_engine_destroy_v1(engine) };

        let mut pcm = format(AUDIO_PCM, SAMPLE_S24_LE, 24, 24);
        pcm.rate_hz = 352_800;
        assert_eq!(
            dsd_engine(pcm, &mut dsd_mock(16, 2, 352_800)).err(),
            Some(ENGINE_E_UNSUPPORTED_FORMAT)
        );
        // DSD512 (2822400 byte frames per second) needs a 1411.2 kHz DoP carrier, not 705.6 kHz.
        assert_eq!(
            dsd_engine(
                dop_format(SAMPLE_DOP_S24_LE, 24, 705_600),
                &mut dsd_mock(16, 2, 2_822_400)
            )
            .err(),
            Some(ENGINE_E_UNSUPPORTED_FORMAT)
        );
        let dsd512 = dsd_engine(
            dop_format(SAMPLE_DOP_S24_LE, 24, 1_411_200),
            &mut dsd_mock(16, 2, 2_822_400),
        )
        .unwrap();
        unsafe { camel_engine_destroy_v1(dsd512) };
        assert_eq!(
            dsd_engine(
                dop_format(SAMPLE_DOP_S24_LE, 24, 176_400),
                &mut dsd_mock(16, 6, 352_800)
            )
            .err(),
            Some(ENGINE_E_UNSUPPORTED_FORMAT)
        );
    }

    // DSD128 reaches a DSD64 stream, native or DoP, only with the DSD conversion permission: modulated again, half the
    // byte frames.
    #[test]
    fn raw_dsd_steps_down_to_a_lower_rate_only_with_permission() {
        let mut native = format(AUDIO_DSD, SAMPLE_DSD_U8, 1, 8);
        native.rate_hz = 2_822_400;
        let dop = dop_format(SAMPLE_DOP_S24_LE, 24, 176_400);
        for (output, units, bytes_per_unit) in [(native, 2048 * 8, 2), (dop, 1024, 6)] {
            assert_eq!(
                dsd_engine(output, &mut dsd_mock(4096, 2, 705_600)).err(),
                Some(ENGINE_E_UNSUPPORTED_FORMAT)
            );
            let mut allowed = config(output);
            allowed.allow_dsd_to_pcm = 1;
            let engine = dsd_engine_from(allowed, &mut dsd_mock(4096, 2, 705_600)).unwrap();
            assert_eq!(
                unsafe { camel_engine_play_v1(engine, &command_stamp(2)) },
                ENGINE_OK
            );
            let mut rendered = 0;
            loop {
                let mut buf = vec![0; 512 * bytes_per_unit];
                let mut request =
                    render_request(if bytes_per_unit == 2 { 512 * 8 } else { 512 }, &mut buf);
                request.output_capacity_bytes = buf.len() as u64;
                let mut result = render_result();
                let status = unsafe { camel_engine_render_v1(engine, &request, &mut result) };
                rendered += result.units_rendered;
                if status == ENGINE_END {
                    break;
                }
                assert_eq!(status, ENGINE_OK);
            }
            assert_eq!(rendered, units);
            unsafe { camel_engine_destroy_v1(engine) };
        }
    }
}
