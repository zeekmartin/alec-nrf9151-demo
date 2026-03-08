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
AlecEncoder *alec_encoder_new_with_checksum(void);
void         alec_encoder_free(AlecEncoder *encoder);

/**
 * Compress raw bytes.
 *
 * Matches Rust API: encoder.encode(&raw_bytes, &mut ctx) → Vec<u8>
 *
 * @param encoder       Encoder handle (owns internal Context)
 * @param input         Raw bytes to compress
 * @param input_len     Length of input
 * @param output        Buffer for compressed output
 * @param output_capacity  Size of output buffer
 * @param output_len    [out] Actual compressed length written
 * @return ALEC_OK on success
 */
AlecResult alec_encode(
    AlecEncoder   *encoder,
    const uint8_t *input,
    size_t         input_len,
    uint8_t       *output,
    size_t         output_capacity,
    size_t        *output_len
);

/**
 * Encode a single floating-point sensor value.
 */
AlecResult alec_encode_value(
    AlecEncoder *encoder,
    double       value,
    uint64_t     timestamp,
    const char  *source_id,
    uint8_t     *output,
    size_t       output_capacity,
    size_t      *output_len
);

/**
 * Encode multiple floating-point sensor values.
 */
AlecResult alec_encode_multi(
    AlecEncoder   *encoder,
    const double  *values,
    size_t         value_count,
    uint64_t       timestamp,
    const char    *source_id,
    uint8_t       *output,
    size_t         output_capacity,
    size_t        *output_len
);

/* ------------------------------------------------------------------ */
/*  Decoder                                                            */
/* ------------------------------------------------------------------ */

AlecDecoder *alec_decoder_new(void);
AlecDecoder *alec_decoder_new_with_checksum(void);
void         alec_decoder_free(AlecDecoder *decoder);

/**
 * Decompress ALEC-encoded bytes.
 *
 * Matches Rust API: decoder.decode(&compressed, &mut ctx) → Vec<u8>
 */
AlecResult alec_decode(
    AlecDecoder   *decoder,
    const uint8_t *input,
    size_t         input_len,
    uint8_t       *output,
    size_t         output_capacity,
    size_t        *output_len
);

AlecResult alec_decode_value(
    AlecDecoder   *decoder,
    const uint8_t *input,
    size_t         input_len,
    double        *value,
    uint64_t      *timestamp
);

AlecResult alec_decode_multi(
    AlecDecoder   *decoder,
    const uint8_t *input,
    size_t         input_len,
    double        *values,
    size_t         values_capacity,
    size_t        *values_count
);

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
