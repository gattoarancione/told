/*
 * Struct mapping in C: a descriptor table instead of templates, and a check that this side
 * agrees with the Zig, C++ and Python mappings on the same golden frame.
 *
 *   zig build tld-map-c
 *
 * The schema is the one in lib/testdata/interop.tld. C cannot hold the dynamic parts of it
 * -- a std::vector of readings, an optional string -- so this uses the fixed-size
 * equivalents: an array of two readings, and a note that is simply absent from the frame.
 */

#include "qnnt_tld.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t channel;
    double value;
    bool valid;
} reading;

static const qnnt_tld_field reading_fields[] = {
    QNNT_TLD_FIELD(reading, channel, QNNT_TLD_F_U8),
    QNNT_TLD_FIELD(reading, value, QNNT_TLD_F_F64),
    QNNT_TLD_FIELD(reading, valid, QNNT_TLD_F_BOOL),
};
static const qnnt_tld_desc reading_desc =
    QNNT_TLD_DESC("reading", reading_fields, 1);

typedef struct {
    double x;
    double y;
} point;

static const qnnt_tld_field point_fields[] = {
    QNNT_TLD_FIELD(point, x, QNNT_TLD_F_F64),
    QNNT_TLD_FIELD(point, y, QNNT_TLD_F_F64),
};
static const qnnt_tld_desc point_desc = QNNT_TLD_DESC("point", point_fields, 1);

typedef struct {
    uint64_t id;
    char name[32];
    int32_t mode;
    reading readings[2];
    uint8_t raw[8];
    size_t raw_len;
    point origin;
} sample;

static const qnnt_tld_field sample_fields[] = {
    QNNT_TLD_FIELD(sample, id, QNNT_TLD_F_U64),
    QNNT_TLD_FIELD_STRING(sample, name),
    QNNT_TLD_FIELD(sample, mode, QNNT_TLD_F_I32),
    QNNT_TLD_FIELD_ARRAY_STRUCT(sample, readings, &reading_desc),
    QNNT_TLD_FIELD_BYTES(sample, raw, raw_len),
    QNNT_TLD_FIELD_STRUCT(sample, origin, &point_desc),
};
static const qnnt_tld_desc sample_desc = QNNT_TLD_DESC("sample", sample_fields, 1);

/* Short wire keys, the C counterpart of TLD_NVP. */
typedef struct {
    uint8_t channel;
    double value;
} terse;

static const qnnt_tld_field terse_fields[] = {
    QNNT_TLD_FIELD_AS(terse, channel, QNNT_TLD_F_U8, "ch"),
    QNNT_TLD_FIELD_AS(terse, value, QNNT_TLD_F_F64, "v"),
};
static const qnnt_tld_desc terse_desc = QNNT_TLD_DESC("terse", terse_fields, 1);

/* Positional: no keys on the wire at all. */
static const qnnt_tld_desc point_array_desc =
    QNNT_TLD_DESC("point", point_fields, 0);

static sample canonical(void) {
    sample s;
    memset(&s, 0, sizeof s);
    s.id = 99;
    snprintf(s.name, sizeof s.name, "sensor-a");
    s.mode = 2; /* faulted */
    s.readings[0] = (reading){1, 3.5, true};
    s.readings[1] = (reading){2, -0.25, false};
    s.raw[0] = 0x00;
    s.raw[1] = 0xFF;
    s.raw[2] = 0x0A;
    s.raw_len = 3;
    s.origin = (point){1.5, -2.5};
    return s;
}

int main(void) {
    qnnt_tld_encoder *enc = qnnt_tld_encoder_create();
    assert(enc != NULL);

    /* 1. round trip through the descriptor */
    const sample sent = canonical();
    assert(qnnt_tld_pack_struct(enc, &sample_desc, &sent) == QNNT_TLD_OK);

    size_t len = 0;
    const uint8_t *bytes = qnnt_tld_encoder_bytes(enc, &len);
    assert(bytes != NULL);

    sample got;
    memset(&got, 0, sizeof got);
    assert(qnnt_tld_unpack_struct_bytes(bytes, len, &sample_desc, &got) == QNNT_TLD_OK);

    assert(got.id == 99);
    assert(strcmp(got.name, "sensor-a") == 0);
    assert(got.mode == 2);
    assert(got.readings[0].channel == 1 && got.readings[0].value == 3.5);
    assert(got.readings[0].valid && !got.readings[1].valid);
    assert(got.raw_len == 3 && got.raw[1] == 0xFF);
    assert(got.origin.x == 1.5 && got.origin.y == -2.5);
    printf("  round trip: %zu bytes\n", len);

    /* 2. short keys, and the positional form */
    qnnt_tld_encoder_reset(enc);
    const terse t = {3, 9.5};
    assert(qnnt_tld_pack_struct(enc, &terse_desc, &t) == QNNT_TLD_OK);
    size_t terse_len = 0;
    (void)qnnt_tld_encoder_bytes(enc, &terse_len);

    qnnt_tld_encoder_reset(enc);
    const point p = {1.0, 2.0};
    assert(qnnt_tld_pack_struct(enc, &point_desc, &p) == QNNT_TLD_OK);
    size_t named_len = 0;
    (void)qnnt_tld_encoder_bytes(enc, &named_len);

    qnnt_tld_encoder_reset(enc);
    assert(qnnt_tld_pack_struct(enc, &point_array_desc, &p) == QNNT_TLD_OK);
    size_t positional_len = 0;
    const uint8_t *positional = qnnt_tld_encoder_bytes(enc, &positional_len);
    assert(positional_len < named_len);
    printf("  point: %zu bytes named, %zu positional\n", named_len, positional_len);

    point back = {0, 0};
    assert(qnnt_tld_unpack_struct_bytes(positional, positional_len, &point_array_desc,
                                        &back) == QNNT_TLD_OK);
    assert(back.x == 1.0 && back.y == 2.0);

    /* 3. the golden frame written by the Zig mapping */
    FILE *f = fopen("lib/testdata/interop.tld", "rb");
    if (f == NULL) {
        printf("  golden frame not found (run from the project root) -- skipped\n");
        qnnt_tld_encoder_destroy(enc);
        return 0;
    }
    static uint8_t golden[4096];
    const size_t golden_len = fread(golden, 1, sizeof golden, f);
    fclose(f);

    sample from_zig;
    memset(&from_zig, 0, sizeof from_zig);
    assert(qnnt_tld_unpack_struct_bytes(golden, golden_len, &sample_desc, &from_zig) ==
           QNNT_TLD_OK);
    assert(from_zig.id == 99);
    assert(strcmp(from_zig.name, "sensor-a") == 0);
    assert(from_zig.mode == 2);
    assert(from_zig.readings[1].value == -0.25 && !from_zig.readings[1].valid);
    assert(from_zig.raw_len == 3 && from_zig.raw[1] == 0xFF);
    assert(from_zig.origin.y == -2.5);
    printf("  golden frame from Zig decoded through the descriptor table\n");

    /* The `note` field is null in the golden frame and has no C counterpart here; the
     * descriptor simply does not mention it, which is the forward-compatibility rule
     * doing its job rather than an omission. */

    qnnt_tld_encoder_destroy(enc);
    printf("  all C struct-mapping checks passed\n");
    return 0;
}
