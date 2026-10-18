use crate::{
    AudioFormat, AUDIO_DOP, AUDIO_DSD, AUDIO_PCM, ENGINE_E_UNSUPPORTED_FORMAT, SAMPLE_S16_LE,
    SAMPLE_S24_IN_32_LE, SAMPLE_S24_LE, SAMPLE_S32_LE,
};

/// Bytes of one engine frame: a PCM or DoP sample per channel, or one byte per channel for native DSD.
pub(crate) fn bytes_per_frame(format: &AudioFormat) -> Option<usize> {
    let bytes_per_sample = match format.stream_kind {
        AUDIO_PCM | AUDIO_DOP => format.container_bits / 8,
        AUDIO_DSD => 1,
        _ => return None,
    };
    usize::try_from(format.channel_count)
        .ok()?
        .checked_mul(usize::try_from(bytes_per_sample).ok()?)
}

/// Packing integer PCM; DSD and DoP streams are packed by the converter.
fn pcm_bytes_per_frame(format: &AudioFormat) -> Option<usize> {
    if format.stream_kind != AUDIO_PCM {
        return None;
    }
    bytes_per_frame(format)
}

pub(crate) fn pack_pcm(
    samples: &[i32],
    source_bits: u32,
    format: &AudioFormat,
    output: &mut [u8],
) -> Result<usize, i32> {
    let Some(bytes_per_sample) = pcm_bytes_per_frame(format)
        .and_then(|frame_bytes| frame_bytes.checked_div(format.channel_count as usize))
    else {
        return Err(ENGINE_E_UNSUPPORTED_FORMAT);
    };
    let output_bits = (bytes_per_sample * 8) as u32;
    if !matches!(source_bits, 16 | 24)
        || source_bits > output_bits
        || !matches!(
            format.sample_format,
            SAMPLE_S16_LE | SAMPLE_S24_LE | SAMPLE_S24_IN_32_LE | SAMPLE_S32_LE
        )
    {
        return Err(ENGINE_E_UNSUPPORTED_FORMAT);
    }
    let required_bytes = samples
        .len()
        .checked_mul(bytes_per_sample)
        .ok_or(ENGINE_E_UNSUPPORTED_FORMAT)?;
    if output.len() < required_bytes {
        return Err(ENGINE_E_UNSUPPORTED_FORMAT);
    }

    for (sample, destination) in samples
        .iter()
        .zip(output[..required_bytes].chunks_exact_mut(bytes_per_sample))
    {
        let aligned = (*sample as u32).wrapping_shl(output_bits - source_bits);
        for (index, byte) in destination.iter_mut().enumerate() {
            *byte = (aligned >> (index * 8)) as u8;
        }
    }
    Ok(required_bytes)
}

#[cfg(test)]
mod tests {
    use super::pack_pcm;
    use crate::{
        AudioFormat, AUDIO_PCM, ENGINE_E_UNSUPPORTED_FORMAT, SAMPLE_S16_LE, SAMPLE_S24_IN_32_LE,
        SAMPLE_S24_LE, SAMPLE_S32_LE,
    };

    fn format(sample_format: u32, valid_bits: u32, container_bits: u32) -> AudioFormat {
        AudioFormat {
            stream_kind: AUDIO_PCM,
            sample_format,
            channel_count: 1,
            valid_bits,
            container_bits,
        }
    }

    #[test]
    fn packs_signed_pcm_little_endian_without_changing_sample_bits() {
        let samples = [0x1234, -0x1234];
        let mut output = [0; 4];
        assert_eq!(
            pack_pcm(&samples, 16, &format(SAMPLE_S16_LE, 16, 16), &mut output),
            Ok(4)
        );
        assert_eq!(output, [0x34, 0x12, 0xCC, 0xED]);

        let samples = [0x1234, -0x1234];
        let mut output = [0; 6];
        assert_eq!(
            pack_pcm(&samples, 16, &format(SAMPLE_S24_LE, 24, 24), &mut output),
            Ok(6)
        );
        assert_eq!(output, [0x00, 0x34, 0x12, 0x00, 0xCC, 0xED]);
    }

    #[test]
    fn packs_24_bit_pcm_into_supported_integer_containers() {
        let samples = [0x123456, -0x123456];
        let mut packed = [0; 6];
        assert_eq!(
            pack_pcm(&samples, 24, &format(SAMPLE_S24_LE, 24, 24), &mut packed),
            Ok(6)
        );
        assert_eq!(packed, [0x56, 0x34, 0x12, 0xAA, 0xCB, 0xED]);

        let mut padded = [0; 8];
        assert_eq!(
            pack_pcm(
                &samples,
                24,
                &format(SAMPLE_S24_IN_32_LE, 24, 32),
                &mut padded
            ),
            Ok(8)
        );
        assert_eq!(padded, [0x00, 0x56, 0x34, 0x12, 0x00, 0xAA, 0xCB, 0xED]);

        let mut s32 = [0; 8];
        assert_eq!(
            pack_pcm(&samples, 24, &format(SAMPLE_S32_LE, 32, 32), &mut s32),
            Ok(8)
        );
        assert_eq!(s32, padded);
    }

    #[test]
    fn rejects_lossy_or_non_pcm_packing() {
        let mut output = [0; 2];
        assert_eq!(
            pack_pcm(&[1], 24, &format(SAMPLE_S16_LE, 16, 16), &mut output),
            Err(ENGINE_E_UNSUPPORTED_FORMAT)
        );
        assert_eq!(
            pack_pcm(&[1], 32, &format(SAMPLE_S32_LE, 32, 32), &mut output),
            Err(ENGINE_E_UNSUPPORTED_FORMAT)
        );
        let mut non_pcm = format(SAMPLE_S32_LE, 32, 32);
        non_pcm.stream_kind = crate::AUDIO_DSD;
        assert_eq!(
            pack_pcm(&[1], 16, &non_pcm, &mut output),
            Err(ENGINE_E_UNSUPPORTED_FORMAT)
        );
    }
}
