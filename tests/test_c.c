/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Claudio Russo (gattoarancione) */

/* Unit tests for the C ABI in told.h. Byte vectors follow the ones src/told.test.zig pins
 * for the Zig side, so the two implementations of every claim are checked against the same
 * bytes. */
#include <told.h>

#include <math.h>
#include <stdbool.h>

#include "harness.h"

static told_encoder *enc;

/* Bytes written since the last reset. */
static const uint8_t *encoded(size_t *len) {
    return told_encoder_bytes(enc, len);
}

#define ENCODES_AS(write_call, ...)                                                   \
    do {                                                                              \
        told_encoder_reset(enc);                                                      \
        CHECK((write_call) == TOLD_OK);                                               \
        size_t enc_len_ = 0;                                                          \
        const uint8_t *enc_bytes_ = encoded(&enc_len_);                               \
        EXPECT_BYTES(enc_bytes_, enc_len_, __VA_ARGS__);                              \
    } while (0)

static told_frame decoded(const uint8_t *bytes, size_t len) {
    told_frame frame;
    memset(&frame, 0, sizeof frame);
    CHECK(told_decode(bytes, len, &frame) == TOLD_OK);
    return frame;
}

/* --- inline family -------------------------------------------------------------- */

static void test_inline_values_are_one_byte(void) {
    ENCODES_AS(told_write_null(enc), 0x00);
    ENCODES_AS(told_write_bool(enc, true), 0x11);
    ENCODES_AS(told_write_bool(enc, false), 0x10);
    ENCODES_AS(told_write_u64(enc, 0), 0x20);
    ENCODES_AS(told_write_u64(enc, 15), 0x2F);

    const uint8_t t = 0x11;
    bool b = false;
    told_frame f = decoded(&t, 1);
    CHECK(told_as_bool(&f, &b) == TOLD_OK && b);
    CHECK(f.raw_len == 1 && f.data_len == 0);
}

static void test_null_ignores_mod_bool_rejects_it(void) {
    const uint8_t null_with_mod = 0x07, bool_two = 0x12;
    told_frame f;
    CHECK(told_decode(&null_with_mod, 1, &f) == TOLD_OK && f.type == TOLD_NULL);
    CHECK(told_decode(&bool_two, 1, &f) == TOLD_INVALID_MOD);
}

/* --- sized ints ------------------------------------------------------------------ */

static void test_spec_int32_holding_300_in_two_bytes(void) {
    const uint8_t bytes[] = {0x52, 0x01, 0x2C};
    told_frame f = decoded(bytes, sizeof bytes);
    int64_t v = 0;
    CHECK(f.type == TOLD_INT32);
    CHECK(told_as_i64(&f, &v) == TOLD_OK && v == 300);
    CHECK(f.raw_len == 3);
}

static void test_ints_use_the_fewest_bytes(void) {
    ENCODES_AS(told_write_u64(enc, 16), 0x31, 0x10);
    ENCODES_AS(told_write_u64(enc, 300), 0x42, 0x01, 0x2C);
    ENCODES_AS(told_write_u64(enc, 65536), 0x53, 0x01, 0x00, 0x00);
    ENCODES_AS(told_write_i64(enc, 0), 0x30);
    ENCODES_AS(told_write_i64(enc, -1), 0x31, 0xFF);
    ENCODES_AS(told_write_i64(enc, -300), 0x42, 0xFE, 0xD4);
    ENCODES_AS(told_write_i64(enc, INT64_MIN), 0x68, 0x80, 0, 0, 0, 0, 0, 0, 0);
}

static void test_u64_above_i64_reads_back_unsigned(void) {
    ENCODES_AS(told_write_u64(enc, UINT64_MAX), 0x68, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
               0xFF, 0xFF);
    size_t len;
    told_frame f = decoded(encoded(&len), len);
    uint64_t u = 0;
    int64_t i = 0;
    CHECK(told_as_u64(&f, &u) == TOLD_OK && u == UINT64_MAX);
    CHECK(told_as_i64(&f, &i) == TOLD_OK && i == -1); /* the same bits, read signed */
}

static void test_zero_length_ints_decode_to_zero(void) {
    const uint8_t headers[] = {0x30, 0x40, 0x50, 0x60};
    for (size_t k = 0; k < sizeof headers; k++) {
        told_frame f = decoded(&headers[k], 1);
        int64_t v = 99;
        CHECK(told_as_i64(&f, &v) == TOLD_OK && v == 0);
    }
}

