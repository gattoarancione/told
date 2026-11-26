/* A seeded mutational driver for tests/fuzz_read_frame.c, for toolchains without a
 * libFuzzer runtime. `zig build fuzz-c` runs it.
 *
 *     fuzz-read-frame [--iterations N] [--seed S]   generate and mutate N inputs
 *     fuzz-read-frame FILE...                       replay inputs, e.g. a saved crash
 *
 * Inputs are built the way the target reads them: four bytes of reader conditions, then
 * a stream of frames produced by the real encoder, then a few mutations -- so most inputs
 * reach deep into told_read_frame, and the rest exercise every way a stream can be wrong.
 * A finding aborts inside the target; the handler below saves the input that caused it. */
#include <told.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_INPUT 8192

/* --- reproducibility ------------------------------------------------------------- */

static uint64_t rng_state;

/* splitmix64: small, fast, and the same sequence on every platform. */
static uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static size_t below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static uint8_t current[MAX_INPUT];
static size_t current_len;
static unsigned long long current_seed, current_iteration;

/* Save the input that broke a property, then die as the signal intended. Not strictly
 * async-signal-safe, which a test tool on its way out can afford. */
static void on_crash(int sig) {
    char name[96];
    snprintf(name, sizeof name, "fuzz-crash-%llu-%llu.bin", current_seed, current_iteration);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(current, 1, current_len, f);
        fclose(f);
    }
    fprintf(stderr, "\nfuzz failure at iteration %llu of --seed %llu; input saved to %s\n"
                    "replay with: zig build fuzz-c -- %s\n",
            current_iteration, current_seed, name, name);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_handlers(void) {
    signal(SIGABRT, on_crash);
    signal(SIGSEGV, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGFPE, on_crash);
#ifdef SIGBUS
    signal(SIGBUS, on_crash);
#endif
#ifdef SIGTRAP
    signal(SIGTRAP, on_crash); /* UBSan in trap mode */
#endif
}

/* --- generating streams ----------------------------------------------------------- */

static const char *const pieces[] = {"a", "z", "0", " ", "\"", "\\", "\n", "\xC3\xA9", "\xE2\x82\xAC"};

static void gen_string(told_encoder *enc) {
    char text[64];
    size_t n = 0;
    for (size_t count = below(10); count > 0; count--) {
        const char *p = pieces[below(sizeof pieces / sizeof pieces[0])];
        size_t len = strlen(p);
        if (n + len > sizeof text) break;
        memcpy(text + n, p, len);
        n += len;
    }
    told_write_string(enc, (const uint8_t *)text, n);
}

static void gen_value(told_encoder *enc, int depth) {
    static uint8_t bytes[400];
    size_t n, mark;
    switch (below(depth > 0 ? 11 : 9)) {
    case 0: told_write_null(enc); break;
    case 1: told_write_bool(enc, rnd() & 1); break;
    case 2: told_write_u64(enc, rnd() >> below(64)); break;
    case 3: told_write_i64(enc, (int64_t)rnd() >> below(64)); break;
    case 4: told_write_f64(enc, (double)(int64_t)rnd() / 1e6); break;
    case 5: gen_string(enc); break;
    case 6:
        /* Sometimes past 255 bytes, where the length field takes two bytes. */
        n = below(rnd() & 1 ? 16 : sizeof bytes);
        for (size_t k = 0; k < n; k++) bytes[k] = (uint8_t)rnd();
        told_write_buffer(enc, bytes, n);
        break;
    case 7:
        n = below(4);
        for (size_t k = 0; k < n; k++) bytes[k] = (uint8_t)rnd();
        told_write_extended(enc, (uint8_t)rnd(), bytes, n);
        break;
    case 8: told_write_f32(enc, (float)below(1000) / 8.0f); break;
    case 9:
        told_begin_array(enc, &mark);
        for (size_t k = below(5); k > 0; k--) gen_value(enc, depth - 1);
        told_end_array(enc, mark);
        break;
    default:
        told_begin_object(enc, &mark);
        for (size_t k = below(4); k > 0; k--) {
            gen_string(enc);
            gen_value(enc, depth - 1);
        }
        told_end_object(enc, mark);
        break;
    }
}

/* Four bytes of reader conditions, biased towards the interesting ones: small buffers, a
 * byte or two per read, and an occasional failed read. */
