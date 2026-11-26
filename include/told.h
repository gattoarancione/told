/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Claudio Russo (gattoarancione) */

/*
 * TOLD (Typo Lenght Data) binary streaming protocol, v1.0 -- C interface.
 *
 * Hand-written and kept in step with src/told_c.zig by hand, because Zig no longer emits
 * C headers. If you add a symbol there, add it here.
 *
 * Decoding is zero-copy: a decoded frame's `data` and `raw` pointers borrow the buffer
 * you passed in, so they stay valid exactly as long as that buffer does. Nothing is
 * copied in either direction and the library allocates nothing while decoding.
 *
 * Thread safety: the decoder is pure and re-entrant. An encoder handle is not -- give
 * each thread its own.
 */
#ifndef TOLD_H
#define TOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type nibble; see the specification's section 3. */
typedef enum {
    TOLD_NULL     = 0x0,
    TOLD_BOOL     = 0x1,
    TOLD_TINYUINT = 0x2,
    TOLD_INT8     = 0x3,
    TOLD_INT16    = 0x4,
    TOLD_INT32    = 0x5,
    TOLD_INT64    = 0x6,
    TOLD_FLOAT32  = 0x7,
    TOLD_FLOAT64  = 0x8,
    TOLD_STRING   = 0x9,
    TOLD_ARRAY    = 0xA,
    TOLD_OBJECT   = 0xB,
    TOLD_BUFFER   = 0xC, /* raw opaque bytes: the default when nothing else fits */
    TOLD_EXTENDED = 0xF
} told_type;

/* Values are part of the ABI: appended to, never renumbered. */
typedef enum {
    TOLD_OK                        = 0,
    TOLD_TRUNCATED                 = 1,
    TOLD_INVALID_MOD               = 2,
    TOLD_RESERVED_TYPE             = 3,
    TOLD_MALFORMED_CONTAINER       = 4,
    TOLD_INVALID_UTF8              = 5,
    TOLD_LENGTH_OVERFLOW           = 6,
    TOLD_DEPTH_EXCEEDED            = 7,
    TOLD_TYPE_MISMATCH             = 8,
    TOLD_EXTENSION_FRAMING_UNKNOWN = 9,
    TOLD_OUT_OF_MEMORY             = 10,
    /* told_frame_size only: too few bytes to tell yet -- read more. */
    TOLD_INCOMPLETE                = 11,
    TOLD_INVALID_ARGUMENT          = 12,
    TOLD_UNEXPECTED                = 13,
    /* told_read_frame only: the stream ended cleanly, between frames. */
    TOLD_END_OF_STREAM             = 14,
    /* told_read_frame only: the read callback returned a negative value. */
    TOLD_READ_FAILED               = 15,
    /* told_read_frame only: the frame does not fit the buffer. */
    TOLD_FRAME_TOO_LARGE           = 16,
    /* Struct mapping: the value does not fit the member's type. */
    TOLD_OUT_OF_RANGE              = 17
} told_status;

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
} told_frame;

/*
 * The layout above is an ABI, not an implementation detail: the Zig side declares the same
 * struct in src/told_abi.zig and hands these out by value across the C boundary. Before
 * these assertions the agreement was a comment reading "must match", and nothing checked
 * it -- a field reordered on one side would have been a callback reading a length as a
 * pointer, at runtime, silently.
 *
 * Written against sizeof(void *) rather than as literals so they hold on a 32-bit target
 * too, where the struct is 20 bytes rather than 40. src/told_abi.zig asserts exactly these.
 *
 * If one of these fires, do not "fix" it by editing the number: the two declarations have
 * drifted, and one of them is now wrong about what the other sends.
 */
#if defined(__cplusplus)
#  define TOLD_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define TOLD_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
/* Pre-C11 has no compile-time assertion worth using; the Zig side still checks its half. */
#  define TOLD_ABI_ASSERT(cond, msg)
#endif

TOLD_ABI_ASSERT(offsetof(told_frame, type) == 0, "told_frame.type moved");
TOLD_ABI_ASSERT(offsetof(told_frame, mod) == 1, "told_frame.mod moved");
TOLD_ABI_ASSERT(offsetof(told_frame, ext_code) == 2, "told_frame.ext_code moved");
TOLD_ABI_ASSERT(offsetof(told_frame, data) == sizeof(void *),
                    "told_frame.data is not one pointer in");
