# TOLD: A Nibble-Unified Binary Encoding for Compact TCP Streaming

## Abstract

TOLD (Type-Length-Data — read as 'told': data is not traded, mangled, or structurally exaggerated, it is simply told) is a binary serialization format designed for streaming structured data over TCP with minimal framing overhead. Its central design principle is a single header byte whose high nibble identifies a value's type and whose low nibble is overloaded, by type family, to carry either the value itself, its byte length, or the length of its length field. This paper introduces the design principles behind TOLD, specifies its frame structure, compares it against established binary formats (MessagePack, BSON, CBOR) and text formats (JSON), discusses representative application domains, and concludes with an assessment of its strengths and limitations.

---

## 1. Introduction

Structured data exchanged between systems is conventionally serialized in one of two ways: as human-readable text (JSON, XML) or as a binary encoding optimized for size and parse speed (MessagePack, Protocol Buffers, BSON, CBOR). Text formats are easy to inspect and debug but carry substantial overhead — field delimiters, quoting, whitespace, and decimal-text number encoding all inflate a payload well beyond its information content. Binary formats close this gap, but many existing binary schemes allocate a fixed-size length field regardless of whether the value is large or small, or dedicate large ranges of the type space unevenly, producing inconsistent overhead across value shapes.

TOLD was designed around one governing question: **what is the minimum number of bytes needed before a receiver knows how many more bytes to buffer?** Every part of the frame format follows from answering that question as economically as possible for each type family, rather than applying one uniform length-encoding rule across all types.

---

## 2. Design Principles

TOLD rests on four principles, arrived at incrementally by identifying and removing redundancy at each step of the design process.

### 2.1 Self-description over external schema

Every frame carries its own type. A receiver never needs an external schema, IDL, or prior negotiation to know what a value is — the header byte alone answers "what kind of value is this." This trades a small per-value overhead (the header byte) for the ability to parse any TOLD stream without shared context beyond the spec itself.

### 2.2 The header byte should do as much work as possible

A single byte has 256 states. Rather than spending an entire byte purely on a type tag, TOLD splits it into two 4-bit fields — `TYPE` and `MOD` — and uses `MOD`'s meaning differently per type family:

- For **inline types** (null, bool, small unsigned integers), `MOD` *is* the value. No further bytes are read at all.
- For **sized numeric types** (int8–int64, float32/64), `MOD` is the actual number of data bytes present, which may be fewer than the type's nominal width.
- For **variable-length types** (string, array, object, buffer), `MOD`'s top bit switches between an inline length (0–7, no further length field) and an extended length (a length-of-length, for anything larger).

This means the "wasted" work that a naive design would spend on a separate, always-present length field is eliminated for the common cases, and only paid for when the payload actually requires it.

### 2.3 Redundant fields must justify their existence with real flexibility

An earlier iteration of this design included a `TL` (type-of-length) field, describing whether a length was big-endian, little-endian, or varint-encoded. This field was removed once it became clear that a protocol-wide fixed convention (always big-endian, always unsigned) makes the field pure redundancy — it described a choice that was never actually exercised on a per-message basis. The design principle generalized from this: **a field earns its place only if the protocol genuinely varies along that dimension per message; otherwise it belongs in the specification, not on the wire.**

### 2.4 Typed and untyped data are not the same thing, even when their framing is identical

TOLD distinguishes `STRING` (a contract: valid UTF-8 text) from `BUFFER` (a contract: none — opaque bytes). Both share byte-for-byte identical framing rules, but collapsing them into one type would force every consumer to either validate opaque binary data as text (which fails for most real binary payloads) or lose the guarantee that a STRING is safely decodable as text. The lesson generalizes: **wire-level shape and semantic contract are independent axes, and a format should not conflate them merely because their framing happens to coincide.**

---

## 3. Protocol Structure

### 3.1 Frame layout

