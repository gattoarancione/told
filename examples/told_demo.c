/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Claudio Russo (gattoarancione) */

/* Exercises the TOLD C ABI end to end: encode an object, decode it back, walk it. */
#include <told.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                   \
    do {                                                                              \
        told_status _s = (expr);                                                      \
        if (_s != TOLD_OK) {                                                          \
            printf("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr,               \
                   told_status_name(_s));                                             \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

int main(void) {
    printf("TOLD version %s\n", told_version());

    told_encoder *enc = told_encoder_create();
    if (!enc) { printf("FAIL: encoder_create returned NULL\n"); return 1; }

    /* { "seq": 4242, "live": true, "node": "node-c", "raw": <0a ff 00>, "tags": [..] } */
    const uint8_t raw_payload[] = { 0x0A, 0xFF, 0x00 };
    size_t obj, arr;
    CHECK(told_begin_object(enc, &obj));
    CHECK(told_write_string(enc, (const uint8_t *)"seq", 3));
    CHECK(told_write_u64(enc, 4242));
    CHECK(told_write_string(enc, (const uint8_t *)"live", 4));
    CHECK(told_write_bool(enc, true));
    CHECK(told_write_string(enc, (const uint8_t *)"node", 4));
    CHECK(told_write_string(enc, (const uint8_t *)"node-c", 6));
    CHECK(told_write_string(enc, (const uint8_t *)"raw", 3));
    CHECK(told_write_buffer(enc, raw_payload, sizeof raw_payload));
    CHECK(told_write_string(enc, (const uint8_t *)"tags", 4));
    CHECK(told_begin_array(enc, &arr));
    CHECK(told_write_u64(enc, 1));
    CHECK(told_write_f64(enc, 1.5));
    CHECK(told_write_null(enc));
    CHECK(told_end_array(enc, arr));
    CHECK(told_end_object(enc, obj));

    size_t len = 0;
    const uint8_t *bytes = told_encoder_bytes(enc, &len);
    printf("encoded %zu bytes\n", len);

    /* A reader off a socket would use this to know how much to buffer. */
    size_t frame_size = 0;
    CHECK(told_frame_size(bytes, 2, &frame_size));
    printf("frame_size from a 2-byte prefix: %zu (== %zu)\n", frame_size, len);

    /* Only 1 byte available: must report INCOMPLETE, not guess. */
    told_status partial = told_frame_size(bytes, 1, &frame_size);
    printf("frame_size from 1 byte: %s\n", told_status_name(partial));

    CHECK(told_validate(bytes, len, 8));

    told_frame frame;
    CHECK(told_decode(bytes, len, &frame));
    printf("decoded type=0x%X raw_len=%zu (zero-copy: raw %s inside the input buffer)\n",
           frame.type, frame.raw_len,
           (frame.raw >= bytes && frame.raw < bytes + len) ? "points" : "does NOT point");

    told_iter it;
    CHECK(told_object_iter(&frame, &it));
    for (;;) {
        const uint8_t *key; size_t key_len; told_frame value; bool have;
        CHECK(told_object_next(&it, &key, &key_len, &value, &have));
        if (!have) break;

        printf("  %.*s = ", (int)key_len, (const char *)key);
        switch (value.type) {
        case TOLD_TINYUINT:
        case TOLD_INT8: case TOLD_INT16:
        case TOLD_INT32: case TOLD_INT64: {
            uint64_t n; CHECK(told_as_u64(&value, &n)); printf("%llu\n", (unsigned long long)n);
            break;
        }
        case TOLD_BOOL: {
            bool b; CHECK(told_as_bool(&value, &b)); printf("%s\n", b ? "true" : "false");
            break;
        }
        case TOLD_STRING: {
            const uint8_t *p; size_t l; CHECK(told_as_string(&value, &p, &l));
            printf("\"%.*s\"\n", (int)l, (const char *)p);
            break;
        }
        case TOLD_BUFFER: {
            const uint8_t *p; size_t l; CHECK(told_as_buffer(&value, &p, &l));
            printf("<buffer %zuB:", l);
            for (size_t i = 0; i < l; i++) printf(" %02x", p[i]);
            printf(">\n");
            break;
        }
        case TOLD_ARRAY: {
            told_iter inner; CHECK(told_array_iter(&value, &inner));
            printf("[");
            for (;;) {
                told_frame elem; bool more;
                CHECK(told_array_next(&inner, &elem, &more));
                if (!more) break;
                if (elem.type == TOLD_NULL) { printf(" null"); continue; }
                if (elem.type == TOLD_FLOAT64) {
                    double d; CHECK(told_as_f64(&elem, &d)); printf(" %.1f", d); continue;
                }
                uint64_t n; CHECK(told_as_u64(&elem, &n)); printf(" %llu", (unsigned long long)n);
            }
            printf(" ]\n");
            break;
        }
        default: printf("(type 0x%X)\n", value.type);
        }
    }

    /* Errors must surface as status codes, never as a trap. */
    const uint8_t reserved[] = { 0xD0 };
    printf("reserved nibble 0xD0 -> %s\n",
           told_status_name(told_decode(reserved, 1, &frame)));
    const uint8_t bad_utf8[] = { 0x92, 0xFF, 0xFE };
    CHECK(told_decode(bad_utf8, sizeof bad_utf8, &frame));
    const uint8_t *sp; size_t sl;
    printf("STRING holding invalid utf-8 -> %s\n",
           told_status_name(told_as_string(&frame, &sp, &sl)));

    told_encoder_destroy(enc);
    told_encoder_destroy(NULL); /* must be a no-op */
    printf("OK\n");
    return 0;
}
