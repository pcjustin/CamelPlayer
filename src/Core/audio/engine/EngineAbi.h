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

#define CAMEL_RENDER_STATE_NO_SOURCE 0u
#define CAMEL_RENDER_STATE_ACTIVE 1u
#define CAMEL_RENDER_STATE_END_OF_STREAM 2u
#define CAMEL_RENDER_STATE_FAULT 3u

#define CAMEL_PLAYBACK_EMPTY 0u
#define CAMEL_PLAYBACK_READY 1u
#define CAMEL_PLAYBACK_PLAYING 2u
#define CAMEL_PLAYBACK_PAUSED 3u
#define CAMEL_PLAYBACK_STOPPED 4u
#define CAMEL_PLAYBACK_FAILED 5u

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

typedef struct CamelEngine CamelEngine;

uint32_t camel_engine_abi_version(void);
CamelEngineStatus camel_engine_create_v1(const CamelEngineConfigV1* config, CamelEngine** out_engine);
CamelEngineStatus camel_engine_load_v1(CamelEngine* engine, const CamelCommandStampV1* command, const CamelTrackSourceV1* source);
CamelEngineStatus camel_engine_play_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_pause_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_stop_v1(CamelEngine* engine, const CamelCommandStampV1* command);
CamelEngineStatus camel_engine_seek_v1(CamelEngine* engine, const CamelCommandStampV1* command, uint64_t frame);
CamelEngineStatus camel_engine_set_output_format_v1(CamelEngine* engine, const CamelCommandStampV1* command,
                                                    const CamelAudioFormatV1* format);
CamelEngineStatus camel_engine_get_state_v1(CamelEngine* engine, CamelEngineStateV1* state);
CamelEngineStatus camel_engine_copy_last_error_v1(CamelEngine* engine, uint8_t* output, uint64_t output_capacity_bytes,
                                                  uint64_t* required_bytes);
CamelEngineStatus camel_engine_render_v1(CamelEngine* engine, const CamelRenderRequestV1* request, CamelRenderResultV1* result);
CamelEngineStatus camel_engine_quiesce_v1(CamelEngine* engine);
CamelEngineStatus camel_engine_destroy_v1(CamelEngine* engine);

#ifdef __cplusplus
}
#endif
