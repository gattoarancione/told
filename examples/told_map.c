/*
 * Struct mapping in C: a descriptor table instead of templates, and a check that this side
 * agrees with the Zig, C++ and Python mappings on the same golden frame.
 *
 *   zig build told-map-c
 *
 * The schema is the one in src/testdata/interop.tld. C cannot hold the dynamic parts of it
 * -- a std::vector of readings, an optional string -- so this uses the fixed-size
 * equivalents: an array of two readings, and a note that is simply absent from the frame.
 */

#include "told.h"

#include <stdio.h>
#include <string.h>

/* CHECK() vanishes under NDEBUG, which a release build defines -- then the checks below
 * would compile away and the example would prove nothing while still passing. */
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);    \
            return 1;                                                                \
        }                                                                            \
    } while (0)

typedef struct {
    uint8_t channel;
    double value;
    bool valid;
} reading;

static const told_field reading_fields[] = {
    TOLD_FIELD(reading, channel, TOLD_F_U8),
    TOLD_FIELD(reading, value, TOLD_F_F64),
    TOLD_FIELD(reading, valid, TOLD_F_BOOL),
};
static const told_desc reading_desc =
    TOLD_DESC("reading", reading_fields, 1);

typedef struct {
    double x;
    double y;
} point;

static const told_field point_fields[] = {
    TOLD_FIELD(point, x, TOLD_F_F64),
    TOLD_FIELD(point, y, TOLD_F_F64),
};
static const told_desc point_desc = TOLD_DESC("point", point_fields, 1);

typedef struct {
    uint64_t id;
    char name[32];
    int32_t mode;
    reading readings[2];
    uint8_t raw[8];
    size_t raw_len;
    point origin;
} sample;

static const told_field sample_fields[] = {
    TOLD_FIELD(sample, id, TOLD_F_U64),
    TOLD_FIELD_STRING(sample, name),
    TOLD_FIELD(sample, mode, TOLD_F_I32),
    TOLD_FIELD_ARRAY_STRUCT(sample, readings, &reading_desc),
    TOLD_FIELD_BYTES(sample, raw, raw_len),
    TOLD_FIELD_STRUCT(sample, origin, &point_desc),
};
static const told_desc sample_desc = TOLD_DESC("sample", sample_fields, 1);

/* Short wire keys, the C counterpart of TOLD_NVP. */
typedef struct {
    uint8_t channel;
    double value;
} terse;

static const told_field terse_fields[] = {
    TOLD_FIELD_AS(terse, channel, TOLD_F_U8, "ch"),
    TOLD_FIELD_AS(terse, value, TOLD_F_F64, "v"),
};
static const told_desc terse_desc = TOLD_DESC("terse", terse_fields, 1);

/* Positional: no keys on the wire at all. */
static const told_desc point_array_desc =
    TOLD_DESC("point", point_fields, 0);

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
    told_encoder *enc = told_encoder_create();
    CHECK(enc != NULL);

    /* 1. round trip through the descriptor */
    const sample sent = canonical();
    CHECK(told_pack_struct(enc, &sample_desc, &sent) == TOLD_OK);

    size_t len = 0;
    const uint8_t *bytes = told_encoder_bytes(enc, &len);
    CHECK(bytes != NULL);

    sample got;
    memset(&got, 0, sizeof got);
    CHECK(told_unpack_struct_bytes(bytes, len, &sample_desc, &got) == TOLD_OK);

    CHECK(got.id == 99);
    CHECK(strcmp(got.name, "sensor-a") == 0);
    CHECK(got.mode == 2);
    CHECK(got.readings[0].channel == 1 && got.readings[0].value == 3.5);
    CHECK(got.readings[0].valid && !got.readings[1].valid);
    CHECK(got.raw_len == 3 && got.raw[1] == 0xFF);
    CHECK(got.origin.x == 1.5 && got.origin.y == -2.5);
    printf("  round trip: %zu bytes\n", len);

    /* 2. short keys, and the positional form */
    told_encoder_reset(enc);
    const terse t = {3, 9.5};
    CHECK(told_pack_struct(enc, &terse_desc, &t) == TOLD_OK);
    size_t terse_len = 0;
    (void)told_encoder_bytes(enc, &terse_len);

    told_encoder_reset(enc);
    const point p = {1.0, 2.0};
    CHECK(told_pack_struct(enc, &point_desc, &p) == TOLD_OK);
    size_t named_len = 0;
    (void)told_encoder_bytes(enc, &named_len);

    told_encoder_reset(enc);
    CHECK(told_pack_struct(enc, &point_array_desc, &p) == TOLD_OK);
    size_t positional_len = 0;
    const uint8_t *positional = told_encoder_bytes(enc, &positional_len);
    CHECK(positional_len < named_len);
    printf("  point: %zu bytes named, %zu positional\n", named_len, positional_len);

    point back = {0, 0};
    CHECK(told_unpack_struct_bytes(positional, positional_len, &point_array_desc,
                                        &back) == TOLD_OK);
    CHECK(back.x == 1.0 && back.y == 2.0);

    /* 3. the golden frame written by the Zig mapping */
    FILE *f = fopen("src/testdata/interop.tld", "rb");
    if (f == NULL) {
        printf("  golden frame not found (run from the project root) -- skipped\n");
        told_encoder_destroy(enc);
        return 0;
    }
    static uint8_t golden[4096];
    const size_t golden_len = fread(golden, 1, sizeof golden, f);
    fclose(f);

    sample from_zig;
    memset(&from_zig, 0, sizeof from_zig);
    CHECK(told_unpack_struct_bytes(golden, golden_len, &sample_desc, &from_zig) ==
           TOLD_OK);
    CHECK(from_zig.id == 99);
    CHECK(strcmp(from_zig.name, "sensor-a") == 0);
    CHECK(from_zig.mode == 2);
    CHECK(from_zig.readings[1].value == -0.25 && !from_zig.readings[1].valid);
    CHECK(from_zig.raw_len == 3 && from_zig.raw[1] == 0xFF);
    CHECK(from_zig.origin.y == -2.5);
    printf("  golden frame from Zig decoded through the descriptor table\n");

    /* The `note` field is null in the golden frame and has no C counterpart here; the
     * descriptor simply does not mention it, which is the forward-compatibility rule
     * doing its job rather than an omission. */

    told_encoder_destroy(enc);
    printf("  all C struct-mapping checks passed\n");
    return 0;
}