static void test_mod_above_the_width_is_invalid(void) {
    const uint8_t cases[][2] = {{0x32, 0}, {0x43, 0}, {0x55, 0}, {0x69, 0},
                                {0x75, 0}, {0x89, 0}, {0xF1, 0x42}};
    told_frame f;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++)
        CHECK(told_decode(cases[k], 2, &f) == TOLD_INVALID_MOD);
}

/* --- floats ---------------------------------------------------------------------- */

static void test_floats_drop_trailing_zero_bytes(void) {
    ENCODES_AS(told_write_f64(enc, 0.0), 0x80);
    ENCODES_AS(told_write_f32(enc, 0.0f), 0x70);
    ENCODES_AS(told_write_f64(enc, 1.5), 0x82, 0x3F, 0xF8);
    ENCODES_AS(told_write_f64(enc, 0.1), 0x88, 0x3F, 0xB9, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A);
    ENCODES_AS(told_write_f32(enc, 1.5f), 0x72, 0x3F, 0xC0);

    size_t len;
    told_frame f = decoded(encoded(&len), len);
    double d = 0;
    CHECK(told_as_f64(&f, &d) == TOLD_OK && d == 1.5); /* FLOAT32 widens */
}

static void test_partial_floats_are_zero_filled(void) {
    const uint8_t two[] = {0x81, 0x40};
    const uint8_t full[] = {0x88, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0}; /* as a 1.0 encoder wrote it */
    double d = 0;
    told_frame f = decoded(two, sizeof two);
    CHECK(told_as_f64(&f, &d) == TOLD_OK && d == 2.0);
    f = decoded(full, sizeof full);
    CHECK(told_as_f64(&f, &d) == TOLD_OK && d == 1.5);
}

static void test_negative_zero_keeps_its_sign(void) {
    ENCODES_AS(told_write_f64(enc, -0.0), 0x81, 0x80);
    size_t len;
    told_frame f = decoded(encoded(&len), len);
    double d = 1;
    CHECK(told_as_f64(&f, &d) == TOLD_OK && d == 0.0 && signbit(d));
}

/* --- variable-length ------------------------------------------------------------- */

static void test_strings_inline_up_to_seven_bytes(void) {
    ENCODES_AS(told_write_string(enc, (const uint8_t *)"", 0), 0x90);
    ENCODES_AS(told_write_string(enc, (const uint8_t *)"hi", 2), 0x92, 'h', 'i');
    ENCODES_AS(told_write_string(enc, (const uint8_t *)"abcdefg", 7), 0x97, 'a', 'b', 'c',
               'd', 'e', 'f', 'g');
    ENCODES_AS(told_write_string(enc, (const uint8_t *)"abcdefgh", 8), 0x98, 0x08, 'a', 'b',
               'c', 'd', 'e', 'f', 'g', 'h');
    /* Lengths are bytes, not codepoints. */
    ENCODES_AS(told_write_string(enc, (const uint8_t *)"\xC3\xA9", 2), 0x92, 0xC3, 0xA9);
}

static void test_null_with_zero_length_is_an_empty_value(void) {
    /* An empty std::vector's data() may be null; that is still a legal empty value. */
    ENCODES_AS(told_write_string(enc, NULL, 0), 0x90);
    ENCODES_AS(told_write_buffer(enc, NULL, 0), 0xC0);
}

static void test_utf8_is_enforced_both_ways(void) {
    told_encoder_reset(enc);
    CHECK(told_write_string(enc, (const uint8_t *)"\xFF\xFE", 2) == TOLD_INVALID_UTF8);

    const uint8_t bad[] = {0x92, 0xFF, 0xFE};
    told_frame f = decoded(bad, sizeof bad); /* framing is fine */
    const uint8_t *p;
    size_t n;
    CHECK(told_as_string(&f, &p, &n) == TOLD_INVALID_UTF8);
}

static void test_spec_300_byte_buffer(void) {
    static uint8_t payload[300];
    memset(payload, 0xAB, sizeof payload);
    told_encoder_reset(enc);
    CHECK(told_write_buffer(enc, payload, sizeof payload) == TOLD_OK);

    size_t len;
    const uint8_t *bytes = encoded(&len);
    CHECK(len == 303);
    EXPECT_BYTES(bytes, 3, 0xC9, 0x01, 0x2C);

    told_frame f = decoded(bytes, len);
    const uint8_t *p;
    size_t n;
    CHECK(told_as_buffer(&f, &p, &n) == TOLD_OK && n == 300 && memcmp(p, payload, n) == 0);
    CHECK(p == bytes + 3); /* zero-copy: a view into the input */
}

