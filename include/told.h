/*
 * TLD (Type-Length-Data) binary streaming protocol, v1.0 -- C interface.
 *
 * Hand-written and kept in step with lib/tld_c.zig by hand, because Zig no longer emits
 * C headers. If you add a symbol there, add it here.
 *
 * Decoding is zero-copy: a decoded frame's `data` and `raw` pointers borrow the buffer
 * you passed in, so they stay valid exactly as long as that buffer does. Nothing is
 * copied in either direction and the library allocates nothing while decoding.
 *
 * Thread safety: the decoder is pure and re-entrant. An encoder handle is not -- give
 * each thread its own.
 */
#ifndef QNNT_TLD_H
#define QNNT_TLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type nibble; see the specification's section 3. */
typedef enum {
    QNNT_TLD_NULL     = 0x0,
    QNNT_TLD_BOOL     = 0x1,
    QNNT_TLD_TINYUINT = 0x2,
    QNNT_TLD_INT8     = 0x3,
    QNNT_TLD_INT16    = 0x4,
    QNNT_TLD_INT32    = 0x5,
    QNNT_TLD_INT64    = 0x6,
    QNNT_TLD_FLOAT32  = 0x7,
    QNNT_TLD_FLOAT64  = 0x8,
    QNNT_TLD_STRING   = 0x9,
    QNNT_TLD_ARRAY    = 0xA,
    QNNT_TLD_OBJECT   = 0xB,
    QNNT_TLD_BUFFER   = 0xC, /* raw opaque bytes: the default when nothing else fits */
    QNNT_TLD_EXTENDED = 0xF
} qnnt_tld_type;

/* Values are part of the ABI: appended to, never renumbered. */
typedef enum {
    QNNT_TLD_OK                        = 0,
    QNNT_TLD_TRUNCATED                 = 1,
    QNNT_TLD_INVALID_MOD               = 2,
    QNNT_TLD_RESERVED_TYPE             = 3,
    QNNT_TLD_MALFORMED_CONTAINER       = 4,
    QNNT_TLD_INVALID_UTF8              = 5,
    QNNT_TLD_LENGTH_OVERFLOW           = 6,
    QNNT_TLD_DEPTH_EXCEEDED            = 7,
    QNNT_TLD_TYPE_MISMATCH             = 8,
    QNNT_TLD_EXTENSION_FRAMING_UNKNOWN = 9,
    QNNT_TLD_OUT_OF_MEMORY             = 10,
    /* qnnt_tld_frame_size only: too few bytes to tell yet -- read more. */
    QNNT_TLD_INCOMPLETE                = 11,
    QNNT_TLD_INVALID_ARGUMENT          = 12,
    QNNT_TLD_UNEXPECTED                = 13
} qnnt_tld_status;

/*
 * One decoded frame. Flat rather than opaque because the zero-copy decoder already
 * reduces a frame to a type, a mod and two borrowed spans.
 *
 *   data / data_len : the L payload bytes; empty for the inline family
 *   raw  / raw_len  : the whole frame, prefix included -- what to write to forward it
 *   ext_code        : extension type code, or -1 when not an EXTENDED frame
 */
typedef struct {
    uint8_t        type;
    uint8_t        mod;
    int16_t        ext_code;
    const uint8_t *data;
    size_t         data_len;
    const uint8_t *raw;
    size_t         raw_len;
} qnnt_tld_frame;

/* Cursor over a container's unconsumed bytes. Treat the fields as private. */
typedef struct {
    const uint8_t *rest;
    size_t         rest_len;
} qnnt_tld_iter;

typedef struct qnnt_tld_encoder qnnt_tld_encoder;

/* --- decoding ---------------------------------------------------------------- */

/* Decode the frame at `bytes`. `len` may exceed the frame; out->raw_len says where it
 * ends, which is how back-to-back stream frames are walked. Validates only the outermost
 * frame -- see qnnt_tld_validate. */
qnnt_tld_status qnnt_tld_decode(const uint8_t *bytes, size_t len, qnnt_tld_frame *out);

/* Total size of the frame at `bytes`, or QNNT_TLD_INCOMPLETE if the prefix is too short
 * to say. At most 9 bytes are needed to decide, which is what lets a reader buffer
 * exactly one frame off a socket without parsing it. */
qnnt_tld_status qnnt_tld_frame_size(const uint8_t *bytes, size_t len, size_t *out);

/* Walk the whole tree: nested frames, UTF-8 in strings, object shape. Use on untrusted
 * input; `max_depth` bounds recursion. */
