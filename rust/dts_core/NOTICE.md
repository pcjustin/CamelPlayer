# Notice

The modules of this crate, except `src/stream.rs` and the crate root's first lines, are copied from
[oxideav-dts](https://github.com/OxideAV/oxideav-dts) at commit `ec55712` (0.0.2), MIT license, copyright
Karpelès Lab Inc.; its license is in `LICENSE-oxideav-dts`.

Left out: the encoder and what only it uses (`encoder.rs`, `bitwriter.rs`, `qmf_analysis.rs`, `lfe_analysis.rs`) and the
`oxideav-core` integration (`registry.rs`). Fixtures in `tests/fixtures` come from the same repository.

Also left out, as the decoder never reaches them: the crate root exports only what `src/stream.rs`, the tests and the
fuzz target use, and what the compiler then reported as never used was removed, with the unit tests and the doc
examples that only reached it. The tests split a stream into frames with `find_sync` and `frame_info`, as the Engine
does, so the frame iterator is not exported either, and the modules only it reached went whole: `aux_data`, `crc16`,
`dmix_coeff`, `drc_range`, `optional_info` and `rev2_aux`, with `sum_diff`, which had nothing left but its tests'
section headings (its matrix decode is `apply_sum_difference` in `subframe_pcm.rs`). By module, of those kept:

- `audio_array`: `channel_count`, `decode_audio_data_subframe_at`, `decode_audio_data_subframe_partial_at`
- `audio_huff`: `abits`, `levels`
- `block_code`: `block_code_max_code`, `block_code_offset`
- `d10_vq`: `ADPCM_VQ_INDEX_BITS`, `HFREQ_VQ_ENTRIES_PER_VECTOR`, `VqCodebookShapeError`, `from_coefficients`, `from_elements`, `from_entries`, `from_packed_entries`, `is_empty`, `none`, `unpack_hfreq_vq_entry`, `with_adpcm`, `with_hfreq`
- `d6_block_book`: `D6BlockBook`, `D6_BLOCK_ELEMENTS`, `D6_BOOK_13`, `D6_BOOK_17`, `D6_BOOK_25`, `D6_BOOK_3`, `D6_BOOK_5`, `D6_BOOK_7`, `D6_BOOK_9`, `build_book`, `code_value`, `d6_book_for_levels`, `decode_block_code_table`, `levels`
- `dsync`: `DSYNC_WIRE_BITS`, `decode_dsync_at`, `dsync_present`
- `header`: `encode_frame_header_be`, `encode_frame_header_le`
- `inverse_adpcm`: `inverse_adpcm_decode_i32`, `inverse_adpcm_required`, `update_history_f64`, `update_history_i32`
- `iter`: `FrameIterator`, `FrameIterator14`, `FrameIteratorResync`, `FrameView`, `FrameView14`, `ResyncCause`, `ResyncEvent`, `SyncIterator`, `find_all_syncs`, `iter_frames`, `iter_frames_14bit`, `iter_frames_resync`, `iter_syncs`, `sync_byte_length`, `sync_byte_range`
- `join_scale`: `JOIN_SCALE_UNITY_INDEX`
- `joint_subband`: `joint_subband_decode_range_f64`, `joint_subband_decode_range_i32`, `joint_subband_required`, `validate_joint_shape`
- `lfe_interp`: `decimation_select`, `spec_table_name`
- `lfe_synth`: `decode_subframe`, `history`, `interpolator`
- `qmf_multichannel`: `channels`, `synthesize_interleaved`, `synthesize_planar_from_header`
- `qmf_synth`: `x_history`, `z_accumulator`
- `side_info`: `WIRE_BITS`, `code`, `decode_abits_at`, `decode_adj_at`, `decode_join_scale_at`, `decode_scales_at`, `decode_subsubframe_count_at`, `decode_tmode_at`, `is_termination_tail`, `multiplier`, `multiplier_rational`, `partial_sample_count`, `samples_per_subsubframe_normal`, `scales_code`, `scales_table`, `thuff`
- `step_size`: `dequant_scale`, `dequant_subsubframe`, `scale_subsubframe_samples`
- `subframe_pcm`: `Subframe`, `adpcm_history`, `decode_core_frame`, `decode_core_frame_with_info`, `decode_core_frame_with_info_into`, `decode_frame_with_info`, `decode_subframe`, `decode_subframe_with_joint`, `qmf`, `reset_adpcm_history`, `set_vq_codebooks`, `subframe_decoder`, `vq_codebooks`