/* --- containers ------------------------------------------------------------------ */

static void test_empty_containers_are_one_byte(void) {
    size_t mark;
    told_encoder_reset(enc);
    CHECK(told_begin_array(enc, &mark) == TOLD_OK && told_end_array(enc, mark) == TOLD_OK);
    CHECK(told_begin_object(enc, &mark) == TOLD_OK && told_end_object(enc, mark) == TOLD_OK);
    size_t len;
    EXPECT_BYTES(encoded(&len), len, 0xA0, 0xB0);
}

static void test_array_walks_its_children(void) {
    size_t mark;
    told_encoder_reset(enc);
    told_begin_array(enc, &mark);
    told_write_u64(enc, 1);
    told_write_string(enc, (const uint8_t *)"a", 1);
    told_write_null(enc);
    CHECK(told_end_array(enc, mark) == TOLD_OK);

    size_t len;
    const uint8_t *bytes = encoded(&len);
    EXPECT_BYTES(bytes, len, 0xA4, 0x21, 0x91, 'a', 0x00);

    told_frame f = decoded(bytes, len), child;
    told_iter it;
    bool have;
    uint8_t types[4];
    size_t count = 0;
    CHECK(told_array_iter(&f, &it) == TOLD_OK);
    while (told_array_next(&it, &child, &have) == TOLD_OK && have && count < 4)
        types[count++] = child.type;
    CHECK(count == 3);
    CHECK(types[0] == TOLD_TINYUINT && types[1] == TOLD_STRING && types[2] == TOLD_NULL);
}

static void test_object_keeps_key_order_and_nests(void) {
    size_t obj, arr;
    told_encoder_reset(enc);
    told_begin_object(enc, &obj);
    told_write_string(enc, (const uint8_t *)"z", 1);
    told_write_bool(enc, true);
    told_write_string(enc, (const uint8_t *)"a", 1);
    told_begin_array(enc, &arr);
    told_write_u64(enc, 2);
    told_end_array(enc, arr);
    CHECK(told_end_object(enc, obj) == TOLD_OK);

    size_t len;
    const uint8_t *bytes = encoded(&len);
    EXPECT_BYTES(bytes, len, 0xB7, 0x91, 'z', 0x11, 0x91, 'a', 0xA1, 0x22);

    told_frame f = decoded(bytes, len), value;
    told_iter it;
    const uint8_t *key;
    size_t key_len;
    bool have;
    CHECK(told_object_iter(&f, &it) == TOLD_OK);
    CHECK(told_object_next(&it, &key, &key_len, &value, &have) == TOLD_OK && have);
    CHECK(key_len == 1 && key[0] == 'z' && value.type == TOLD_BOOL);
    CHECK(told_object_next(&it, &key, &key_len, &value, &have) == TOLD_OK && have);
    CHECK(key_len == 1 && key[0] == 'a' && value.type == TOLD_ARRAY);
    CHECK(told_object_next(&it, &key, &key_len, &value, &have) == TOLD_OK && !have);
}

static void test_long_container_gets_a_length_field(void) {
    size_t mark;
    told_encoder_reset(enc);
    told_begin_array(enc, &mark);
    for (int k = 0; k < 8; k++) told_write_null(enc);
    CHECK(told_end_array(enc, mark) == TOLD_OK);
    size_t len;
    EXPECT_BYTES(encoded(&len), len, 0xA8, 0x08, 0, 0, 0, 0, 0, 0, 0, 0);
}

static void test_malformed_containers_are_caught(void) {
    told_iter it;
    told_frame f, child;
    const uint8_t *key;
    size_t key_len;
    bool have;

    const uint8_t int_key[] = {0xB2, 0x2A, 0x2B};
    f = decoded(int_key, sizeof int_key);
    CHECK(told_object_iter(&f, &it) == TOLD_OK);
    CHECK(told_object_next(&it, &key, &key_len, &child, &have) == TOLD_MALFORMED_CONTAINER);

    const uint8_t dangling_key[] = {0xB3, 0x92, 'h', 'i'};
    f = decoded(dangling_key, sizeof dangling_key);
    CHECK(told_object_iter(&f, &it) == TOLD_OK);
    CHECK(told_object_next(&it, &key, &key_len, &child, &have) == TOLD_MALFORMED_CONTAINER);

    const uint8_t overrun[] = {0xA2, 0x94, 'a'};
    f = decoded(overrun, sizeof overrun);
    CHECK(told_array_iter(&f, &it) == TOLD_OK);
    CHECK(told_array_next(&it, &child, &have) == TOLD_TRUNCATED);
}

