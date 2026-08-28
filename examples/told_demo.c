/* Exercises the TLD C ABI end to end: encode an object, decode it back, walk it. */
#include <qnnt_tld.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                                   \
    do {                                                                              \
        qnnt_tld_status _s = (expr);                                                   \
        if (_s != QNNT_TLD_OK) {                                                       \
            printf("FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr,                \
                   qnnt_tld_status_name(_s));                                          \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

int main(void) {
    printf("TLD version %s\n", qnnt_tld_version());

    qnnt_tld_encoder *enc = qnnt_tld_encoder_create();
    if (!enc) { printf("FAIL: encoder_create returned NULL\n"); return 1; }

    /* { "seq": 4242, "live": true, "node": "node-c", "raw": <0a ff 00>, "tags": [..] } */
    const uint8_t raw_payload[] = { 0x0A, 0xFF, 0x00 };
    size_t obj, arr;
    CHECK(qnnt_tld_begin_object(enc, &obj));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"seq", 3));
    CHECK(qnnt_tld_write_u64(enc, 4242));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"live", 4));
    CHECK(qnnt_tld_write_bool(enc, true));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"node", 4));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"node-c", 6));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"raw", 3));
    CHECK(qnnt_tld_write_buffer(enc, raw_payload, sizeof raw_payload));
    CHECK(qnnt_tld_write_string(enc, (const uint8_t *)"tags", 4));
    CHECK(qnnt_tld_begin_array(enc, &arr));
    CHECK(qnnt_tld_write_u64(enc, 1));
    CHECK(qnnt_tld_write_f64(enc, 1.5));
    CHECK(qnnt_tld_write_null(enc));
    CHECK(qnnt_tld_end_array(enc, arr));
    CHECK(qnnt_tld_end_object(enc, obj));

    size_t len = 0;
    const uint8_t *bytes = qnnt_tld_encoder_bytes(enc, &len);
    printf("encoded %zu bytes\n", len);

    /* A reader off a socket would use this to know how much to buffer. */
    size_t frame_size = 0;
    CHECK(qnnt_tld_frame_size(bytes, 2, &frame_size));
    printf("frame_size from a 2-byte prefix: %zu (== %zu)\n", frame_size, len);

    /* Only 1 byte available: must report INCOMPLETE, not guess. */
    qnnt_tld_status partial = qnnt_tld_frame_size(bytes, 1, &frame_size);
    printf("frame_size from 1 byte: %s\n", qnnt_tld_status_name(partial));

    CHECK(qnnt_tld_validate(bytes, len, 8));

    qnnt_tld_frame frame;
    CHECK(qnnt_tld_decode(bytes, len, &frame));
    printf("decoded type=0x%X raw_len=%zu (zero-copy: raw %s inside the input buffer)\n",
           frame.type, frame.raw_len,
           (frame.raw >= bytes && frame.raw < bytes + len) ? "points" : "does NOT point");

    qnnt_tld_iter it;
    CHECK(qnnt_tld_object_iter(&frame, &it));
    for (;;) {
        const uint8_t *key; size_t key_len; qnnt_tld_frame value; bool have;
        CHECK(qnnt_tld_object_next(&it, &key, &key_len, &value, &have));
        if (!have) break;

        printf("  %.*s = ", (int)key_len, (const char *)key);
        switch (value.type) {
        case QNNT_TLD_TINYUINT:
        case QNNT_TLD_INT8: case QNNT_TLD_INT16:
        case QNNT_TLD_INT32: case QNNT_TLD_INT64: {
            uint64_t n; CHECK(qnnt_tld_as_u64(&value, &n)); printf("%llu\n", (unsigned long long)n);
            break;
        }
        case QNNT_TLD_BOOL: {
            bool b; CHECK(qnnt_tld_as_bool(&value, &b)); printf("%s\n", b ? "true" : "false");
            break;
        }
        case QNNT_TLD_STRING: {
            const uint8_t *p; size_t l; CHECK(qnnt_tld_as_string(&value, &p, &l));
            printf("\"%.*s\"\n", (int)l, (const char *)p);
            break;
        }
        case QNNT_TLD_BUFFER: {
            const uint8_t *p; size_t l; CHECK(qnnt_tld_as_buffer(&value, &p, &l));
            printf("<buffer %zuB:", l);
            for (size_t i = 0; i < l; i++) printf(" %02x", p[i]);
            printf(">\n");
            break;
        }
        case QNNT_TLD_ARRAY: {
            qnnt_tld_iter inner; CHECK(qnnt_tld_array_iter(&value, &inner));
            printf("[");
            for (;;) {
                qnnt_tld_frame elem; bool more;
                CHECK(qnnt_tld_array_next(&inner, &elem, &more));
                if (!more) break;
                if (elem.type == QNNT_TLD_NULL) { printf(" null"); continue; }
                if (elem.type == QNNT_TLD_FLOAT64) {
                    double d; CHECK(qnnt_tld_as_f64(&elem, &d)); printf(" %.1f", d); continue;
                }
                uint64_t n; CHECK(qnnt_tld_as_u64(&elem, &n)); printf(" %llu", (unsigned long long)n);
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
           qnnt_tld_status_name(qnnt_tld_decode(reserved, 1, &frame)));
    const uint8_t bad_utf8[] = { 0x92, 0xFF, 0xFE };
    CHECK(qnnt_tld_decode(bad_utf8, sizeof bad_utf8, &frame));
    const uint8_t *sp; size_t sl;
    printf("STRING holding invalid utf-8 -> %s\n",
           qnnt_tld_status_name(qnnt_tld_as_string(&frame, &sp, &sl)));

    qnnt_tld_encoder_destroy(enc);
    qnnt_tld_encoder_destroy(NULL); /* must be a no-op */
    printf("OK\n");
    return 0;
}
