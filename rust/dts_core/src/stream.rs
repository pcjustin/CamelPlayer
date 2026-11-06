// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Frames as a DTS-CD file carries them, in any of the four packings. The core decoder's integers saturate at full scale,
//! so overs are clipped there and the float output stays within -1 to 1; the owner's 141-minute album has 2 such samples.

use std::borrow::Cow;

use crate::{
    AmodeArrangement, CoreFrameDecodeError, CoreStreamDecoder, DtsFrameHeader, Error,
    FourteenBitByteOrder, SyncWordEncoding, decode_audio_coding_header_at, find_next_sync,
    parse_frame_header, parse_frame_header_14bit, unpack_14bit_to_16bit,
};

const FULL_SCALE: f32 = 2_147_483_648.0;
const HEADER_WINDOW_BYTES: usize = 64;

/// Where a channel plays.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Speaker {
    /// Front left.
    Left,
    /// Front right.
    Right,
    /// Front center.
    Center,
    /// Low-frequency effects.
    Lfe,
    /// Left surround.
    LeftSurround,
    /// Right surround.
    RightSurround,
    /// One surround channel behind the listener.
    BackCenter,
}

/// A frame's start in a byte stream.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Sync {
    /// Bytes from the start of the search's input.
    pub offset: usize,
    /// How the frame is packed.
    pub packing: SyncWordEncoding,
}

/// What a frame's header says.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FrameInfo {
    /// Bytes the frame takes in its packing.
    pub container_bytes: usize,
    /// In Hz.
    pub sample_rate: u32,
}

/// One frame's audio.
#[derive(Debug, Clone, PartialEq)]
pub struct Decoded {
    /// Interleaved, in the order of `speakers`.
    pub pcm: Vec<f32>,
    /// In the order L R C LFE Ls Rs, of those present.
    pub speakers: Vec<Speaker>,
    /// Samples per channel.
    pub samples: usize,
    /// In Hz.
    pub sample_rate: u32,
}

/// Why a frame gave no audio.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeError {
    /// The bytes do not start with a frame.
    NoSync,
    /// The bytes end before the frame does.
    Truncated,
    /// The header cannot be read.
    Header(Error),
    /// A channel arrangement beyond 5.1, or with more or fewer channels than it names.
    UnsupportedChannels,
    /// The header names no sample rate.
    UnsupportedSampleRate,
    /// The audio cannot be read.
    Frame(CoreFrameDecodeError),
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoSync => write!(f, "no DTS frame here"),
            Self::Truncated => write!(f, "the DTS frame is cut short"),
            Self::Header(e) => write!(f, "DTS frame header: {e}"),
            Self::UnsupportedChannels => write!(f, "a DTS channel arrangement beyond 5.1"),
            Self::UnsupportedSampleRate => write!(f, "a DTS frame without a sample rate"),
            Self::Frame(e) => write!(f, "DTS frame: {e}"),
        }
    }
}

impl std::error::Error for DecodeError {}

impl From<Error> for DecodeError {
    fn from(e: Error) -> Self {
        Self::Header(e)
    }
}

impl From<CoreFrameDecodeError> for DecodeError {
    fn from(e: CoreFrameDecodeError) -> Self {
        Self::Frame(e)
    }
}

/// The next frame start at or after `start`.
#[must_use]
pub fn find_sync(bytes: &[u8], start: usize) -> Option<Sync> {
    find_next_sync(bytes, start).map(|found| Sync {
        offset: found.offset,
        packing: found.encoding,
    })
}

fn order_of(packing: SyncWordEncoding) -> Option<FourteenBitByteOrder> {
    match packing {
        SyncWordEncoding::FourteenBitBigEndian => Some(FourteenBitByteOrder::BigEndian),
        SyncWordEncoding::FourteenBitLittleEndian => Some(FourteenBitByteOrder::LittleEndian),
        SyncWordEncoding::RawBigEndian | SyncWordEncoding::RawLittleEndian => None,
    }
}

fn packing_at_start(bytes: &[u8]) -> Result<SyncWordEncoding, DecodeError> {
    match find_sync(bytes, 0) {
        Some(found) if found.offset == 0 => Ok(found.packing),
        _ => Err(DecodeError::NoSync),
    }
}

fn header_of(bytes: &[u8], packing: SyncWordEncoding) -> Result<DtsFrameHeader, DecodeError> {
    let window = &bytes[..bytes.len().min(HEADER_WINDOW_BYTES) & !1];
    Ok(match packing {
        SyncWordEncoding::RawBigEndian => parse_frame_header(window)?,
        SyncWordEncoding::RawLittleEndian => parse_frame_header(&swapped(window))?,
        SyncWordEncoding::FourteenBitBigEndian | SyncWordEncoding::FourteenBitLittleEndian => {
            parse_frame_header_14bit(window)?
        }
    })
}