static void test_accessors_refuse_the_wrong_type(void) {
    const uint8_t seven = 0x27;
    told_frame f = decoded(&seven, 1);
    bool b;
    double d;
    const uint8_t *p;
    size_t n;
    told_iter it;
    CHECK(told_as_bool(&f, &b) == TOLD_TYPE_MISMATCH);
    CHECK(told_as_f64(&f, &d) == TOLD_TYPE_MISMATCH);
    CHECK(told_as_string(&f, &p, &n) == TOLD_TYPE_MISMATCH);
    CHECK(told_as_buffer(&f, &p, &n) == TOLD_TYPE_MISMATCH);
    CHECK(told_array_iter(&f, &it) == TOLD_TYPE_MISMATCH);
    CHECK(told_object_iter(&f, &it) == TOLD_TYPE_MISMATCH);
}

/* --- reserved and extended -------------------------------------------------------- */

static void test_reserved_nibbles_are_refused(void) {
    const uint8_t d = 0xD0, e = 0xE0;
    told_frame f;
    size_t size;
    CHECK(told_decode(&d, 1, &f) == TOLD_RESERVED_TYPE);
    CHECK(told_decode(&e, 1, &f) == TOLD_RESERVED_TYPE);
    CHECK(told_frame_size(&d, 1, &size) == TOLD_RESERVED_TYPE);
}

static void test_extended_exposes_its_code_not_its_extent(void) {
    const uint8_t payload[] = {1, 2, 3};
    ENCODES_AS(told_write_extended(enc, 0x42, payload, sizeof payload), 0xF0, 0x42, 1, 2, 3);
    size_t len, size;
    const uint8_t *bytes = encoded(&len);
    told_frame f = decoded(bytes, len);
    CHECK(f.type == TOLD_EXTENDED && f.ext_code == 0x42);
    CHECK(told_frame_size(bytes, len, &size) == TOLD_EXTENSION_FRAMING_UNKNOWN);

    const uint8_t seven = 0x27;
    f = decoded(&seven, 1);
    CHECK(f.ext_code == -1);
}

/* --- framing --------------------------------------------------------------------- */

static void test_frame_size_needs_at_most_nine_bytes(void) {
    size_t size = 0;
    const uint8_t null_frame = 0x00, int32 = 0x52, f64 = 0x88;
    const uint8_t long_buffer[] = {0xC9, 0x01, 0x2C};
    const uint8_t widest[] = {0xCF, 0, 0, 0, 0, 0, 0, 0, 5};

    CHECK(told_frame_size(long_buffer, 0, &size) == TOLD_INCOMPLETE);
    CHECK(told_frame_size(&null_frame, 1, &size) == TOLD_OK && size == 1);
    CHECK(told_frame_size(&int32, 1, &size) == TOLD_OK && size == 3);
    CHECK(told_frame_size(&f64, 1, &size) == TOLD_OK && size == 9);
    CHECK(told_frame_size(long_buffer, 1, &size) == TOLD_INCOMPLETE);
    CHECK(told_frame_size(long_buffer, 2, &size) == TOLD_INCOMPLETE);
    CHECK(told_frame_size(long_buffer, 3, &size) == TOLD_OK && size == 303);
    CHECK(told_frame_size(widest, sizeof widest, &size) == TOLD_OK && size == 14);
}

static void test_truncated_frames_are_refused(void) {
    const uint8_t int32[] = {0x52, 0x01};
    const uint8_t str[] = {0x92, 'h'};
    told_frame f;
    CHECK(told_decode(int32, 0, &f) == TOLD_TRUNCATED);
    CHECK(told_decode(int32, sizeof int32, &f) == TOLD_TRUNCATED);
    CHECK(told_decode(str, sizeof str, &f) == TOLD_TRUNCATED);
}

static void test_decode_reports_where_the_frame_ends(void) {
    const uint8_t two[] = {0x92, 'h', 'i', 0x11};
    told_frame f = decoded(two, sizeof two);
    CHECK(f.raw_len == 3 && f.raw == two && f.data == two + 1 && f.data_len == 2);
    f = decoded(two + f.raw_len, sizeof two - 3);
    CHECK(f.type == TOLD_BOOL);
}

