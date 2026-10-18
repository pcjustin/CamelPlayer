//! Raw DSD output (docs/Plan.md §5.8): native bytes or DoP, and format-correct silence.
//!
//! Engine frames of a native DSD stream are byte frames: one byte per channel, eight DSD samples, the oldest in the
//! most significant bit. A DoP frame carries two such bytes per channel in a 24-bit sample whose top byte is a marker
//! that alternates 0x05/0xFA from frame to frame.

use crate::{AudioFormat, AUDIO_DOP, AUDIO_DSD};

/// The DSD idle pattern.
pub(crate) const DSD_SILENCE: u8 = 0x69;
pub(crate) const DOP_MARKERS: [u8; 2] = [0x05, 0xFA];

/// ABI render units per engine frame: DSD streams count DSD samples, eight per byte frame.
pub(crate) fn units_per_frame(format: &AudioFormat) -> u64 {
    if format.stream_kind == AUDIO_DSD {
        8
    } else {
        1
    }
}

/// One DoP sample, little endian: the earlier DSD byte above the later one, the marker on top. A 32-bit container
/// carries the same 24 bits MSB-aligned.
pub(crate) fn put_dop_sample(out: &mut [u8], first: u8, second: u8, marker: u8) {
    let base = out.len() - 3;
    out[..base].fill(0);
    out[base] = second;
    out[base + 1] = first;
    out[base + 2] = marker;
}

/// Fills whole frames with silence: the DSD idle pattern, as DoP payload for a DoP stream (markers are stamped
/// separately), zero for PCM.
pub(crate) fn fill_silence(format: &AudioFormat, output: &mut [u8]) {
    match format.stream_kind {
        AUDIO_DSD => output.fill(DSD_SILENCE),
        AUDIO_DOP => {
            let bytes_per_sample = (format.container_bits / 8) as usize;
            for sample in output.chunks_exact_mut(bytes_per_sample) {
                put_dop_sample(sample, DSD_SILENCE, DSD_SILENCE, DOP_MARKERS[0]);
            }
        }
        _ => output.fill(0),
    }
}

/// Writes alternating DoP markers into every frame of `output`, continuing from `phase` (the index of the next
/// marker), and returns the phase after the last frame.
pub(crate) fn stamp_markers(format: &AudioFormat, output: &mut [u8], mut phase: usize) -> usize {
    let bytes_per_sample = (format.container_bits / 8) as usize;
    let bytes_per_frame = bytes_per_sample * format.channel_count as usize;
    for frame in output.chunks_exact_mut(bytes_per_frame) {
        for sample in frame.chunks_exact_mut(bytes_per_sample) {
            sample[bytes_per_sample - 1] = DOP_MARKERS[phase];
        }
        phase ^= 1;
    }
    phase
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{SAMPLE_DOP_S24_IN_32_LE, SAMPLE_DOP_S24_LE, SAMPLE_DSD_U8};

    fn dop(container_bits: u32) -> AudioFormat {
        AudioFormat {
            stream_kind: AUDIO_DOP,
            sample_format: if container_bits == 24 {
                SAMPLE_DOP_S24_LE
            } else {
                SAMPLE_DOP_S24_IN_32_LE
            },
            channel_count: 2,
            valid_bits: 24,
            container_bits,
        }
    }

    #[test]
    fn dop_sample_puts_the_earlier_byte_above_the_later_one_under_the_marker() {
        let mut packed = [0xEE; 3];
        put_dop_sample(&mut packed, 0x12, 0x34, 0x05);
        assert_eq!(packed, [0x34, 0x12, 0x05]);
        let mut padded = [0xEE; 4];
        put_dop_sample(&mut padded, 0x12, 0x34, 0xFA);
        assert_eq!(padded, [0x00, 0x34, 0x12, 0xFA]);
    }

    #[test]
    fn silence_is_the_idle_pattern_and_markers_continue_across_calls() {
        let native = AudioFormat {
            stream_kind: AUDIO_DSD,
            sample_format: SAMPLE_DSD_U8,
            channel_count: 2,
            valid_bits: 1,
            container_bits: 8,
        };
        let mut bytes = [0; 6];
        fill_silence(&native, &mut bytes);
        assert_eq!(bytes, [DSD_SILENCE; 6]);

        for bits in [24, 32] {
            let format = dop(bits);
            let frame = bits as usize / 8 * 2;
            let mut out = vec![0xEE; 3 * frame];
            fill_silence(&format, &mut out);
            let phase = stamp_markers(&format, &mut out[..frame], 1);
            assert_eq!(stamp_markers(&format, &mut out[frame..], phase), 0);
            let markers: Vec<u8> = out
                .chunks_exact(bits as usize / 8)
                .map(|s| s[s.len() - 1])
                .collect();
            assert_eq!(markers, [0xFA, 0xFA, 0x05, 0x05, 0xFA, 0xFA]);
            for sample in out.chunks_exact(bits as usize / 8) {
                let payload = &sample[sample.len() - 3..sample.len() - 1];
                assert_eq!(payload, [DSD_SILENCE, DSD_SILENCE]);
                assert!(sample[..sample.len() - 3].iter().all(|&b| b == 0));
            }
        }
    }
}
