#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMEL_ENGINE_ABI_VERSION 1u
#define CAMEL_ENGINE_MAX_RENDER_UNITS 65536u

#define CAMEL_AUDIO_PCM 1u
#define CAMEL_AUDIO_DSD 2u
#define CAMEL_AUDIO_DOP 3u

#define CAMEL_SAMPLE_S16_LE 1u
#define CAMEL_SAMPLE_S24_LE 2u
#define CAMEL_SAMPLE_S24_IN_32_LE 3u
#define CAMEL_SAMPLE_S32_LE 4u
#define CAMEL_SAMPLE_F32_LE 5u
#define CAMEL_SAMPLE_DSD_U8 6u
#define CAMEL_SAMPLE_DOP_S24_LE 7u

typedef int32_t CamelEngineStatus;

#define CAMEL_ENGINE_OK 0
#define CAMEL_ENGINE_END 1
#define CAMEL_ENGINE_UNDERRUN 2
#define CAMEL_ENGINE_E_INVALID_ARGUMENT (-1)
#define CAMEL_ENGINE_E_ABI_MISMATCH (-2)
#define CAMEL_ENGINE_E_STRUCT_SIZE (-3)
#define CAMEL_ENGINE_E_BUFFER_TOO_SMALL (-4)
#define CAMEL_ENGINE_E_INVALID_STATE (-5)
#define CAMEL_ENGINE_E_UNSUPPORTED_FORMAT (-6)
#define CAMEL_ENGINE_E_LIMIT_EXCEEDED (-7)
#define CAMEL_ENGINE_E_NOT_READY (-8)
#define CAMEL_ENGINE_E_PANIC (-9)
#define CAMEL_ENGINE_E_SOURCE_IO (-10)
#define CAMEL_ENGINE_E_DECODE (-11)
#define CAMEL_ENGINE_E_SEEK (-12)

#define CAMEL_DECODER_FFMPEG 1u
#define CAMEL_DECODER_SYMPHONIA 2u
#define CAMEL_DECODER_DR_FLAC 3u
#define CAMEL_DECODER_SNDFILE 4u

#define CAMEL_DECODER_HOST_OK 0
#define CAMEL_DECODER_HOST_E_IO (-1)
#define CAMEL_DECODER_HOST_E_FORMAT (-2)
#define CAMEL_DECODER_HOST_E_UNSUPPORTED (-3)
#define CAMEL_DECODER_HOST_E_DECODE (-4)
#define CAMEL_DECODER_HOST_E_SEEK (-5)

#define CAMEL_RENDER_STATE_NO_SOURCE 0u
#define CAMEL_RENDER_STATE_ACTIVE 1u
#define CAMEL_RENDER_STATE_END_OF_STREAM 2u
#define CAMEL_RENDER_STATE_FAULT 3u
#define CAMEL_RENDER_STATE_UNDERRUN 4u
#define CAMEL_RENDER_STATE_PAUSED 5u

#define CAMEL_PLAYBACK_EMPTY 0u
#define CAMEL_PLAYBACK_READY 1u
#define CAMEL_PLAYBACK_PLAYING 2u
#define CAMEL_PLAYBACK_PAUSED 3u
#define CAMEL_PLAYBACK_STOPPED 4u
#define CAMEL_PLAYBACK_FAILED 5u

#define CAMEL_FLAC_INTEGRITY_UNAVAILABLE 0u
#define CAMEL_FLAC_INTEGRITY_INCOMPLETE 1u
#define CAMEL_FLAC_INTEGRITY_VERIFIED 2u
#define CAMEL_FLAC_INTEGRITY_FAILED 3u

typedef struct CamelAudioFormatV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t stream_kind;
    uint32_t sample_format;
    uint32_t rate_hz;
    uint32_t channel_count;
    uint32_t valid_bits;
    uint32_t container_bits;
    uint64_t channel_mask;
} CamelAudioFormatV1;

typedef struct CamelEngineConfigV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t max_render_units;
    CamelAudioFormatV1 format;
} CamelEngineConfigV1;

typedef struct CamelCommandStampV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t command_id;
    uint64_t generation;
    uint64_t queue_revision;
} CamelCommandStampV1;

typedef struct CamelTrackSourceV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const uint8_t* path_utf8;
    uint64_t path_length_bytes;
    uint64_t start_frame;
    uint64_t end_frame_exclusive;
    uint32_t has_end_frame;
    uint32_t reserved;
} CamelTrackSourceV1;

typedef struct CamelSourceInfoV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t valid_bits;
    uint32_t reserved;
    uint64_t total_frames;
    uint32_t has_md5;
    uint32_t reserved2;
    uint8_t md5[16];
} CamelSourceInfoV1;

typedef int32_t (*CamelDecoderOpenV1)(void* context, uint32_t decoder_kind, const uint8_t* path_utf8, uint64_t path_length_bytes,
                                      void** out_decoder, CamelSourceInfoV1* info);
