/* Exercises told_read_frame: frames read one at a time off a stream, through a callback,
 * into a caller-owned buffer. The checks that matter are the boundaries -- short reads,
 * a length field split across reads, never reading past a frame -- and that every way a
 * stream can go wrong comes back as its own status rather than a guess. */
#include <told.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(name, cond)                                                            \
    do {                                                                              \
        if (cond) {                                                                   \
            printf("  ok    %s\n", name);                                             \
        } else {                                                                      \
            printf("  FAIL  %s (%s:%d)\n", name, __FILE__, __LINE__);                 \
            failures++;                                                               \
        }                                                                             \
    } while (0)

#define EXPECT_STATUS(name, expr, want)                                                 \
    do {                                                                                \
        told_status _s = (expr);                                                        \
        if (_s == (want)) {                                                             \
            printf("  ok    %s -> %s\n", name, told_status_name(_s));                   \
        } else {                                                                        \
            printf("  FAIL  %s -> %s, wanted %s (%s:%d)\n", name, told_status_name(_s), \
                   told_status_name(want), __FILE__, __LINE__);                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* --- streams ------------------------------------------------------------------ */

/* An in-memory stream that hands out at most `chunk` bytes per call, so every frame
 * boundary and every length field gets split across reads. */
typedef struct {
    const uint8_t *bytes;
    size_t         len;
    size_t         pos;
    size_t         chunk;
    size_t         calls;
} mem_stream;

static ptrdiff_t mem_read(void *ctx, uint8_t *dst, size_t len) {
    mem_stream *m = ctx;
    m->calls++;
    size_t n = m->len - m->pos;
    if (n > len) n = len;
    if (n > m->chunk) n = m->chunk;
    memcpy(dst, m->bytes + m->pos, n);
    m->pos += n;
    return (ptrdiff_t)n;
}

static ptrdiff_t file_read(void *ctx, uint8_t *dst, size_t len) {
    FILE *f = ctx;
    size_t n = fread(dst, 1, len, f);
    if (n == 0 && ferror(f)) return -1;
    return (ptrdiff_t)n;
}

static ptrdiff_t failing_read(void *ctx, uint8_t *dst, size_t len) {
    (void)ctx; (void)dst; (void)len;
    return -1;
}

/* Claims more bytes than it was asked for -- a broken callback that must not be believed. */
static ptrdiff_t lying_read(void *ctx, uint8_t *dst, size_t len) {
    (void)ctx;
    dst[0] = 0x92; /* STRING, length 2 */
    return (ptrdiff_t)len + 1;
}

/* --- frames ------------------------------------------------------------------- */

/* Appends one encoded frame to `out`, returning its length. */
static size_t append(uint8_t *out, told_encoder *enc) {
    size_t len = 0;
    const uint8_t *bytes = told_encoder_bytes(enc, &len);
    memcpy(out, bytes, len);
    told_encoder_reset(enc);
    return len;
}

int main(void) {
    told_encoder *enc = told_encoder_create();
    if (!enc) { printf("FAIL: encoder_create returned NULL\n"); return 1; }

    /* Three frames back to back: an object, a 300-byte BUFFER (whose length needs a
     * two-byte length field), and a one-byte TINYUINT. */
    static uint8_t stream[1024];
    size_t sizes[3], len = 0;

    size_t obj;
    told_begin_object(enc, &obj);
    told_write_string(enc, (const uint8_t *)"seq", 3);
    told_write_u64(enc, 4242);
    told_end_object(enc, obj);
    len += sizes[0] = append(stream + len, enc);

    static uint8_t payload[300];
    memset(payload, 'x', sizeof payload);
    told_write_buffer(enc, payload, sizeof payload);
    len += sizes[1] = append(stream + len, enc);

    told_write_u64(enc, 7);
    len += sizes[2] = append(stream + len, enc);

    uint8_t buf[512];
    told_frame frame;

    printf("one byte per read:\n");
    {
        mem_stream m = { stream, len, 0, 1, 0 };
        size_t at = 0;
        for (int i = 0; i < 3; i++) {
            char name[64];
            snprintf(name, sizeof name, "frame %d (%zu bytes)", i, sizes[i]);
            EXPECT_STATUS(name, told_read_frame(mem_read, &m, buf, sizeof buf, &frame), TOLD_OK);
            EXPECT("  its bytes are the ones encoded",
                   frame.raw_len == sizes[i] && memcmp(frame.raw, stream + at, sizes[i]) == 0);
            EXPECT("  and nothing past it was read", m.pos == at + sizes[i]);
            if (i == 1) {
                const uint8_t *data; size_t data_len;
                EXPECT("  the BUFFER's 300 bytes are intact",
                       told_as_buffer(&frame, &data, &data_len) == TOLD_OK &&
                           data_len == sizeof payload && memcmp(data, payload, data_len) == 0);
            }
            at += sizes[i];
        }
        EXPECT_STATUS("then the clean end", told_read_frame(mem_read, &m, buf, sizeof buf, &frame),
                      TOLD_END_OF_STREAM);
        printf("  (%zu read calls for %zu bytes)\n", m.calls, len);
    }

    printf("a FILE*, with bytes after the frames that are not ours:\n");
    {
        FILE *f = tmpfile();
        if (!f) { printf("FAIL: tmpfile\n"); return 1; }
        fwrite(stream, 1, len, f);
        fputs("tail", f);
        rewind(f);

        int read_ok = 1;
        for (int i = 0; i < 3; i++)
            read_ok &= told_read_frame(file_read, f, buf, sizeof buf, &frame) == TOLD_OK;
        EXPECT("three frames read", read_ok);

        uint64_t seven = 0;
        EXPECT("the last decodes to 7", told_as_u64(&frame, &seven) == TOLD_OK && seven == 7);

        char rest[8] = {0};
        EXPECT("the stream is left exactly at the tail",
               fread(rest, 1, sizeof rest - 1, f) == 4 && strcmp(rest, "tail") == 0);
        fclose(f);
    }

    printf("streams that go wrong:\n");
    {
        mem_stream m = { stream, sizes[0] - 1, 0, 64, 0 };
        EXPECT_STATUS("ends inside the data", told_read_frame(mem_read, &m, buf, sizeof buf, &frame),
                      TOLD_TRUNCATED);
    }
    {
        mem_stream m = { stream + sizes[0], 2, 0, 64, 0 };
        EXPECT_STATUS("ends inside the length field",
                      told_read_frame(mem_read, &m, buf, sizeof buf, &frame), TOLD_TRUNCATED);
    }
    {
        mem_stream m = { stream + sizes[0], sizes[1], 0, 64, 0 };
        EXPECT_STATUS("frame bigger than the buffer",
                      told_read_frame(mem_read, &m, buf, 64, &frame), TOLD_FRAME_TOO_LARGE);
        EXPECT("  refused from the length field alone, not after reading the data", m.pos == 3);
    }
    {
        mem_stream m = { stream + sizes[0], sizes[1], 0, 64, 0 };
        EXPECT_STATUS("buffer too small for the length field",
                      told_read_frame(mem_read, &m, buf, 2, &frame), TOLD_FRAME_TOO_LARGE);
    }
    EXPECT_STATUS("callback fails", told_read_frame(failing_read, NULL, buf, sizeof buf, &frame),
                  TOLD_READ_FAILED);
    EXPECT_STATUS("callback over-reports", told_read_frame(lying_read, NULL, buf, sizeof buf, &frame),
                  TOLD_READ_FAILED);
    {
        const uint8_t reserved[] = { 0xD0 };
        mem_stream m = { reserved, sizeof reserved, 0, 64, 0 };
        EXPECT_STATUS("reserved type nibble",
                      told_read_frame(mem_read, &m, buf, sizeof buf, &frame), TOLD_RESERVED_TYPE);
    }
    {
        const uint8_t extended[] = { 0xF0, 0x01, 0xAA };
        mem_stream m = { extended, sizeof extended, 0, 64, 0 };
        EXPECT_STATUS("EXTENDED frame", told_read_frame(mem_read, &m, buf, sizeof buf, &frame),
                      TOLD_EXTENSION_FRAMING_UNKNOWN);
    }
    {
        mem_stream m = { stream, len, 0, 64, 0 };
        EXPECT_STATUS("NULL callback", told_read_frame(NULL, &m, buf, sizeof buf, &frame),
                      TOLD_INVALID_ARGUMENT);
        EXPECT_STATUS("NULL buffer", told_read_frame(mem_read, &m, NULL, sizeof buf, &frame),
                      TOLD_INVALID_ARGUMENT);
        EXPECT_STATUS("zero capacity", told_read_frame(mem_read, &m, buf, 0, &frame),
                      TOLD_INVALID_ARGUMENT);
        EXPECT_STATUS("NULL frame", told_read_frame(mem_read, &m, buf, sizeof buf, NULL),
                      TOLD_INVALID_ARGUMENT);
        EXPECT("  and none of those touched the stream", m.calls == 0);
    }

    told_encoder_destroy(enc);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all stream checks passed\n");
    return 0;
}