static void test_validate_walks_the_whole_tree(void) {
    const uint8_t nested_bad[] = {0xB7, 0x92, 'o', 'k', 0xA3, 0x92, 0xFF, 0xFE};
    told_frame f;
    CHECK(told_decode(nested_bad, sizeof nested_bad, &f) == TOLD_OK);
    CHECK(told_validate(nested_bad, sizeof nested_bad, 8) == TOLD_INVALID_UTF8);
}

static void test_validate_enforces_a_depth_limit(void) {
    size_t marks[16];
    told_encoder_reset(enc);
    for (int k = 0; k < 16; k++) told_begin_array(enc, &marks[k]);
    for (int k = 15; k >= 0; k--) told_end_array(enc, marks[k]);
    size_t len;
    const uint8_t *bytes = encoded(&len);
    CHECK(told_validate(bytes, len, 32) == TOLD_OK);
    CHECK(told_validate(bytes, len, 4) == TOLD_DEPTH_EXCEEDED);
}

/* --- the encoder handle ---------------------------------------------------------- */

static void test_reset_keeps_the_handle_usable(void) {
    told_encoder_reset(enc);
    told_write_u64(enc, 1);
    told_encoder_reset(enc);
    size_t len = 99;
    encoded(&len);
    CHECK(len == 0);
    ENCODES_AS(told_write_u64(enc, 2), 0x22);
}

static void test_null_arguments_are_a_status_never_a_crash(void) {
    told_frame f;
    told_iter it;
    size_t n, mark;
    const uint8_t byte = 0x00;
    CHECK(told_decode(NULL, 1, &f) == TOLD_INVALID_ARGUMENT);
    CHECK(told_decode(&byte, 1, NULL) == TOLD_INVALID_ARGUMENT);
    CHECK(told_frame_size(NULL, 1, &n) == TOLD_INVALID_ARGUMENT);
    CHECK(told_validate(NULL, 1, 8) == TOLD_INVALID_ARGUMENT);
    CHECK(told_array_iter(NULL, &it) == TOLD_INVALID_ARGUMENT);
    CHECK(told_write_null(NULL) == TOLD_INVALID_ARGUMENT);
    CHECK(told_begin_array(NULL, &mark) == TOLD_INVALID_ARGUMENT);
    CHECK(told_encoder_bytes(NULL, &n) == NULL);
    told_encoder_destroy(NULL); /* a no-op, as free(NULL) is */
}

static void test_a_mark_past_the_end_is_refused(void) {
    told_encoder_reset(enc);
    told_write_null(enc);
    CHECK(told_end_array(enc, 2) == TOLD_INVALID_ARGUMENT);
    CHECK(told_end_object(enc, (size_t)-1) == TOLD_INVALID_ARGUMENT);
    size_t len;
    EXPECT_BYTES(encoded(&len), len, 0x00); /* and the buffer is untouched */
}

static void test_a_mark_that_is_no_open_container_is_refused(void) {
    told_encoder_reset(enc);
    told_write_string(enc, (const uint8_t *)"hi", 2);
    told_write_buffer(enc, (const uint8_t[]){0x05}, 1);
    CHECK(told_end_array(enc, 0) == TOLD_INVALID_ARGUMENT);  /* a STRING header */
    CHECK(told_end_object(enc, 4) == TOLD_INVALID_ARGUMENT); /* 5 bytes would run past the end */
    size_t len;
    EXPECT_BYTES(encoded(&len), len, 0x92, 'h', 'i', 0xC1, 0x05);
}

static void test_a_reused_encoder_writes_the_same_bytes(void) {
    /* The second message is encoded with the header sizes learnt from the first. */
    uint8_t first[512];
    size_t first_len = 0;
    for (int round = 0; round < 2; round++) {
        told_encoder_reset(enc);
        size_t obj, arr;
        told_begin_object(enc, &obj);
        told_write_string(enc, (const uint8_t *)"readings", 8);
        told_begin_array(enc, &arr);
        for (int k = 0; k < 40; k++) told_write_u64(enc, 1000u + (uint64_t)k);
        CHECK(told_end_array(enc, arr) == TOLD_OK);
        CHECK(told_end_object(enc, obj) == TOLD_OK);
        size_t len;
        const uint8_t *bytes = encoded(&len);
        CHECK(told_validate(bytes, len, 8) == TOLD_OK);
        if (round == 0) {
            CHECK(len <= sizeof first);
            memcpy(first, bytes, len);
            first_len = len;
        } else {
            CHECK(len == first_len && memcmp(first, bytes, len) == 0);
        }
    }
}

