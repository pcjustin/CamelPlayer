// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use dts_core::{
    DecodeError, Decoded, Decoder, FourteenBitByteOrder, Speaker, SyncWordEncoding, find_sync,
    frame_info,
};
use dts_core::{iter_frames, pack_16bit_to_14bit};

const SURROUND: &[u8] = include_bytes!("fixtures/dts_51_lfe.bin");
const SURROUND_REFERENCE: &[u8] = include_bytes!("fixtures/dts_51_lfe_ffmpeg_ref.s32");
const STEREO: &[u8] = include_bytes!("fixtures/dts_5_frames.bin");

const ORDER: [Speaker; 6] = [
    Speaker::Left,
    Speaker::Right,
    Speaker::Center,
    Speaker::Lfe,
    Speaker::LeftSurround,
    Speaker::RightSurround,
];

fn frames_of(stream: &[u8]) -> Vec<&[u8]> {
    iter_frames(stream)
        .map(|frame| frame.expect("the fixture's frames parse").data)
        .collect()
}

fn decode_all(frames: &[Vec<u8>]) -> Vec<Decoded> {
    let mut decoder = Decoder::new();
    frames
        .iter()
        .map(|frame| decoder.decode(frame).expect("the frame decodes"))
        .collect()
}

fn big_endian(stream: &[u8]) -> Vec<Vec<u8>> {
    frames_of(stream).into_iter().map(<[u8]>::to_vec).collect()
}

fn pcm_of(decoded: &[Decoded]) -> Vec<f32> {
    decoded
        .iter()
        .flat_map(|frame| frame.pcm.iter().copied())
        .collect()
}

fn little_endian_words(frame: &[u8]) -> Vec<u8> {
    frame
        .as_chunks::<2>()
        .0
        .iter()
        .flat_map(|word| [word[1], word[0]])
        .collect()
}

#[test]
fn surround_comes_out_in_the_players_order_at_the_references_level() {
    let decoded = decode_all(&big_endian(SURROUND));
    let pcm = pcm_of(&decoded);
    assert_eq!(decoded[0].speakers, ORDER);
    assert_eq!(decoded[0].sample_rate, 48000);
    let reference: Vec<f32> = SURROUND_REFERENCE
        .as_chunks::<4>()
        .0
        .iter()
        .map(|sample| {
            i32::from_le_bytes([sample[0], sample[1], sample[2], sample[3]]) as f32
                / 2_147_483_648.0
        })
        .collect();
    assert_eq!(pcm.len(), reference.len());
    let peak = pcm
        .iter()
        .zip(&reference)
        .map(|(a, b)| (a - b).abs())
        .fold(0.0_f32, f32::max);
    assert!(peak < 1e-4, "peak difference {peak}");
    assert!(
        pcm.iter().any(|sample| sample.abs() > 0.05),
        "the fixture is not silent"
    );
}

#[test]
fn stereo_has_two_channels_and_no_lfe() {
    let decoded = decode_all(&big_endian(STEREO));
    assert_eq!(decoded[0].speakers, [Speaker::Left, Speaker::Right]);
    assert_eq!(decoded[0].pcm.len(), decoded[0].samples * 2);
}

#[test]
fn every_packing_decodes_to_the_same_samples() {
    let frames = big_endian(SURROUND);
    let expected = pcm_of(&decode_all(&frames));

    let little: Vec<Vec<u8>> = frames
        .iter()
        .map(|frame| little_endian_words(frame))
        .collect();
    assert_eq!(pcm_of(&decode_all(&little)), expected);

    for order in [
        FourteenBitByteOrder::BigEndian,
        FourteenBitByteOrder::LittleEndian,
    ] {
        let packed: Vec<Vec<u8>> = frames
            .iter()
            .map(|frame| pack_16bit_to_14bit(frame, order).0)
            .collect();
        assert_eq!(pcm_of(&decode_all(&packed)), expected);
    }
}

#[test]
fn a_frame_is_found_in_each_packing_and_measured() {
    let frame = &big_endian(SURROUND)[0];
    let mut stream = vec![0x11_u8; 6];
    stream.extend_from_slice(frame);
    let sync = find_sync(&stream, 0).expect("found");
    assert_eq!(sync.offset, 6);
    assert_eq!(sync.packing, SyncWordEncoding::RawBigEndian);
    assert_eq!(
        frame_info(&stream[6..]).expect("read").container_bytes,
        frame.len()
    );

    let (packed, _) = pack_16bit_to_14bit(frame, FourteenBitByteOrder::LittleEndian);
    assert_eq!(
        find_sync(&packed, 0).expect("found").packing,
        SyncWordEncoding::FourteenBitLittleEndian
    );
    let info = frame_info(&packed).expect("read");
    assert_eq!(info.container_bytes, packed.len());
    assert_eq!(info.sample_rate, 48000);
    assert_eq!(
        find_sync(&little_endian_words(frame), 0)
            .expect("found")
            .packing,
        SyncWordEncoding::RawLittleEndian
    );
}

