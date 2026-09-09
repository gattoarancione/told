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

/*
 * The layout above is an ABI, not an implementation detail: the Zig side declares the same
 * struct in lib/tld_abi.zig and hands these out by value across the C boundary. Before
 * these assertions the agreement was a comment reading "must match", and nothing checked
 * it -- a field reordered on one side would have been a callback reading a length as a
 * pointer, at runtime, silently.
 *
 * Written against sizeof(void *) rather than as literals so they hold on a 32-bit target
 * too, where the struct is 20 bytes rather than 40. lib/tld_abi.zig asserts exactly these.
 *
 * If one of these fires, do not "fix" it by editing the number: the two declarations have
 * drifted, and one of them is now wrong about what the other sends.
 */
#if defined(__cplusplus)
#  define QNNT_TLD_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define QNNT_TLD_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
/* Pre-C11 has no compile-time assertion worth using; the Zig side still checks its half. */
#  define QNNT_TLD_ABI_ASSERT(cond, msg)
#endif

QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, type) == 0, "qnnt_tld_frame.type moved");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, mod) == 1, "qnnt_tld_frame.mod moved");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, ext_code) == 2, "qnnt_tld_frame.ext_code moved");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, data) == sizeof(void *),
                    "qnnt_tld_frame.data is not one pointer in");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, data_len) == 2 * sizeof(void *),
                    "qnnt_tld_frame.data_len moved");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, raw) == 3 * sizeof(void *),
                    "qnnt_tld_frame.raw moved");
QNNT_TLD_ABI_ASSERT(offsetof(qnnt_tld_frame, raw_len) == 4 * sizeof(void *),
                    "qnnt_tld_frame.raw_len moved");
QNNT_TLD_ABI_ASSERT(sizeof(qnnt_tld_frame) == 5 * sizeof(void *),
                    "qnnt_tld_frame changed size");
/* size_t and a pointer must be the same width, or every length above is misread. */
QNNT_TLD_ABI_ASSERT(sizeof(size_t) == sizeof(void *),
                    "size_t and void* differ in width on this target");

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

/* ---------------------------------------------------------------- struct mapping
 *
 * C has no templates and no reflection, so a struct describes itself once as a table of
 * fields and the two calls below walk it. Same idea as TLD_DEFINE_MAP in C++ and the
 * comptime mapping in Zig, expressed the only way C allows: offsetof, a kind, a size.
 *
 *     typedef struct { uint8_t channel; double value; int valid; } reading;
 *
 *     static const qnnt_tld_field reading_fields[] = {
 *         QNNT_TLD_FIELD(reading, channel, QNNT_TLD_F_U8),
 *         QNNT_TLD_FIELD(reading, value,   QNNT_TLD_F_F64),
 *         QNNT_TLD_FIELD(reading, valid,   QNNT_TLD_F_I32),
 *     };
 *     static const qnnt_tld_desc reading_desc = QNNT_TLD_DESC("reading", reading_fields, 1);
 *
 *     qnnt_tld_pack_struct(enc, &reading_desc, &value);
 *     qnnt_tld_unpack_struct_bytes(bytes, len, &reading_desc, &out);
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
    QNNT_TLD_F_BOOL   = 0,
    QNNT_TLD_F_I8     = 1,
    QNNT_TLD_F_I16    = 2,
    QNNT_TLD_F_I32    = 3,
    QNNT_TLD_F_I64    = 4,
    QNNT_TLD_F_U8     = 5,
    QNNT_TLD_F_U16    = 6,
    QNNT_TLD_F_U32    = 7,
    QNNT_TLD_F_U64    = 8,
    QNNT_TLD_F_F32    = 9,
    QNNT_TLD_F_F64    = 10,
    /* char[size], NUL-terminated on both sides. */
    QNNT_TLD_F_STRING = 11,
    /* uint8_t[size] with a size_t length member named by length_offset. */
    QNNT_TLD_F_BYTES  = 12,
    /* A nested struct, described by the `nested` descriptor. */
    QNNT_TLD_F_STRUCT = 13,
    /* A fixed-length array; see element_kind, element_size and count. */
    QNNT_TLD_F_ARRAY  = 14
} qnnt_tld_field_kind;

struct qnnt_tld_desc;

