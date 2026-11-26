# TOLD roadmap

Where the format and its implementations could go next. Nothing here is committed to yet:
each item says what it buys and what it costs, so the choices can be made deliberately.

## The specification

The format is specified in [`SPEC.md`](SPEC.md), version 1.1, with the rationale in
[`doc/TOLD-protocol-paper.md`](doc/TOLD-protocol-paper.md). Every wire-format change
below amends the spec and bumps its version.

## What the format already does

- Inline values: null, true/false and the integers 0–15 are one byte.
- Integers carry only the bytes they need; the declared width is only a ceiling.
- Floats omit their trailing zero bytes (1.1): `0.0` is one byte, `1.5` three, `-0.0` two.
- Strings, buffers and containers up to 7 bytes need no length field; longer ones use the
  shortest of 1–8 length bytes.
- Zero-copy decoding: frames borrow the caller's bytes, nothing is allocated.
- Stream framing: at most 9 bytes tell a reader how long a frame is.

## Wire-format changes

These compete for very little space: two reserved type nibbles (`0xD`, `0xE`) and the
unused MOD values of EXTENDED. NULL's MOD looks free but is not — decoders ignore it, so
an old decoder would silently read any new meaning there as null.

| # | Change | Gain | Cost |
|---|---|---|---|
| 1 | **Typed arrays** (`0xD`): one header and an element type, then the elements packed back to back, e.g. `f32[1024]` | 1 byte saved per element — about 20% for f32, 50% for i8 — and the payload decodes as a zero-copy slice. Fits the lidar, ZED and CAN payloads qnnt carries | One reserved nibble |
| 2 | **Unsigned integers** (`0xE`, MOD = data bytes) | Closes the documented gap where values above 2⁶³ − 1 decode as negative | One reserved nibble, competing with #1 and #3 |
| 3 | **Small negative integers** in one byte, e.g. −16…−1 | `-1` goes from 2 bytes to 1 | A nibble, or a sub-range of #2 |
| 4 | **Key references**: a key already seen in the frame is sent as a one-byte index | Arrays of structs stop repeating `"channel"`, `"value"` per element — often the largest single saving for telemetry | Decoders keep a per-frame key table; keys are no longer free zero-copy slices |
| 5 | **Length-prefixed EXTENDED** (MOD 1 = a length follows) | Generic decoders can skip unknown extensions; EXTENDED is today the one frame `frameSize` cannot measure | EXTENDED MOD space; backward-compatible, since MOD ≠ 0 is refused today |
| 6 | **Half-precision floats** | Halves sensor floats where f16 is enough | A type of its own, or an EXTENDED code |

## Implementation changes, no wire change

| # | Change | Gain |
|---|---|---|
| 7 | **Float64 narrowing**, the remainder of the original #7: write a float64 exactly representable as float32 as one, when that is shorter. Trailing-zero floats (done in 1.1) already captured most of the gain; narrowing still saves a byte on values like `21.5` (`83 40 35 80` → `72 41 AC`). It changes the declared type, so readers that insist on FLOAT64 need to accept either | Occasionally one more byte |
| 8 | **No memmove on container close**: `end()` currently shifts the container's whole contents to insert its header. Reserve the header space up front, or add `beginArray(len)` for callers that know the count | Nested containers stop costing depth × size in moves |
| 9 | **Python decodes in one call**: `told.loads` crosses into C several times per value, which its own docstring calls fine for messages but not for megabytes. One call returning a flat token list would end that | Large-frame decoding in Python, by an estimated order of magnitude; to be measured |
| 10 | **SIMD UTF-8 validation** in `validate` and `asString` | Faster text-heavy frames |
| 11 | **Canonical mode**, with optionally sorted keys | Stable bytes for hashing and deduplication; the fuzz tests already prove the encoder's output is a fixed point |

## Suggested order

1. **#8.** No compatibility risk, measurable gains, and the fuzzers and the four-language
   golden frame catch any regression. (Trailing-zero floats, the bulk of #7, shipped in
   1.1: the golden frame went from 151 to 127 bytes.)
2. **#5.** Backward-compatible, and removes the only frame nothing can skip.
3. **Decide what `0xD` and `0xE` are for.** The recommendation: `0xD` for typed arrays (#1),
   the best fit for qnnt's sensor payloads, and `0xE` for integers that #2 and #3 share —
   unsigned in MOD 0–8, small negatives in the MOD values left over.
4. **#4 and #9** once a benchmark shows where the time and bytes go on real payloads.

Every wire change ships with a spec bump, new golden frames decoded in all four
languages, and fuzz-corpus seeds for the new type.