TOLD_ABI_ASSERT(offsetof(told_frame, data_len) == 2 * sizeof(void *),
                    "told_frame.data_len moved");
TOLD_ABI_ASSERT(offsetof(told_frame, raw) == 3 * sizeof(void *),
                    "told_frame.raw moved");
TOLD_ABI_ASSERT(offsetof(told_frame, raw_len) == 4 * sizeof(void *),
                    "told_frame.raw_len moved");
TOLD_ABI_ASSERT(sizeof(told_frame) == 5 * sizeof(void *),
                    "told_frame changed size");
/* size_t and a pointer must be the same width, or every length above is misread. */
TOLD_ABI_ASSERT(sizeof(size_t) == sizeof(void *),
                    "size_t and void* differ in width on this target");

/* Cursor over a container's unconsumed bytes. Treat the fields as private. */
typedef struct {
    const uint8_t *rest;
    size_t         rest_len;
} told_iter;

typedef struct told_encoder told_encoder;

/* --- decoding ---------------------------------------------------------------- */

/* Decode the frame at `bytes`. `len` may exceed the frame; out->raw_len says where it
 * ends, which is how back-to-back stream frames are walked. Validates only the outermost
 * frame -- see told_validate. */
told_status told_decode(const uint8_t *bytes, size_t len, told_frame *out);

/* Total size of the frame at `bytes`, or TOLD_INCOMPLETE if the prefix is too short
 * to say. At most 9 bytes are needed to decide, which is what lets a reader buffer
 * exactly one frame off a socket without parsing it. */
told_status told_frame_size(const uint8_t *bytes, size_t len, size_t *out);

/* Walk the whole tree: nested frames, UTF-8 in strings, object shape. Use on untrusted
 * input; `max_depth` bounds recursion. */
told_status told_validate(const uint8_t *bytes, size_t len, size_t max_depth);

/* --- stream reading ---------------------------------------------------------- */

/* Fill up to `len` bytes of `dst` from the stream behind `ctx`. Return the number of
 * bytes read -- fewer than `len` is fine, the caller asks again -- 0 at end of stream,
 * or a negative value on error. Retrying EINTR is the callback's business, and so is
 * keeping errno: stash it in `ctx` if the caller needs it. */
typedef ptrdiff_t (*told_read_fn)(void *ctx, uint8_t *dst, size_t len);

/* Read exactly one frame into `buf` (at most `cap` bytes) and decode it into `out`, which
 * then borrows `buf`. No byte past the frame is requested, so the next call starts on the
 * next header and anything else sharing the stream is left where it was.
 *
 *   TOLD_END_OF_STREAM    the stream ended before a header byte: the clean end
 *   TOLD_TRUNCATED        it ended mid-frame
 *   TOLD_READ_FAILED      the callback returned a negative value
 *   TOLD_FRAME_TOO_LARGE  the frame needs more than `cap` bytes
 *
 * After the last three the stream is mid-frame and cannot be resynchronised: close it.
 * Validates only the outermost frame, as told_decode does -- see told_validate. EXTENDED
 * frames are refused with TOLD_EXTENSION_FRAMING_UNKNOWN, since their length is the
 * extension's business. */
told_status told_read_frame(told_read_fn read, void *ctx, uint8_t *buf, size_t cap,
                            told_frame *out);

/* --- accessors --------------------------------------------------------------- */

told_status told_as_bool(const told_frame *frame, bool *out);
told_status told_as_i64(const told_frame *frame, int64_t *out);
told_status told_as_u64(const told_frame *frame, uint64_t *out);
told_status told_as_f64(const told_frame *frame, double *out);
/* UTF-8 validated. Borrows the input buffer. */
told_status told_as_string(const told_frame *frame, const uint8_t **ptr, size_t *len);
/* Opaque bytes, no encoding contract. Borrows the input buffer. */
told_status told_as_buffer(const told_frame *frame, const uint8_t **ptr, size_t *len);

/* --- iteration --------------------------------------------------------------- */

told_status told_array_iter(const told_frame *frame, told_iter *out);
told_status told_object_iter(const told_frame *frame, told_iter *out);

/* `*have` false means the end was reached; a non-OK status means malformed input. */
told_status told_array_next(told_iter *iter, told_frame *out, bool *have);
told_status told_object_next(told_iter *iter, const uint8_t **key, size_t *key_len,
                                     told_frame *value, bool *have);

/* --- encoding ---------------------------------------------------------------- */