static void test_every_status_has_a_name(void) {
    for (int s = TOLD_OK; s <= TOLD_OUT_OF_RANGE; s++) {
        const char *name = told_status_name((told_status)s);
        CHECK(name != NULL && name[0] != '\0');
    }
    CHECK(strcmp(told_status_name(TOLD_END_OF_STREAM), "end of stream") == 0);
    CHECK(strcmp(told_version(), "1.1") == 0);
}

/* --- struct mapping -------------------------------------------------------------- */

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
static const told_desc reading_named = TOLD_DESC("reading", reading_fields, 1);
static const told_desc reading_positional = TOLD_DESC("reading", reading_fields, 0);

static void test_struct_packs_as_an_object_keyed_by_name(void) {
    const reading r = {1, 0.0, true};
    ENCODES_AS(told_pack_struct(enc, &reading_named, &r), 0xB8, 0x17, 0x97, 'c', 'h', 'a',
               'n', 'n', 'e', 'l', 0x21, 0x95, 'v', 'a', 'l', 'u', 'e', 0x80, 0x95, 'v', 'a',
               'l', 'i', 'd', 0x11);
    size_t len;
    const uint8_t *bytes = encoded(&len);
    reading back;
    memset(&back, 0, sizeof back);
    CHECK(told_unpack_struct_bytes(bytes, len, &reading_named, &back) == TOLD_OK);
    CHECK(back.channel == 1 && back.value == 0.0 && back.valid);
}

static void test_struct_packs_positionally(void) {
    const reading r = {2, 0.0, false};
    ENCODES_AS(told_pack_struct(enc, &reading_positional, &r), 0xA3, 0x22, 0x80, 0x10);
}

static void test_unpack_leaves_absent_fields_and_ignores_unknown_keys(void) {
    /* {"value": 0.0, "extra": null} */
    const uint8_t bytes[] = {0xB8, 0x0E, 0x95, 'v', 'a', 'l', 'u', 'e', 0x80,
                             0x95, 'e', 'x', 't', 'r', 'a', 0x00};
    reading r = {7, 9.5, true};
    CHECK(told_unpack_struct_bytes(bytes, sizeof bytes, &reading_named, &r) == TOLD_OK);
    CHECK(r.channel == 7 && r.value == 0.0 && r.valid);
}

static void test_unpack_refuses_a_value_out_of_range(void) {
    /* {"channel": 300} into a uint8_t */
    const uint8_t bytes[] = {0xB8, 0x0B, 0x97, 'c', 'h', 'a', 'n', 'n', 'e', 'l', 0x42, 0x01,
                             0x2C};
    reading r = {7, 0.0, false};
    CHECK(told_unpack_struct_bytes(bytes, sizeof bytes, &reading_named, &r) == TOLD_OUT_OF_RANGE);
    CHECK(r.channel == 7);
}

/* --- stream reading -------------------------------------------------------------- */

/* An in-memory stream handing out at most `chunk` bytes per call, so frame boundaries and
 * length fields get split across reads. */
typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
    size_t chunk;
    size_t calls;
} mem_stream;

static ptrdiff_t mem_read(void *ctx, uint8_t *dst, size_t len) {
    mem_stream *m = (mem_stream *)ctx;
    m->calls++;
    size_t n = m->len - m->pos;
    if (n > len) n = len;
    if (n > m->chunk) n = m->chunk;
    memcpy(dst, m->bytes + m->pos, n);
    m->pos += n;
    return (ptrdiff_t)n;
}

static ptrdiff_t file_read(void *ctx, uint8_t *dst, size_t len) {
    FILE *f = (FILE *)ctx;
    size_t n = fread(dst, 1, len, f);
    if (n == 0 && ferror(f)) return -1;
    return (ptrdiff_t)n;
}

static ptrdiff_t failing_read(void *ctx, uint8_t *dst, size_t len) {
    (void)ctx; (void)dst; (void)len;
    return -1;
}

/* Claims more than it was asked for: a broken callback that must not be believed. */
static ptrdiff_t lying_read(void *ctx, uint8_t *dst, size_t len) {
    (void)ctx;
    dst[0] = 0x92;
    return (ptrdiff_t)len + 1;
}