static void gen_conditions(uint8_t *out) {
    static const uint8_t chunks[] = {0, 0, 1, 1, 2, 3, 7, 16};
    out[0] = chunks[below(sizeof chunks)];
    size_t cap = below(4) == 0 ? below(12) : below(4096);
    out[1] = (uint8_t)cap;
    out[2] = (uint8_t)(cap >> 8);
    out[3] = below(8) == 0 ? (uint8_t)(1 + below(20)) : 0;
}

static void gen_stream(told_encoder *enc) {
    current_len = 4;
    gen_conditions(current);
    for (size_t frames = 1 + below(4); frames > 0; frames--) {
        told_encoder_reset(enc);
        gen_value(enc, (int)below(5));
        size_t len;
        const uint8_t *bytes = told_encoder_bytes(enc, &len);
        if (current_len + len > MAX_INPUT) break;
        memcpy(current + current_len, bytes, len);
        current_len += len;
    }
}

/* --- mutating --------------------------------------------------------------------- */

static void mutate(void) {
    size_t len = current_len;
    switch (below(8)) {
    case 0: /* flip a bit */
        if (len) current[below(len)] ^= (uint8_t)(1u << below(8));
        break;
    case 1: /* a header for any type and MOD */
        if (len) current[below(len)] = (uint8_t)rnd();
        break;
    case 2: /* insert a byte */
        if (len < MAX_INPUT) {
            size_t at = below(len + 1);
            memmove(current + at + 1, current + at, len - at);
            current[at] = (uint8_t)rnd();
            current_len++;
        }
        break;
    case 3: /* delete a byte */
        if (len) {
            size_t at = below(len);
            memmove(current + at, current + at + 1, len - at - 1);
            current_len--;
        }
        break;
    case 4: /* truncate */
        if (len) current_len = below(len);
        break;
    case 5: /* duplicate a run */
        if (len) {
            static uint8_t run[MAX_INPUT];
            size_t start = below(len);
            size_t n = below(len - start + 1);
            if (n > MAX_INPUT - len) n = MAX_INPUT - len;
            memcpy(run, current + start, n);
            size_t at = below(len + 1);
            memmove(current + at + n, current + at, len - at);
            memcpy(current + at, run, n);
            current_len += n;
        }
        break;
    case 6: /* splice in a header with an 8-byte length near 2^64, which random bytes
             * would essentially never produce */
        if (len + 9 <= MAX_INPUT) {
            static const uint8_t huge[] = {0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8};
            size_t at = 4 + below(len > 4 ? len - 4 + 1 : 1);
            if (at > len) at = len;
            memmove(current + at + sizeof huge, current + at, len - at);
            memcpy(current + at, huge, sizeof huge);
            current[at + sizeof huge - 1] |= (uint8_t)below(8);
            current_len += sizeof huge;
        }
        break;
    default: /* new reader conditions over the same stream */
        if (len >= 4) gen_conditions(current);
        break;
    }
}

/* --- main ------------------------------------------------------------------------- */

static int replay(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    current_len = fread(current, 1, MAX_INPUT, f);
    fclose(f);
    LLVMFuzzerTestOneInput(current, current_len);
    printf("%s: %zu bytes, no property broken\n", path, current_len);
    return 0;
}

int main(int argc, char **argv) {
    unsigned long long iterations = 100000, seed = 1;
    int replayed = 0;
    install_handlers();

    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--iterations") == 0 && k + 1 < argc) {
            iterations = strtoull(argv[++k], NULL, 10);
        } else if (strcmp(argv[k], "--seed") == 0 && k + 1 < argc) {
            seed = strtoull(argv[++k], NULL, 10);
        } else {
            if (replay(argv[k]) != 0) return 1;
            replayed = 1;
        }
    }
    if (replayed) return 0;

    told_encoder *enc = told_encoder_create();
    if (!enc) return 1;
    rng_state = current_seed = seed;

    for (current_iteration = 0; current_iteration < iterations; current_iteration++) {
        gen_stream(enc);
        /* A quarter go in untouched, so well-formed streams are read end to end. */
        if (below(4) != 0)
            for (size_t rounds = 1 + below(5); rounds > 0; rounds--) mutate();
        LLVMFuzzerTestOneInput(current, current_len);
    }

    told_encoder_destroy(enc);
    printf("told_read_frame: %llu inputs from --seed %llu, no property broken\n", iterations,
           seed);
    return 0;
}