/* NULL on allocation failure. */
told_encoder *told_encoder_create(void);
/* Accepts NULL, like free(). */
void told_encoder_destroy(told_encoder *enc);
/* Drop the contents, keep the capacity, for reusing one encoder per message. */
void told_encoder_reset(told_encoder *enc);
/* The encoded bytes. Borrowed: copy them before the next write, which may reallocate. */
const uint8_t *told_encoder_bytes(const told_encoder *enc, size_t *len);

told_status told_write_null(told_encoder *enc);
told_status told_write_bool(told_encoder *enc, bool value);
/* TINYUINT for 0..15, else the shortest INT* that fits. Above INT64_MAX it goes out as a
 * full-width INT64 to be read back as unsigned: the spec defines no UINT family. */
told_status told_write_u64(told_encoder *enc, uint64_t value);
told_status told_write_i64(told_encoder *enc, int64_t value);
told_status told_write_f32(told_encoder *enc, float value);
told_status told_write_f64(told_encoder *enc, double value);
/* Fails with TOLD_INVALID_UTF8 rather than emitting a STRING a decoder may refuse. */
told_status told_write_string(told_encoder *enc, const uint8_t *bytes, size_t len);
told_status told_write_buffer(told_encoder *enc, const uint8_t *bytes, size_t len);
told_status told_write_extended(told_encoder *enc, uint8_t code,
                                        const uint8_t *bytes, size_t len);

/* A container's length is only known once its children are written, so its header is
 * inserted on close. Pass the mark from begin_* to the matching end_*. */
told_status told_begin_array(told_encoder *enc, size_t *mark);
told_status told_begin_object(told_encoder *enc, size_t *mark);
told_status told_end_array(told_encoder *enc, size_t mark);
told_status told_end_object(told_encoder *enc, size_t mark);

/* --- diagnostics ------------------------------------------------------------- */

const char *told_status_name(told_status status);
const char *told_version(void);

/* ---------------------------------------------------------------- struct mapping
 *
 * C has no templates and no reflection, so a struct describes itself once as a table of
 * fields and the two calls below walk it. Same idea as TOLD_DEFINE_MAP in C++ and the
 * comptime mapping in Zig, expressed the only way C allows: offsetof, a kind, a size.
 *
 *     typedef struct { uint8_t channel; double value; int valid; } reading;
 *
 *     static const told_field reading_fields[] = {
 *         TOLD_FIELD(reading, channel, TOLD_F_U8),
 *         TOLD_FIELD(reading, value,   TOLD_F_F64),
 *         TOLD_FIELD(reading, valid,   TOLD_F_I32),
 *     };
 *     static const told_desc reading_desc = TOLD_DESC("reading", reading_fields, 1);
 *
 *     told_pack_struct(enc, &reading_desc, &value);
 *     told_unpack_struct_bytes(bytes, len, &reading_desc, &out);
 *
 * The `named` flag on the descriptor picks the shape: 1 for an object keyed by field name
 * (tolerant of fields being added or reordered), 0 for a positional array (smaller, and
 * both ends must agree on the order).
 *
 * What C cannot express, and therefore is not supported: dynamically sized members. A
 * string is a fixed char[N], packed to the first NUL and unpacked NUL-terminated and
 * truncated to fit; a byte buffer is a fixed array plus a length member; an array is
 * fixed-length. Anything genuinely dynamic belongs to the encoder and iterator API
 * directly, which has no such limits.
 *
 * On unpack, a field whose key the frame does not carry is left untouched -- zero the
 * struct first if that matters -- and a key matching no field is ignored, so a producer
 * may add one without breaking consumers built earlier.
 */

typedef enum {
    TOLD_F_BOOL   = 0,
    TOLD_F_I8     = 1,
    TOLD_F_I16    = 2,
    TOLD_F_I32    = 3,
    TOLD_F_I64    = 4,
    TOLD_F_U8     = 5,
    TOLD_F_U16    = 6,
    TOLD_F_U32    = 7,
    TOLD_F_U64    = 8,
    TOLD_F_F32    = 9,
    TOLD_F_F64    = 10,
    /* char[size], NUL-terminated on both sides. */
    TOLD_F_STRING = 11,
    /* uint8_t[size] with a size_t length member named by length_offset. */
    TOLD_F_BYTES  = 12,
    /* A nested struct, described by the `nested` descriptor. */
    TOLD_F_STRUCT = 13,
    /* A fixed-length array; see element_kind, element_size and count. */
    TOLD_F_ARRAY  = 14
} told_field_kind;