typedef int32_t (*CamelDecoderReadV1)(void* context, void* decoder, int32_t* output, uint64_t frames, uint64_t* frames_read);
typedef int32_t (*CamelDecoderSeekV1)(void* context, void* decoder, uint64_t frame);
typedef void (*CamelDecoderCloseV1)(void* context, void* decoder);

typedef struct CamelDecoderHostV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    CamelDecoderOpenV1 open;
    CamelDecoderReadV1 read;
    CamelDecoderSeekV1 seek;
    CamelDecoderCloseV1 close;
} CamelDecoderHostV1;

typedef struct CamelRenderRequestV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t units;
    uint8_t* output;
    uint64_t output_capacity_bytes;
} CamelRenderRequestV1;

typedef struct CamelRenderResultV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t units_rendered;
    uint64_t required_bytes;
    uint32_t state;
    uint32_t reserved;
} CamelRenderResultV1;

typedef struct CamelEngineStateV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t playback_state;
    int32_t last_error_code;
    CamelCommandStampV1 command;
    uint64_t position_frames;
    uint64_t total_frames;
    uint32_t has_source;
    uint32_t reserved;
    CamelAudioFormatV1 source_format;
    CamelAudioFormatV1 output_format;
} CamelEngineStateV1;

typedef struct CamelEngineTimelineV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t playback_state;
    uint32_t reserved;
    CamelCommandStampV1 command;
    uint64_t decoded_position_frames;
    uint64_t submitted_position_frames;
    uint64_t audible_position_frames;
    uint64_t total_frames;
    uint32_t has_source;
    uint32_t reserved2;
} CamelEngineTimelineV1;

typedef struct CamelFlacIntegrityV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t status;
    uint32_t has_reference_md5;
    uint64_t total_frames;
    uint64_t position_frames;
} CamelFlacIntegrityV1;

typedef struct CamelEngine CamelEngine;

uint32_t camel_engine_abi_version(void);
CamelEngineStatus camel_engine_create_v1(const CamelEngineConfigV1* config, CamelEngine** out_engine);
CamelEngineStatus camel_engine_load_v1(CamelEngine* engine, const CamelCommandStampV1* command, const CamelTrackSourceV1* source);
CamelEngineStatus camel_engine_load_flac_v1(CamelEngine* engine, const CamelCommandStampV1* command,
                                            const CamelTrackSourceV1* source, uint32_t decoder_kind,
                                            const CamelDecoderHostV1* decoder_host);
// Gapless: queue the track that follows the current one; it must decode to the current output format.
CamelEngineStatus camel_engine_queue_next_flac_v1(CamelEngine* engine, const CamelCommandStampV1* command,
                                                  const CamelTrackSourceV1* source, uint32_t decoder_kind,
                                                  const CamelDecoderHostV1* decoder_host);
// Discards the queued track; CAMEL_ENGINE_E_INVALID_STATE once decoding has already moved on to it.
CamelEngineStatus camel_engine_clear_next_v1(CamelEngine* engine, const CamelCommandStampV1* command);
// Queued tracks that became audible, the current track position, and whether a queued track still waits in the
// slot (one more may be queued only when it does not). Safe on the output callback thread.
CamelEngineStatus camel_engine_get_transitions_v1(CamelEngine* engine, uint64_t* transitions, uint64_t* track_position,
                                                  uint32_t* next_queued);
CamelEngineStatus camel_engine_play_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_pause_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_stop_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_seek_v1(CamelEngine* engine, const CamelCommandStampV1* command, uint64_t frame);
CamelEngineStatus camel_engine_get_timeline_v1(CamelEngine* engine, CamelEngineTimelineV1* timeline);
CamelEngineStatus camel_engine_report_audible_position_v1(CamelEngine* engine, const CamelCommandStampV1* command,
                                                          uint64_t audible_frame);
CamelEngineStatus camel_engine_prefill_v1(CamelEngine* engine, uint64_t frame_limit, uint64_t* frames_prefilled);
CamelEngineStatus camel_engine_read_decoded_v1(CamelEngine* engine, int32_t* output, uint64_t frame_capacity,
                                               uint64_t* frames_read);
CamelEngineStatus camel_engine_set_output_format_v1(CamelEngine* engine, const CamelCommandStampV1* command,
                                                    const CamelAudioFormatV1* format);
CamelEngineStatus camel_engine_get_state_v1(CamelEngine* engine, CamelEngineStateV1* state);
CamelEngineStatus camel_engine_get_flac_integrity_v1(CamelEngine* engine, CamelFlacIntegrityV1* integrity);
CamelEngineStatus camel_engine_copy_last_error_v1(CamelEngine* engine, uint8_t* output, uint64_t output_capacity_bytes,
                                                  uint64_t* required_bytes);
CamelEngineStatus camel_engine_render_v1(CamelEngine* engine, const CamelRenderRequestV1* request, CamelRenderResultV1* result);
CamelEngineStatus camel_engine_quiesce_v1(CamelEngine* engine);
CamelEngineStatus camel_engine_destroy_v1(CamelEngine* engine);

#ifdef __cplusplus
}
#endif