/* An object, a 300-byte BUFFER (two-byte length field) and a TINYUINT, back to back. */
static uint8_t stream_bytes[512];
static size_t stream_sizes[3];
static size_t stream_len;

static void build_stream(void) {
    static uint8_t payload[300];
    memset(payload, 'x', sizeof payload);
    size_t mark, len;
    const uint8_t *bytes;
    stream_len = 0;

    told_encoder_reset(enc);
    told_begin_object(enc, &mark);
    told_write_string(enc, (const uint8_t *)"seq", 3);
    told_write_u64(enc, 4242);
    told_end_object(enc, mark);
    bytes = encoded(&len);
    memcpy(stream_bytes + stream_len, bytes, len);
    stream_len += stream_sizes[0] = len;

    told_encoder_reset(enc);
    told_write_buffer(enc, payload, sizeof payload);
    bytes = encoded(&len);
    memcpy(stream_bytes + stream_len, bytes, len);
    stream_len += stream_sizes[1] = len;

    told_encoder_reset(enc);
    told_write_u64(enc, 7);
    bytes = encoded(&len);
    memcpy(stream_bytes + stream_len, bytes, len);
    stream_len += stream_sizes[2] = len;
}

static void test_read_frame_one_byte_per_read(void) {
    mem_stream m = {stream_bytes, stream_len, 0, 1, 0};
    uint8_t buf[512];
    told_frame f;
    size_t at = 0;
    for (int k = 0; k < 3; k++) {
        CHECK(told_read_frame(mem_read, &m, buf, sizeof buf, &f) == TOLD_OK);
        CHECK(f.raw == buf && f.raw_len == stream_sizes[k]);
        CHECK(memcmp(f.raw, stream_bytes + at, f.raw_len) == 0);
        at += stream_sizes[k];
        CHECK(m.pos == at); /* nothing past the frame was asked for */
    }
    uint64_t seven = 0;
    CHECK(told_as_u64(&f, &seven) == TOLD_OK && seven == 7);
    CHECK(told_read_frame(mem_read, &m, buf, sizeof buf, &f) == TOLD_END_OF_STREAM);
}

static void test_read_frame_leaves_the_rest_of_a_file_alone(void) {
    FILE *file = tmpfile();
    CHECK(file != NULL);
    if (!file) return;
    fwrite(stream_bytes, 1, stream_len, file);
    fputs("tail", file);
    rewind(file);

    uint8_t buf[512];
    told_frame f;
    for (int k = 0; k < 3; k++)
        CHECK(told_read_frame(file_read, file, buf, sizeof buf, &f) == TOLD_OK);
    char rest[8] = {0};
    CHECK(fread(rest, 1, sizeof rest - 1, file) == 4 && strcmp(rest, "tail") == 0);
    fclose(file);
}

static void test_read_frame_reports_a_cut_stream(void) {
    uint8_t buf[512];
    told_frame f;
    mem_stream in_data = {stream_bytes, stream_sizes[0] - 1, 0, 64, 0};
    CHECK(told_read_frame(mem_read, &in_data, buf, sizeof buf, &f) == TOLD_TRUNCATED);
    mem_stream in_length = {stream_bytes + stream_sizes[0], 2, 0, 64, 0};
    CHECK(told_read_frame(mem_read, &in_length, buf, sizeof buf, &f) == TOLD_TRUNCATED);
}

static void test_read_frame_refuses_a_frame_bigger_than_the_buffer(void) {
    uint8_t buf[64];
    told_frame f;
    mem_stream m = {stream_bytes + stream_sizes[0], stream_sizes[1], 0, 64, 0};
    CHECK(told_read_frame(mem_read, &m, buf, sizeof buf, &f) == TOLD_FRAME_TOO_LARGE);
    CHECK(m.pos == 3); /* from the length field alone, before any data */

    mem_stream short_prefix = {stream_bytes + stream_sizes[0], stream_sizes[1], 0, 64, 0};
    CHECK(told_read_frame(mem_read, &short_prefix, buf, 2, &f) == TOLD_FRAME_TOO_LARGE);
}

static void test_read_frame_reports_callback_errors(void) {
    uint8_t buf[64];
    told_frame f;
    CHECK(told_read_frame(failing_read, NULL, buf, sizeof buf, &f) == TOLD_READ_FAILED);
    CHECK(told_read_frame(lying_read, NULL, buf, sizeof buf, &f) == TOLD_READ_FAILED);
}

