/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Claudio Russo (gattoarancione) */

/* Fuzz target for told_read_frame, the C entry point that reads untrusted streams.
 *
 * A standard libFuzzer entry point, so any libFuzzer- or AFL++-style fuzzer can drive it:
 *
 *     clang -fsanitize=fuzzer,address -Iinclude tests/fuzz_read_frame.c \
 *         zig-out/lib/libtold.a -o fuzz-read-frame
 *
 * With no such toolchain at hand (Zig's clang ships no libFuzzer runtime, nor does Apple's),
 * tests/fuzz_main.c drives it instead: `zig build fuzz-c`.
 *
 * The first bytes of an input set the reader's conditions, the rest is the stream:
 *
 *     [0]     how the callback hands out bytes: 0 = all it is asked for, else at most N
 *     [1..2]  the buffer capacity, little-endian, 1..4096 (0 means 4096)
 *     [3]     fail the read on this call number (0 = never)
 *     [4..]   the stream
 *
 * Every input is held to the contract told.h states, checked against told_decode and
 * told_frame_size reading the same bytes from memory. A broken property aborts, which is
 * how libFuzzer and the driver both recognise a finding. */
#include <told.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CAP 4096

/* Report a broken property and abort. Not assert(): it must hold in release builds too. */
#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "fuzz_read_frame: %s:%d: REQUIRE(%s)\n", __FILE__,        \
                    __LINE__, #cond);                                                 \
            abort();                                                                  \
        }                                                                             \
    } while (0)

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
    size_t chunk;     /* 0 = unlimited */
    size_t fail_on;   /* 1-based call number to fail, 0 = never */
    size_t calls;
    int failed;
    /* The caller's buffer, so the callback can check where it is asked to write. */
    const uint8_t *buf;
    size_t cap;
} stream;

static ptrdiff_t stream_read(void *ctx, uint8_t *dst, size_t len) {
    stream *s = (stream *)ctx;

    /* told_read_frame must never ask for nothing, nor for a byte outside the buffer. */
    REQUIRE(len > 0);
    REQUIRE(dst >= s->buf && len <= s->cap && (size_t)(dst - s->buf) <= s->cap - len);
    /* Nor call again after a read has failed. */
    REQUIRE(!s->failed);

    s->calls++;
    if (s->fail_on != 0 && s->calls == s->fail_on) {
        s->failed = 1;
        return -1;
    }

    size_t n = s->len - s->pos;
    if (n > len) n = len;
    if (s->chunk != 0 && n > s->chunk) n = s->chunk;
    memcpy(dst, s->bytes + s->pos, n);
    s->pos += n;
    return (ptrdiff_t)n;
}

/* Every accessor on a frame returns a status; none may crash, whatever the frame holds. */
static void touch(const told_frame *f) {
    bool b;
    int64_t i;
    uint64_t u;
    double d;
    const uint8_t *p;
    size_t n;
    told_iter it;
    (void)told_as_bool(f, &b);
    (void)told_as_i64(f, &i);
    (void)told_as_u64(f, &u);
    (void)told_as_f64(f, &d);
    (void)told_as_string(f, &p, &n);
    (void)told_as_buffer(f, &p, &n);

    if (told_array_iter(f, &it) == TOLD_OK) {
        told_frame child;
        bool have = true;
        while (have && told_array_next(&it, &child, &have) == TOLD_OK) {}
    }
    if (told_object_iter(f, &it) == TOLD_OK) {
        told_frame value;
        const uint8_t *key;
        size_t key_len;
        bool have = true;
        while (have && told_object_next(&it, &key, &key_len, &value, &have) == TOLD_OK) {}
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;

    static uint8_t buf[MAX_CAP];
    size_t cap = (size_t)data[1] | ((size_t)data[2] << 8);
    cap = cap == 0 ? MAX_CAP : (cap % MAX_CAP) + 1;

    stream s = {
        .bytes = data + 4,
        .len = size - 4,
        .chunk = data[0],
        .fail_on = data[3],
        .buf = buf,
        .cap = cap,
    };

    for (;;) {
        const size_t start = s.pos;
        const uint8_t *rest = s.bytes + start;
        const size_t rest_len = s.len - start;
        told_frame f;
        const told_status status = told_read_frame(stream_read, &s, buf, cap, &f);

        if (s.failed) {
            /* A failed read surfaces as exactly that. */
            REQUIRE(status == TOLD_READ_FAILED);
            return 0;
        }

        if (status == TOLD_OK) {
            /* The frame sits at the start of the buffer, inside the capacity, and is made
             * of the very bytes the stream held, with nothing read past it. */
            REQUIRE(f.raw == buf && f.raw_len <= cap);
            REQUIRE(f.data >= f.raw && f.data + f.data_len == f.raw + f.raw_len);
            REQUIRE(s.pos - start == f.raw_len);
            REQUIRE(memcmp(f.raw, rest, f.raw_len) == 0);

            /* The in-memory decoder reads the same frame off the same bytes. */
            told_frame mem;
            REQUIRE(told_decode(rest, rest_len, &mem) == TOLD_OK);
            REQUIRE(mem.raw_len == f.raw_len && mem.type == f.type && mem.mod == f.mod);
            size_t sized = 0;
            REQUIRE(told_frame_size(rest, rest_len, &sized) == TOLD_OK && sized == f.raw_len);

            touch(&f);
            continue;
        }

        size_t needed = 0;
        const told_status size_status = told_frame_size(rest, rest_len, &needed);

        switch (status) {
        case TOLD_END_OF_STREAM:
            /* Only at a frame boundary with nothing left. */
            REQUIRE(rest_len == 0 && s.pos == s.len);
            return 0;

        case TOLD_TRUNCATED:
            /* The stream ran dry mid-frame: it is used up, and memory agrees it was short. */
            REQUIRE(rest_len > 0 && s.pos == s.len);
            REQUIRE(size_status == TOLD_INCOMPLETE ||
                    (size_status == TOLD_OK && needed > rest_len));
            return 0;

        case TOLD_FRAME_TOO_LARGE:
            /* Either the frame or its length field does not fit the buffer. A buffer too
             * small for the length field refuses before reading it, so memory, reading the
             * whole field, may instead find that the length overflows. */
            REQUIRE(size_status == TOLD_INCOMPLETE ||
                    (size_status == TOLD_OK && needed > cap) ||
                    (size_status == TOLD_LENGTH_OVERFLOW && cap < 9));
            /* And it was refused before the data: at most the header and length field read. */
            REQUIRE(s.pos - start <= 9);
            return 0;

        case TOLD_EXTENSION_FRAMING_UNKNOWN:
            REQUIRE(size_status == TOLD_EXTENSION_FRAMING_UNKNOWN);
            REQUIRE(s.pos - start == 1);
            return 0;

        case TOLD_RESERVED_TYPE:
        case TOLD_INVALID_MOD:
        case TOLD_LENGTH_OVERFLOW:
            /* Refused for what the bytes say, which memory must say too. */
            REQUIRE(size_status == status);
            {
                told_frame mem;
                REQUIRE(told_decode(rest, rest_len, &mem) == status ||
                        told_decode(rest, rest_len, &mem) == TOLD_TRUNCATED);
            }
            return 0;

        default:
            fprintf(stderr, "fuzz_read_frame: unexpected status %s\n", told_status_name(status));
            abort();
        }
    }
}