typedef struct qnnt_tld_field {
    const char *key;
    size_t offset;
    qnnt_tld_field_kind kind;
    size_t size;          /* buffer length for STRING and BYTES */
    size_t length_offset; /* BYTES: offset of the size_t holding the used length */
    const struct qnnt_tld_desc *nested; /* STRUCT, and ARRAY of STRUCT */
    qnnt_tld_field_kind element_kind;   /* ARRAY */
    size_t element_size;                /* ARRAY */
    size_t count;                       /* ARRAY */
} qnnt_tld_field;

typedef struct qnnt_tld_desc {
    const char *name;
    const qnnt_tld_field *fields;
    size_t field_count;
    int named; /* 1 = object keyed by name, 0 = positional array */
} qnnt_tld_desc;

/* A scalar member, keyed by its own name. */
#define QNNT_TLD_FIELD(Type, member, field_kind)                                          \
    { .key = #member, .offset = offsetof(Type, member), .kind = (field_kind),              \
      .size = sizeof(((Type *)0)->member) }

/* The same, with an explicit wire key -- the counterpart of TLD_NVP in C++. */
#define QNNT_TLD_FIELD_AS(Type, member, field_kind, wire_key)                             \
    { .key = (wire_key), .offset = offsetof(Type, member), .kind = (field_kind),           \
      .size = sizeof(((Type *)0)->member) }

/* A fixed char[N] string. */
#define QNNT_TLD_FIELD_STRING(Type, member)                                               \
    { .key = #member, .offset = offsetof(Type, member), .kind = QNNT_TLD_F_STRING,         \
      .size = sizeof(((Type *)0)->member) }

/* A fixed uint8_t[N] buffer plus the size_t member holding how much of it is used. */
#define QNNT_TLD_FIELD_BYTES(Type, member, length_member)                                 \
    { .key = #member, .offset = offsetof(Type, member), .kind = QNNT_TLD_F_BYTES,          \
      .size = sizeof(((Type *)0)->member),                                                 \
      .length_offset = offsetof(Type, length_member) }

/* A nested struct. */
#define QNNT_TLD_FIELD_STRUCT(Type, member, desc_ptr)                                     \
    { .key = #member, .offset = offsetof(Type, member), .kind = QNNT_TLD_F_STRUCT,         \
      .size = sizeof(((Type *)0)->member), .nested = (desc_ptr) }

/* A nested struct with an explicit wire key, for when the C member cannot be named after
 * the field -- a collision with a type name, or a key the other languages already fixed. */
#define QNNT_TLD_FIELD_AS_STRUCT(Type, member, desc_ptr, wire_key)                        \
    { .key = (wire_key), .offset = offsetof(Type, member), .kind = QNNT_TLD_F_STRUCT,      \
      .size = sizeof(((Type *)0)->member), .nested = (desc_ptr) }

/* A fixed-length array of a scalar kind. */
#define QNNT_TLD_FIELD_ARRAY(Type, member, elem_kind)                                     \
    { .key = #member, .offset = offsetof(Type, member), .kind = QNNT_TLD_F_ARRAY,          \
      .size = sizeof(((Type *)0)->member), .element_kind = (elem_kind),                    \
      .element_size = sizeof(((Type *)0)->member[0]),                                      \
      .count = sizeof(((Type *)0)->member) / sizeof(((Type *)0)->member[0]) }

/* A fixed-length array of a nested struct. */
#define QNNT_TLD_FIELD_ARRAY_STRUCT(Type, member, desc_ptr)                               \
    { .key = #member, .offset = offsetof(Type, member), .kind = QNNT_TLD_F_ARRAY,          \
      .size = sizeof(((Type *)0)->member), .element_kind = QNNT_TLD_F_STRUCT,               \
      .nested = (desc_ptr),                                                                \
      .element_size = sizeof(((Type *)0)->member[0]),                                      \
      .count = sizeof(((Type *)0)->member) / sizeof(((Type *)0)->member[0]) }

#define QNNT_TLD_DESC(desc_name, field_array, is_named)                                   \
    { .name = (desc_name), .fields = (field_array),                                        \
      .field_count = sizeof(field_array) / sizeof((field_array)[0]), .named = (is_named) }

qnnt_tld_status qnnt_tld_pack_struct(qnnt_tld_encoder *enc,
                                     const qnnt_tld_desc *desc,
                                     const void *value);

qnnt_tld_status qnnt_tld_unpack_struct(const qnnt_tld_frame *frame,
                                       const qnnt_tld_desc *desc,
                                       void *out);

qnnt_tld_status qnnt_tld_unpack_struct_bytes(const uint8_t *bytes, size_t len,
                                             const qnnt_tld_desc *desc,
                                             void *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QNNT_TLD_H */