```
byte 0        : header byte = [ TYPE (4 bits) | MOD (4 bits) ]
bytes 1..N    : optional length field (LL bytes; variable-length types in extended mode only)
bytes N+1..M  : data (L bytes; absent for NULL/BOOL/TINYUINT)
```

### 3.2 Type table and the meaning of each field

| Nibble | Type | `MOD` meaning | Notes |
|---|---|---|---|
| `0x0` | NULL | unused | value is null; 1 byte total |
| `0x1` | BOOL | inline value (0/1) | 1 byte total |
| `0x2` | TINYUINT | inline value (0–15) | 1 byte total |
| `0x3`–`0x6` | INT8/16/32/64 | actual byte count used | sign-extended; `MOD=0` → value 0 |
| `0x7`–`0x8` | FLOAT32/64 | leading byte count (0–4 / 0–8) | trailing zero bytes omitted and restored as zeros, so trimming is lossless; `MOD=0` → value 0.0 (since 1.1) |
| `0x9` | STRING | inline length (0–7) or extended mode flag | UTF-8 contract |
| `0xA` | ARRAY | same as STRING | data = concatenated child frames |
| `0xB` | OBJECT | same as STRING | data = concatenated (key, value) pairs |
| `0xC` | BUFFER | same as STRING | opaque; the default/fallback type |
| `0xD`–`0xE` | reserved | — | — |
| `0xF` | EXTENDED | unused | next byte = application-defined type code |

**Meaning of the length split for variable types**: `MOD` values `0x0`–`0x7` mean "the length is these three bits, 0 to 7, no more fields needed." `MOD` values `0x8`–`0xF` mean "subtract 7 from `MOD` to get `LL`, the number of bytes the *actual* length occupies, which follows next." This gives every payload under 8 bytes a length-field-free encoding, while still permitting lengths up to 2⁶⁴−1 for large payloads.

### 3.3 Worked examples

```
true                    -> 0x11                              (1 byte)
uint 7                   -> 0x27                              (1 byte)
int32 value 300           -> 0x52 0x01 0x2C                     (3 bytes; ceiling is 4, only 2 used)
float64 0.0                -> 0x80                                (1 byte)
float64 1.5                -> 0x82 0x3F 0xF8                      (3 bytes; trailing zeros omitted)
empty string                 -> 0x90                              (1 byte)
"hi"                          -> 0x92 0x68 0x69                    (3 bytes, inline length)
300-byte buffer                -> 0xC9 0x01 0x2C + 300 bytes          (extended length, LL=2)
```

### 3.4 Composite types are self-skippable

Because `ARRAY` and `OBJECT` store a total byte length rather than an element count, a decoder that only needs to route or skip a composite value can do so in constant time from the length field alone, without parsing any child frame. Decoding into child values is only necessary for a consumer that actually needs the contents.

---

## 4. Comparison with Existing Formats

### 4.1 JSON (text)

JSON is the baseline most JSON-like binary formats, including TOLD, implicitly compare against. JSON's costs are structural: every key is a quoted string repeated per object, every number is decimal text (often several bytes for what a binary encoding stores in one or two), and there is no way to express raw binary data without a lossy escape such as Base64, which inflates it by roughly a third. TOLD (and its binary peers) exist specifically to remove this overhead while preserving JSON's aptitude for representing nested, self-describing, dynamically shaped data.

### 4.2 MessagePack

MessagePack is the closest relative to TOLD in spirit: it also devotes portions of a header byte to small inline values (`fixint`, `fixstr`, `fixarray`, `fixmap`) and falls back to explicit length fields for larger payloads. The two diverge in structure and in one specific mechanism:

- MessagePack's header byte allocates ranges unevenly across the full byte (`0xc0`–`0xdf` are individually assigned to distinct types), rather than the systematic 4-bit/4-bit split TOLD uses uniformly across every type.
- MessagePack has no equivalent of TOLD's integer byte-trimming: a value tagged `int32` in MessagePack is always the full 4 bytes; TOLD's `MOD` field lets an `INT32`-ceiling value use only as many bytes as it actually needs (down to 0 for the value zero), independent of the declared type.
- MessagePack's inline-length ceiling for strings/arrays/maps (31 bytes, 15 elements) is larger than TOLD's (7 bytes), because MessagePack spends more header bits on the length itself and fewer on type variety in that byte range.
- Both formats make the same choice to keep string and raw-binary types distinct despite near-identical framing, for the same UTF-8-contract reasoning given in §2.4.

MessagePack is materially more mature: standardized extension type numbering, native timestamp support, and broad multi-language library support that TOLD, as a from-scratch design, does not yet have.

### 4.3 BSON

BSON (used natively by MongoDB) prioritizes different properties than either JSON or TOLD: it is designed for fast field traversal and in-place document mutation within a database engine, not primarily for minimal wire size. Its documents carry an explicit total document length up front (useful for allocation) and store strings with an explicit UTF-8 byte length *and* a trailing null terminator, which is redundant by design for compatibility with C-string tooling. BSON does not attempt small-value inlining the way TOLD and MessagePack do — every value type has a comparatively fixed, non-trimmed encoding, favoring predictable field offsets over minimal size. TOLD's approach is essentially the inverse trade: it sacrifices constant-offset field access (since frame sizes vary with content) in exchange for substantially smaller encodings of typical small values.

### 4.4 CBOR

CBOR (RFC 8949) is architecturally the nearest published analog to TOLD's design philosophy: it also uses a major-type (3-bit) plus additional-information (5-bit) split in its initial byte, with the additional-information field playing exactly the same overloaded role as TOLD's `MOD` — small integers inline, or a byte-count flag for following length/value fields. CBOR's major difference is a richer additional-information encoding (values 24–27 map to explicit 1/2/4/8-byte follow-on fields, value 31 signals indefinite-length/streaming), and a wider standardized tag mechanism for semantic extensions (dates, big numbers, etc.) layered on top of the base types. TOLD's variable-length inline ceiling of 7 bytes (using a single reserved bit as the mode flag) is a narrower but simpler mechanism than CBOR's more granular 1/2/4/8/indefinite scheme; CBOR also natively supports indefinite-length (streaming, unknown-size-at-encode-time) frames, a capability TOLD explicitly does not yet define (see §6).

### 4.5 Summary comparison table

| Property | JSON | MessagePack | BSON | CBOR | TOLD |
|---|---|---|---|---|---|
| Human-readable | Yes | No | No | No | No |
| Small-value inlining | No | Yes | No | Yes | Yes |
| Integer byte-trimming within one type tag | No | No | No | Partial (via additional-info) | Yes |
| Distinct text/binary types | N/A (no binary) | Yes | Yes | Yes | Yes |
| Self-skippable composites | No (must parse) | Partial | Yes (length-prefixed docs) | Partial | Yes |
| Streaming/unknown-length frames | N/A | No | No | Yes | Not yet defined |
| Extension mechanism | No | Yes (ext type) | No | Yes (tags) | Yes (EXTENDED escape) |
| Ecosystem maturity | Very high | High | High (DB-specific) | Moderate-high | None (new design) |

---

## 5. Application Fields

**Embedded and vehicle networking.** Systems that already reason in frames and fixed byte budgets — CAN bus gateways, telemetry aggregators, sensor-to-controller links — benefit from TOLD's small fixed overhead for scalar values (a sensor reading, a status flag, a small counter) without requiring a schema compiler or IDL toolchain, which is often impractical on constrained embedded targets.

**RPC and service-to-service messaging.** Structured request/response payloads (an OBJECT of typed fields) benefit from TOLD's ability to keep small integers, booleans, and short string fields down to one to a few bytes each, which matters cumulatively across high-frequency internal RPC traffic where JSON's per-field text overhead adds up.