fn swapped(bytes: &[u8]) -> Vec<u8> {
    bytes
        .as_chunks::<2>()
        .0
        .iter()
        .flat_map(|word| [word[1], word[0]])
        .collect()
}

/// Reads the header of the frame `bytes` starts with.
///
/// # Errors
///
/// [`DecodeError::NoSync`] when no frame starts there; the others when its header cannot be used.
pub fn frame_info(bytes: &[u8]) -> Result<FrameInfo, DecodeError> {
    let packing = packing_at_start(bytes)?;
    let header = header_of(bytes, packing)?;
    Ok(FrameInfo {
        container_bytes: header.frame_size_container_bytes(packing) as usize,
        sample_rate: header
            .sample_rate_hz()
            .ok_or(DecodeError::UnsupportedSampleRate)?,
    })
}

fn big_endian_frame(
    bytes: &[u8],
    packing: SyncWordEncoding,
    length: usize,
) -> Result<Cow<'_, [u8]>, DecodeError> {
    let frame = bytes.get(..length & !1).ok_or(DecodeError::Truncated)?;
    Ok(match (packing, order_of(packing)) {
        (_, Some(order)) => Cow::Owned(unpack_14bit_to_16bit(frame, order)?),
        (SyncWordEncoding::RawLittleEndian, None) => Cow::Owned(swapped(frame)),
        _ => Cow::Borrowed(frame),
    })
}

fn transmission_order(arrangement: AmodeArrangement) -> Option<&'static [Speaker]> {
    use Speaker::{BackCenter, Center, Left, LeftSurround, Right, RightSurround};
    Some(match arrangement {
        AmodeArrangement::Mono => &[Center],
        AmodeArrangement::DualMono
        | AmodeArrangement::Stereo
        | AmodeArrangement::SumDifference
        | AmodeArrangement::LtRt => &[Left, Right],
        AmodeArrangement::ClR => &[Center, Left, Right],
        AmodeArrangement::LrS => &[Left, Right, BackCenter],
        AmodeArrangement::ClRS => &[Center, Left, Right, BackCenter],
        AmodeArrangement::LrSlSr => &[Left, Right, LeftSurround, RightSurround],
        AmodeArrangement::ClRSlSr => &[Center, Left, Right, LeftSurround, RightSurround],
        _ => return None,
    })
}

fn interleaved(planes: &[(Speaker, Vec<i32>)]) -> (Vec<f32>, usize) {
    let samples = planes
        .iter()
        .map(|(_, plane)| plane.len())
        .min()
        .unwrap_or(0);
    let mut pcm = Vec::with_capacity(samples * planes.len());
    for at in 0..samples {
        pcm.extend(
            planes
                .iter()
                .map(|(_, plane)| plane[at] as f32 / FULL_SCALE),
        );
    }
    (pcm, samples)
}

/// One stream's decoding state. Frames of a stream go through one decoder in order, as each carries on the filter of the
/// frame before.
#[derive(Debug, Default)]
pub struct Decoder {
    stream: Option<CoreStreamDecoder>,
}

impl Decoder {
    /// A decoder with cleared filters.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Forgets the filters, as before a seek.
    pub fn reset(&mut self) {
        self.stream = None;
    }

    /// Decodes the frame `bytes` starts with; what follows it is ignored.
    ///
    /// # Errors
    ///
    /// A frame that cannot be decoded leaves the filters as they were.
    pub fn decode(&mut self, bytes: &[u8]) -> Result<Decoded, DecodeError> {
        let packing = packing_at_start(bytes)?;
        let length = header_of(bytes, packing)?.frame_size_container_bytes(packing) as usize;
        let frame = big_endian_frame(bytes, packing, length)?;
        let header = parse_frame_header(&frame)?;
        let sample_rate = header
            .sample_rate_hz()
            .ok_or(DecodeError::UnsupportedSampleRate)?;
        let order = transmission_order(header.amode_arrangement())
            .ok_or(DecodeError::UnsupportedChannels)?;
        let (coding, _) = decode_audio_coding_header_at(
            &frame,
            header.header_bit_length() as usize,
            header.crc_present,
        )?;
        if coding.n_pchs != order.len() {
            return Err(DecodeError::UnsupportedChannels);
        }
        let stream = match &mut self.stream {
            Some(stream) if stream.channel_count() == order.len() => stream,
            slot => slot.insert(CoreStreamDecoder::new(order.len())),
        };
        let primary = stream.decode_frame(&frame, &header)?;
        let lfe = stream.take_last_lfe_pcm();
        let mut planes: Vec<(Speaker, Vec<i32>)> = order.iter().copied().zip(primary).collect();
        if !lfe.is_empty() {
            planes.push((Speaker::Lfe, lfe));
        }
        planes.sort_by_key(|(speaker, _)| *speaker);
        let (pcm, samples) = interleaved(&planes);
        Ok(Decoded {
            pcm,
            speakers: planes.into_iter().map(|(speaker, _)| speaker).collect(),
            samples,
            sample_rate,
        })
    }
}