static void test_read_frame_refuses_what_decode_refuses(void) {
    uint8_t buf[64];
    told_frame f;
    const uint8_t reserved[] = {0xD0};
    mem_stream r = {reserved, sizeof reserved, 0, 64, 0};
    CHECK(told_read_frame(mem_read, &r, buf, sizeof buf, &f) == TOLD_RESERVED_TYPE);
    const uint8_t extended[] = {0xF0, 0x01, 0xAA};
    mem_stream x = {extended, sizeof extended, 0, 64, 0};
    CHECK(told_read_frame(mem_read, &x, buf, sizeof buf, &f) == TOLD_EXTENSION_FRAMING_UNKNOWN);
}

static void test_read_frame_checks_arguments_before_reading(void) {
    uint8_t buf[64];
    told_frame f;
    mem_stream m = {stream_bytes, stream_len, 0, 64, 0};
    CHECK(told_read_frame(NULL, &m, buf, sizeof buf, &f) == TOLD_INVALID_ARGUMENT);
    CHECK(told_read_frame(mem_read, &m, NULL, sizeof buf, &f) == TOLD_INVALID_ARGUMENT);
    CHECK(told_read_frame(mem_read, &m, buf, 0, &f) == TOLD_INVALID_ARGUMENT);
    CHECK(told_read_frame(mem_read, &m, buf, sizeof buf, NULL) == TOLD_INVALID_ARGUMENT);
    CHECK(m.calls == 0);
}

int main(void) {
    enc = told_encoder_create();
    if (!enc) {
        printf("told_encoder_create returned NULL\n");
        return 1;
    }
    printf("told C unit tests\n");

    RUN(test_inline_values_are_one_byte);
    RUN(test_null_ignores_mod_bool_rejects_it);
    RUN(test_spec_int32_holding_300_in_two_bytes);
    RUN(test_ints_use_the_fewest_bytes);
    RUN(test_u64_above_i64_reads_back_unsigned);
    RUN(test_zero_length_ints_decode_to_zero);
    RUN(test_mod_above_the_width_is_invalid);
    RUN(test_floats_drop_trailing_zero_bytes);
    RUN(test_partial_floats_are_zero_filled);
    RUN(test_negative_zero_keeps_its_sign);
    RUN(test_strings_inline_up_to_seven_bytes);
    RUN(test_null_with_zero_length_is_an_empty_value);
    RUN(test_utf8_is_enforced_both_ways);
    RUN(test_spec_300_byte_buffer);
    RUN(test_empty_containers_are_one_byte);
    RUN(test_array_walks_its_children);
    RUN(test_object_keeps_key_order_and_nests);
    RUN(test_long_container_gets_a_length_field);
    RUN(test_malformed_containers_are_caught);
    RUN(test_accessors_refuse_the_wrong_type);
    RUN(test_reserved_nibbles_are_refused);
    RUN(test_extended_exposes_its_code_not_its_extent);
    RUN(test_frame_size_needs_at_most_nine_bytes);
    RUN(test_truncated_frames_are_refused);
    RUN(test_decode_reports_where_the_frame_ends);
    RUN(test_validate_walks_the_whole_tree);
    RUN(test_validate_enforces_a_depth_limit);
    RUN(test_reset_keeps_the_handle_usable);
    RUN(test_null_arguments_are_a_status_never_a_crash);
    RUN(test_a_mark_past_the_end_is_refused);
    RUN(test_a_mark_that_is_no_open_container_is_refused);
    RUN(test_a_reused_encoder_writes_the_same_bytes);
    RUN(test_every_status_has_a_name);
    RUN(test_struct_packs_as_an_object_keyed_by_name);
    RUN(test_struct_packs_positionally);
    RUN(test_unpack_leaves_absent_fields_and_ignores_unknown_keys);
    RUN(test_unpack_refuses_a_value_out_of_range);

    build_stream();
    RUN(test_read_frame_one_byte_per_read);
    RUN(test_read_frame_leaves_the_rest_of_a_file_alone);
    RUN(test_read_frame_reports_a_cut_stream);
    RUN(test_read_frame_refuses_a_frame_bigger_than_the_buffer);
    RUN(test_read_frame_reports_callback_errors);
    RUN(test_read_frame_refuses_what_decode_refuses);
    RUN(test_read_frame_checks_arguments_before_reading);

    told_encoder_destroy(enc);
    return harness_report();
}