**Binary blob transport alongside structured metadata.** Because `BUFFER` is a first-class type with no interpretation cost, a single TOLD `OBJECT` can carry structured metadata fields (typed, validated) alongside an opaque payload (a compressed chunk, an image, a serialized sub-format) in one self-describing envelope, without forcing the opaque data through a text-safe encoding such as Base64.

**Log and event streaming over a persistent TCP connection.** TOLD's frame-boundary property — a decoder always knows exactly how many more bytes it needs before it can proceed — maps directly onto TCP's partial-read, byte-stream semantics, making it a natural fit for a persistent connection carrying a continuous sequence of discrete typed events.

---

## 6. Strengths and Weaknesses

### Strengths

- **Minimal overhead for the most common value shapes.** Null, boolean, small unsigned integers, empty or short strings/buffers, and zero-valued numbers all encode in a single byte, with no field wasted on values that don't need it.
- **Uniform, learnable structure.** Every type family follows the same underlying rule — the low nibble always answers "how much more do I need to read" — rather than a patchwork of per-type exceptions, which simplifies both implementation and review.
- **Integer byte-trimming decoupled from declared type.** A value can be tagged with a generous type ceiling (e.g. INT64) while still costing only as many bytes as its actual magnitude requires, which most peer formats do not offer within a single type tag.
- **Self-skippable composites.** Length-prefixed arrays and objects allow a decoder to skip or route a nested structure without parsing its contents, which is valuable for intermediaries and partial-interest consumers.
- **Clean separation of framing from semantic contract.** STRING and BUFFER demonstrate that identical wire mechanics can carry different guarantees without conflating the two, avoiding a common source of ambiguity in simpler formats.
- **Natural fit for TCP's byte-stream model.** The protocol makes no assumption about transport-level packet boundaries, and its incremental "read N more bytes to know the next N" structure maps cleanly onto partial socket reads.

### Weaknesses

- **No streaming/indefinite-length frame support.** Every variable-length frame requires its total size to be known before the header is written, unlike CBOR's indefinite-length mechanism. Data whose size is not known upfront (a live sensor feed, a generator-produced sequence) cannot be framed without buffering it first or wrapping it in an application-level chunking convention.
- **No native unsigned integer family.** Only TINYUINT (0–15) is unsigned; anything larger must be carried as a signed INT type and reinterpreted at the application layer, which is a gap relative to formats with explicit unsigned types.
- **Two reserved type slots with no defined use.** `0xD` and `0xE` are presently unused, which is deliberate headroom but also unresolved surface area in the specification.
- **No ecosystem.** Unlike MessagePack, BSON, or CBOR, TOLD has no existing libraries, no interoperability with other systems, no established extension-type registry, and no production hardening. Every consumer must be built from this specification.
- **Variable frame sizes preclude constant-offset field access.** Formats like BSON that accept larger fixed encodings in exchange for predictable byte offsets support use cases (in-place mutation, indexed field lookup) that TOLD's size-optimized, variable-width frames do not support as naturally.
- **Extension mechanism is under-specified.** The `EXTENDED` escape reserves the *mechanism* for application-defined types but does not itself standardize a length/data convention the way CBOR's tag system or MessagePack's `ext` family do, leaving interoperability between independently authored extensions unresolved.

---

## 7. Conclusion

TOLD demonstrates that a small set of consistent design rules — inline the common case, escape to an explicit length field only when necessary, and keep semantic contract separate from wire shape — can produce a binary encoding competitive in density with established formats such as MessagePack and CBOR, while remaining conceptually simpler than either at the level of its core frame structure. Its principal costs are the ones inherent to any new format: the absence of an ecosystem, no defined mechanism for unknown-length streaming, and specification gaps (unsigned integers, extension-type conventions) that mature formats have already closed. TOLD is best understood not as a replacement for MessagePack or CBOR in general-purpose use, but as a worked demonstration of how far header-byte overloading can be pushed toward minimal overhead when a format is designed from first principles for a specific transport (TCP) and a specific goal (minimum bytes for typical small structured values).
