/*
 * ALEC — Adaptive Lazy Evolving Compression
 * C/C++ FFI bindings (from alec-ffi crate v1.2.1, bare-metal)
 *
 * Build libalec_ffi.a for nRF9151 (Cortex-M33):
 *   rustup target add thumbv8m.main-none-eabihf
 *   cd alec-ffi && cargo build --release --target thumbv8m.main-none-eabihf
 *   cp target/thumbv8m.main-none-eabihf/release/libalec_ffi.a <firmware>/lib/alec/
 */

#ifndef ALEC_H
#define ALEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Result codes                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    ALEC_OK = 0,
    ALEC_ERR_INVALID_INPUT = 1,
    ALEC_ERR_BUFFER_TOO_SMALL = 2,
    ALEC_ERR_ENCODE_FAILED = 3,
    ALEC_ERR_DECODE_FAILED = 4,
    ALEC_ERR_NULL_POINTER = 5,
    ALEC_ERR_INVALID_UTF8 = 6,
    ALEC_ERR_IO_ERROR = 7,
    ALEC_ERR_VERSION_MISMATCH = 8,
} AlecResult;

/* ------------------------------------------------------------------ */
/*  Opaque handles                                                     */
/* ------------------------------------------------------------------ */

typedef struct AlecEncoder AlecEncoder;
typedef struct AlecDecoder AlecDecoder;

/* ------------------------------------------------------------------ */
/*  Encoder                                                            */
/* ------------------------------------------------------------------ */

AlecEncoder *alec_encoder_new(void);
void         alec_encoder_free(AlecEncoder *encoder);

/**
 * Encode a single floating-point sensor value.
 */
AlecResult alec_encode_value(
    AlecEncoder *encoder,
    float        value,
    uint8_t     *out_buf,
    uint8_t     *out_bits
);

/**
 * Encode multiple floating-point sensor values into a compressed buffer.
 *
 * @param encoder   Encoder handle (owns internal Context)
 * @param values    Array of float sensor values
 * @param count     Number of values in the array
 * @param out_buf   Buffer for compressed output
 * @param cap       Size of out_buf
 * @param out_len   [out] Actual compressed length written
 * @return ALEC_OK on success
 */
AlecResult alec_encode_multi(
    AlecEncoder *encoder,
    const float *values,
    size_t       count,
    uint8_t     *out_buf,
    size_t       cap,
    size_t      *out_len
);

/* ------------------------------------------------------------------ */
/*  Decoder                                                            */
/* ------------------------------------------------------------------ */

AlecDecoder *alec_decoder_new(void);
void         alec_decoder_free(AlecDecoder *decoder);

AlecResult alec_decode_value(AlecDecoder *decoder);

AlecResult alec_decode_multi(AlecDecoder *decoder);

/* ------------------------------------------------------------------ */
/*  Utility                                                            */
/* ------------------------------------------------------------------ */

const char *alec_version(void);
const char *alec_result_to_string(AlecResult result);

/**
 * Initialize the ALEC internal heap.
 * Must be called once before any other ALEC function on bare-metal targets.
 * Added in alec-ffi 1.2.1.
 */
void alec_heap_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ALEC_H */