qnnt_tld_status qnnt_tld_validate(const uint8_t *bytes, size_t len, size_t max_depth);

/* --- accessors --------------------------------------------------------------- */

qnnt_tld_status qnnt_tld_as_bool(const qnnt_tld_frame *frame, bool *out);
qnnt_tld_status qnnt_tld_as_i64(const qnnt_tld_frame *frame, int64_t *out);
qnnt_tld_status qnnt_tld_as_u64(const qnnt_tld_frame *frame, uint64_t *out);
qnnt_tld_status qnnt_tld_as_f64(const qnnt_tld_frame *frame, double *out);
/* UTF-8 validated. Borrows the input buffer. */
qnnt_tld_status qnnt_tld_as_string(const qnnt_tld_frame *frame, const uint8_t **ptr, size_t *len);
/* Opaque bytes, no encoding contract. Borrows the input buffer. */
qnnt_tld_status qnnt_tld_as_buffer(const qnnt_tld_frame *frame, const uint8_t **ptr, size_t *len);

/* --- iteration --------------------------------------------------------------- */

qnnt_tld_status qnnt_tld_array_iter(const qnnt_tld_frame *frame, qnnt_tld_iter *out);
qnnt_tld_status qnnt_tld_object_iter(const qnnt_tld_frame *frame, qnnt_tld_iter *out);

/* `*have` false means the end was reached; a non-OK status means malformed input. */
qnnt_tld_status qnnt_tld_array_next(qnnt_tld_iter *iter, qnnt_tld_frame *out, bool *have);
qnnt_tld_status qnnt_tld_object_next(qnnt_tld_iter *iter, const uint8_t **key, size_t *key_len,
                                     qnnt_tld_frame *value, bool *have);

/* --- encoding ---------------------------------------------------------------- */

/* NULL on allocation failure. */
qnnt_tld_encoder *qnnt_tld_encoder_create(void);
/* Accepts NULL, like free(). */
void qnnt_tld_encoder_destroy(qnnt_tld_encoder *enc);
/* Drop the contents, keep the capacity, for reusing one encoder per message. */
void qnnt_tld_encoder_reset(qnnt_tld_encoder *enc);
/* The encoded bytes. Borrowed: copy them before the next write, which may reallocate. */
const uint8_t *qnnt_tld_encoder_bytes(const qnnt_tld_encoder *enc, size_t *len);

qnnt_tld_status qnnt_tld_write_null(qnnt_tld_encoder *enc);
qnnt_tld_status qnnt_tld_write_bool(qnnt_tld_encoder *enc, bool value);
/* TINYUINT for 0..15, else the shortest INT* that fits. Above INT64_MAX it goes out as a
 * full-width INT64 to be read back as unsigned: the spec defines no UINT family. */
qnnt_tld_status qnnt_tld_write_u64(qnnt_tld_encoder *enc, uint64_t value);
qnnt_tld_status qnnt_tld_write_i64(qnnt_tld_encoder *enc, int64_t value);
qnnt_tld_status qnnt_tld_write_f32(qnnt_tld_encoder *enc, float value);
qnnt_tld_status qnnt_tld_write_f64(qnnt_tld_encoder *enc, double value);
/* Fails with QNNT_TLD_INVALID_UTF8 rather than emitting a STRING a decoder may refuse. */
qnnt_tld_status qnnt_tld_write_string(qnnt_tld_encoder *enc, const uint8_t *bytes, size_t len);
qnnt_tld_status qnnt_tld_write_buffer(qnnt_tld_encoder *enc, const uint8_t *bytes, size_t len);
qnnt_tld_status qnnt_tld_write_extended(qnnt_tld_encoder *enc, uint8_t code,
                                        const uint8_t *bytes, size_t len);

/* A container's length is only known once its children are written, so its header is
 * inserted on close. Pass the mark from begin_* to the matching end_*. */
qnnt_tld_status qnnt_tld_begin_array(qnnt_tld_encoder *enc, size_t *mark);
qnnt_tld_status qnnt_tld_begin_object(qnnt_tld_encoder *enc, size_t *mark);
qnnt_tld_status qnnt_tld_end_array(qnnt_tld_encoder *enc, size_t mark);
qnnt_tld_status qnnt_tld_end_object(qnnt_tld_encoder *enc, size_t mark);

/* --- diagnostics ------------------------------------------------------------- */

const char *qnnt_tld_status_name(qnnt_tld_status status);
const char *qnnt_tld_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QNNT_TLD_H */
