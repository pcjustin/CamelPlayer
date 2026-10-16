// C ABI of rust/symphonia_ffi (docs/計劃書.md §16.3). Keep in sync with src/lib.rs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SYM_OK = 0,
    SYM_END = 1,
    SYM_E_IO = -1,
    SYM_E_FORMAT = -2,
    SYM_E_UNSUPPORTED = -3,
    SYM_E_DECODE = -4,
    SYM_E_SEEK = -5,
    SYM_E_ARG = -6
};

typedef struct SymDecoder SymDecoder;
typedef struct SymInfo {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint64_t total_frames;
    uint32_t has_md5;
    uint8_t md5[16];
} SymInfo;

int32_t sym_open(const char* utf8Path, SymDecoder** out, SymInfo* info);
int32_t sym_read(SymDecoder* dec, int32_t* dst, size_t frames, size_t* read);
int32_t sym_seek(SymDecoder* dec, uint64_t frame);
void sym_last_error(const SymDecoder* dec, char* buf, size_t len);
void sym_close(SymDecoder* dec);

#ifdef __cplusplus
}
#endif