struct told_desc;

typedef struct told_field {
    const char *key;
    size_t offset;
    told_field_kind kind;
    size_t size;          /* buffer length for STRING and BYTES */
    size_t length_offset; /* BYTES: offset of the size_t holding the used length */
    const struct told_desc *nested; /* STRUCT, and ARRAY of STRUCT */
    told_field_kind element_kind;   /* ARRAY */
    size_t element_size;                /* ARRAY */
    size_t count;                       /* ARRAY */
} told_field;

typedef struct told_desc {
    const char *name;
    const told_field *fields;
    size_t field_count;
    int named; /* 1 = object keyed by name, 0 = positional array */
} told_desc;

/* A scalar member, keyed by its own name. */
#define TOLD_FIELD(Type, member, field_kind)                                          \
    { .key = #member, .offset = offsetof(Type, member), .kind = (field_kind),         \
      .size = sizeof(((Type *)0)->member) }

/* The same, with an explicit wire key -- the counterpart of TOLD_NVP in C++. */
#define TOLD_FIELD_AS(Type, member, field_kind, wire_key)                             \
    { .key = (wire_key), .offset = offsetof(Type, member), .kind = (field_kind),      \
      .size = sizeof(((Type *)0)->member) }

/* A fixed char[N] string. */
#define TOLD_FIELD_STRING(Type, member)                                               \
    { .key = #member, .offset = offsetof(Type, member), .kind = TOLD_F_STRING,        \
      .size = sizeof(((Type *)0)->member) }

/* A fixed uint8_t[N] buffer plus the size_t member holding how much of it is used. */
#define TOLD_FIELD_BYTES(Type, member, length_member)                                 \
    { .key = #member, .offset = offsetof(Type, member), .kind = TOLD_F_BYTES,         \
      .size = sizeof(((Type *)0)->member),                                            \
      .length_offset = offsetof(Type, length_member) }

/* A nested struct. */
#define TOLD_FIELD_STRUCT(Type, member, desc_ptr)                                     \
    { .key = #member, .offset = offsetof(Type, member), .kind = TOLD_F_STRUCT,        \
      .size = sizeof(((Type *)0)->member), .nested = (desc_ptr) }

/* A nested struct with an explicit wire key, for when the C member cannot be named after
 * the field -- a collision with a type name, or a key the other languages already fixed. */
#define TOLD_FIELD_AS_STRUCT(Type, member, desc_ptr, wire_key)                        \
    { .key = (wire_key), .offset = offsetof(Type, member), .kind = TOLD_F_STRUCT,     \
      .size = sizeof(((Type *)0)->member), .nested = (desc_ptr) }

/* A fixed-length array of a scalar kind. */
#define TOLD_FIELD_ARRAY(Type, member, elem_kind)                                     \
    { .key = #member, .offset = offsetof(Type, member), .kind = TOLD_F_ARRAY,         \
      .size = sizeof(((Type *)0)->member), .element_kind = (elem_kind),               \
      .element_size = sizeof(((Type *)0)->member[0]),                                 \
      .count = sizeof(((Type *)0)->member) / sizeof(((Type *)0)->member[0]) }

/* A fixed-length array of a nested struct. */
#define TOLD_FIELD_ARRAY_STRUCT(Type, member, desc_ptr)                               \
    { .key = #member, .offset = offsetof(Type, member), .kind = TOLD_F_ARRAY,         \
      .size = sizeof(((Type *)0)->member), .element_kind = TOLD_F_STRUCT,             \
      .nested = (desc_ptr),                                                           \
      .element_size = sizeof(((Type *)0)->member[0]),                                 \
      .count = sizeof(((Type *)0)->member) / sizeof(((Type *)0)->member[0]) }

#define TOLD_DESC(desc_name, field_array, is_named)                                   \
    { .name = (desc_name), .fields = (field_array),                                   \
      .field_count = sizeof(field_array) / sizeof((field_array)[0]), .named = (is_named) }

told_status told_pack_struct(told_encoder *enc,
                                     const told_desc *desc,
                                     const void *value);

told_status told_unpack_struct(const told_frame *frame,
                                       const told_desc *desc,
                                       void *out);

told_status told_unpack_struct_bytes(const uint8_t *bytes, size_t len,
                                             const told_desc *desc,
                                             void *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TOLD_H */