#[test]
fn what_follows_a_frame_is_not_part_of_it() {
    let frames = big_endian(SURROUND);
    let mut decoder = Decoder::new();
    assert!(decoder.decode(SURROUND).is_ok());
    let alone = Decoder::new().decode(&frames[0]).expect("decodes");
    assert_eq!(Decoder::new().decode(SURROUND).expect("decodes"), alone);
}

#[test]
fn the_filters_carry_on_from_frame_to_frame_until_a_reset() {
    let frames = big_endian(SURROUND);
    let mut decoder = Decoder::new();
    let first = decoder.decode(&frames[0]).expect("decodes");
    assert_ne!(decoder.decode(&frames[0]).expect("decodes"), first);
    decoder.reset();
    assert_eq!(decoder.decode(&frames[0]).expect("decodes"), first);
}

#[test]
fn a_cut_frame_is_an_error_and_not_a_panic() {
    let frame = &big_endian(SURROUND)[0];
    let cut = &frame[..frame.len() / 2];
    assert_eq!(Decoder::new().decode(cut), Err(DecodeError::Truncated));
    assert_eq!(Decoder::new().decode(&[]), Err(DecodeError::NoSync));
    assert_eq!(Decoder::new().decode(&[0_u8; 64]), Err(DecodeError::NoSync));
    assert_eq!(frame_info(&frame[2..]), Err(DecodeError::NoSync));
}

#[test]
fn damaged_frames_never_panic() {
    let frames = big_endian(SURROUND);
    let mut state = 0x9E37_79B9_7F4A_7C15_u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for _ in 0..2000 {
        let mut frame = frames[0].clone();
        for _ in 0..1 + next() % 40 {
            let at = (next() % frame.len() as u64) as usize;
            frame[at] = (next() >> 32) as u8;
        }
        let _ = Decoder::new().decode(&frame);
    }
}

/// The frame with its source-PCM-resolution code (PCMR, 3 bits after the header's 13 flag bits, the optional header
/// CRC and 7 more bits) set to `code`.
fn with_pcm_resolution(frame: &[u8], code: u8) -> Vec<u8> {
    let mut out = frame.to_vec();
    let crc_present = out[4] & 0x02 != 0;
    let first = 95 + if crc_present { 16 } else { 0 };
    for bit in 0..3 {
        let at = first + bit;
        let mask = 0x80_u8 >> (at % 8);
        if code & (0b100 >> bit) != 0 {
            out[at / 8] |= mask;
        } else {
            out[at / 8] &= !mask;
        }
    }
    out
}

#[test]
fn every_source_resolution_decodes_at_the_same_level() {
    let frames = big_endian(SURROUND);
    let expected = pcm_of(&decode_all(&frames));
    for code in [1_u8, 2, 3, 5, 6] {
        let rewritten: Vec<Vec<u8>> = frames
            .iter()
            .map(|frame| with_pcm_resolution(frame, code))
            .collect();
        assert_eq!(
            dts_core::parse_frame_header(&rewritten[0])
                .expect("parses")
                .source_pcm_resolution_index,
            code
        );
        let pcm = pcm_of(&decode_all(&rewritten));
        assert_eq!(pcm.len(), expected.len());
        let peak = pcm
            .iter()
            .zip(&expected)
            .map(|(a, b)| (a - b).abs())
            .fold(0.0_f32, f32::max);
        assert!(peak < 1e-6, "PCMR {code}: peak difference {peak}");
    }
}

#[test]
fn the_invalid_lfe_code_is_an_error() {
    // LFF is the header's bits 85 and 86, before the optional CRC.
    let frames = big_endian(SURROUND);
    assert_eq!(frames[0][10] >> 1 & 0b11, 2);
    for frame in &frames {
        let mut invalid = frame.clone();
        invalid[10] |= 0b110;
        assert!(Decoder::new().decode(&invalid).is_err());
    }
}

/// The frame with its block count (NBLKS, the header's bits 39 to 45) set to `blocks`.
fn with_blocks(frame: &[u8], blocks: u8) -> Vec<u8> {
    let mut out = frame.to_vec();
    out[4] = out[4] & !1 | blocks >> 6;
    out[5] = out[5] & 0b11 | (blocks & 0x3F) << 2;
    out
}

#[test]
fn a_frame_with_more_samples_than_its_header_says_is_an_error() {
    let frames = big_endian(SURROUND);
    let header = dts_core::parse_frame_header(&frames[0]).expect("parses");
    assert_eq!(header.blocks_per_frame, 15);
    assert_eq!(
        Decoder::new().decode(&with_blocks(&frames[0], 15)),
        Decoder::new().decode(&frames[0])
    );
    for frame in &frames {
        assert!(Decoder::new().decode(&with_blocks(frame, 7)).is_err());
    }
}

#[test]
fn output_stays_within_full_scale() {
    for frame in pcm_of(&decode_all(&big_endian(SURROUND))) {
        assert!((-1.0..=1.0).contains(&frame));
    }
}
