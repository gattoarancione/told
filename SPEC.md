# TOLD wire format specification

**Version 1.0**

TOLD (Typo Lenght Data — a play on Type-Length-Data, read as *told*: data is not traded,
mangled or structurally exaggerated, it is simply told) is a self-describing binary
serialization format for streaming structured values over byte streams such as TCP.

This document is normative. The design rationale, and comparisons with JSON, MessagePack,
BSON and CBOR, are in [`doc/TOLD-protocol-paper.md`](doc/TOLD-protocol-paper.md).
Sections 3–8 and 11 and the invariants in section 2 are cited by number from the
reference implementation, so their numbering is stable.

## 1. Conventions

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are to be read as described in
RFC 2119.

- A **frame** is one encoded value: a header byte, an optional length field, and data.
- An **encoder** produces frames; a **decoder** consumes them.
- Byte values are written in hexadecimal, `0x52`; frames as space-separated bytes.
- **L** is the number of data bytes in a frame. **LL** is the number of bytes in its
  length field.
- All multi-byte integers on the wire — values, lengths and floats — are **big-endian**.
  Lengths are unsigned.

## 2. Frame structure and design invariants

```
byte 0         header: TYPE in the high 4 bits, MOD in the low 4 bits
bytes 1..LL    length field, for variable-length types in extended mode only
then L bytes   data; absent for the inline types
```

The header byte is `(TYPE << 4) | MOD`. TYPE selects one of sixteen types (section 3) and
determines how MOD is read; nothing else does.

The format keeps these invariants. Changes to this specification MUST preserve them.

1. Every frame begins with exactly one header byte.
2. TYPE alone determines how MOD is interpreted.
3. Every frame is self-describing: decoding needs no schema or prior negotiation.
4. Byte order and signedness are fixed by this specification, never signalled per frame.
5. A field appears on the wire only if the format genuinely varies along it per frame.
6. Wire shape and semantic contract are independent: types may share framing yet differ
   in what their data promises (STRING and BUFFER, section 7).
7. A value that needs no data bytes takes none.
8. Integers carry only the bytes their magnitude needs; the declared type is a ceiling.
9. Containers are measured in bytes, not elements, so any container can be skipped
   without parsing its contents.
10. A reader can determine a frame's total size from at most its first 9 bytes, without
    examining its data — for every type except EXTENDED (section 8).
11. Decoding a frame never requires bytes beyond its end.

## 3. Types

| TYPE | Name | Family | MOD | Data |
|---|---|---|---|---|
| `0x0` | NULL | inline | ignored (section 4) | none |
| `0x1` | BOOL | inline | the value, 0 or 1 | none |
| `0x2` | TINYUINT | inline | the value, 0–15 | none |
| `0x3` | INT8 | sized integer | data bytes, 0–1 | MOD bytes |
| `0x4` | INT16 | sized integer | data bytes, 0–2 | MOD bytes |
| `0x5` | INT32 | sized integer | data bytes, 0–4 | MOD bytes |
| `0x6` | INT64 | sized integer | data bytes, 0–8 | MOD bytes |
| `0x7` | FLOAT32 | float | 0 or 4 | MOD bytes |
| `0x8` | FLOAT64 | float | 0 or 8 | MOD bytes |
| `0x9` | STRING | variable-length | length or length-of-length (section 7) | UTF-8 text |
| `0xA` | ARRAY | variable-length | as STRING | child frames |
| `0xB` | OBJECT | variable-length | as STRING | key/value frame pairs |
| `0xC` | BUFFER | variable-length | as STRING | opaque bytes |
| `0xD` | reserved | — | — | section 11 |
| `0xE` | reserved | — | — | section 11 |
| `0xF` | EXTENDED | escape | 0 | section 8 |

## 4. Inline types

An inline frame is exactly one byte: the whole value is in MOD.

- **NULL.** Encoders MUST write MOD = 0, so NULL is `0x00`. Decoders MUST ignore MOD and
  read any `0x0_` byte as null.
- **BOOL.** MOD = 0 is false (`0x10`), MOD = 1 is true (`0x11`). Decoders MUST reject
  MOD > 1.
- **TINYUINT.** MOD is an unsigned integer, 0–15: `0x20`–`0x2F`.

## 5. Sized integers

For INT8, INT16, INT32 and INT64, MOD is L, the number of data bytes present. L MUST NOT
exceed the type's width (1, 2, 4 or 8); decoders MUST reject a larger MOD.

The data is the value in two's complement, big-endian, sign-extended from the top bit of
its first byte to 64 bits. L = 0 encodes the value 0.

The declared type is a ceiling, not a size: INT32 holding 300 is `0x52 0x01 0x2C`, three
bytes, and INT16 holding 300 (`0x42 0x01 0x2C`) is equally legal. Encoders SHOULD use the
fewest data bytes that sign-extend back to the value. An encoder MAY declare a wider type
than needed when the declared width carries meaning for the application.

Because the data is sign-extended, a non-negative value whose top data bit would be set
needs one more byte: 200 is `0x42 0x00 0xC8`, not `0x31 0xC8` (which is −56).

## 6. Floats

FLOAT32 and FLOAT64 data is an IEEE 754 binary32 or binary64 value, big-endian.

MOD MUST be 0 or the full width (4 or 8). MOD = 0 encodes positive zero, `+0.0`, in one
byte. Any other MOD is invalid and decoders MUST reject it: a float is never truncated,
since trimming mantissa bytes would silently lose precision.

Negative zero is not `+0.0`, so encoders MUST write it at full width:
`0x88 0x80 0x00 0x00 0x00 0x00 0x00 0x00 0x00`. NaN payloads and infinities are carried
as their bit patterns.

## 7. Variable-length types

STRING, ARRAY, OBJECT and BUFFER share one framing rule:

- **MOD 0–7, inline length.** L = MOD. There is no length field; the data follows the
  header.
- **MOD 8–15, extended length.** LL = MOD − 7, from 1 to 8. The next LL bytes are L, an
  unsigned big-endian integer, and the data follows them.

Every payload under 8 bytes therefore needs no length field, and lengths up to 2⁶⁴ − 1
can be expressed. Encoders SHOULD use the inline form when L ≤ 7 and otherwise the
smallest LL that holds L. Decoders MUST accept any LL from 1 to 8, including one wider
than needed, and an extended length below 8.

A decoder MUST reject a frame whose L, added to the header and length field, exceeds what
it can address, and MUST reject a frame whose declared bytes are not all present.

### 7.1 STRING

The data MUST be valid UTF-8. Encoders MUST NOT write invalid UTF-8 as a STRING, and a
decoder MUST NOT deliver a STRING's data as text without having validated it. L counts
bytes, not characters. There is no terminator.

### 7.2 ARRAY

The data is zero or more complete frames, back to back, that fill exactly L bytes. A
child frame that would extend past the end of the array makes the array malformed.
Elements need not share a type.

Because L counts bytes, a decoder MAY skip an array, or any element in it, without
decoding its contents.

### 7.3 OBJECT

The data is zero or more pairs, each a key frame followed by a value frame, filling
exactly L bytes. Every key MUST be a STRING frame, and may be empty. A key that is not a
STRING, a key without a value, or a pair extending past the end makes the object
malformed.

Pair order on the wire is significant and decoders MUST preserve it. Duplicate keys are
permitted by the encoding; what they mean is left to the application.

### 7.4 BUFFER

The data is opaque bytes with no encoding contract. BUFFER is the type to use when no
more specific type applies, and for any bytes not known to be UTF-8 text.

## 8. EXTENDED

EXTENDED escapes to an application-defined type.

```
0xF0  code  payload...
```

MOD MUST be 0; decoders MUST reject any other MOD, which is reserved for future use. The
byte after the header is the extension's type code, 0–255. The payload that follows is
framed by the extension's own definition, which this specification does not constrain.

A generic decoder can read the code but cannot know where the payload ends, so EXTENDED
is the one type for which invariant 10 does not hold. A decoder that does not know the
extension MUST NOT guess its extent: it SHOULD report the frame as unsupported rather
than continue reading after it. Peers SHOULD use EXTENDED only where both know the
extension, and SHOULD NOT place it inside a container that a generic decoder must skip.

No registry of extension codes is defined yet.

## 9. Encoding

Encoders MUST produce only frames this specification defines, and SHOULD produce the
shortest form of each value:

- TINYUINT for unsigned values 0–15;
- the fewest data bytes for sized integers (section 5);
- the one-byte form for `+0.0` (section 6);
- the inline length when L ≤ 7, else the smallest LL (section 7).

A container's length is only known once its contents are written. An encoder MAY write
the contents first and insert the header in front of them, or compute the size in
advance; the frame is the same either way.

These are recommendations, not a canonical form: two conforming encoders may produce
different bytes for the same value, and decoders MUST accept every legal encoding.

## 10. Decoding and validation

A decoder MUST reject a frame when:

| Condition | Section | Reference implementation |
|---|---|---|
| Fewer bytes are present than the frame declares | 5–7 | `Truncated` |
| MOD is invalid for the type | 4–6, 8 | `InvalidMod` |
| TYPE is `0xD` or `0xE` | 11 | `ReservedType` |
| A child frame or pair extends past the end of its ARRAY or OBJECT | 7.2, 7.3 | `Truncated` |
| An OBJECT key is not a STRING, or has no value | 7.3 | `MalformedContainer` |
| A STRING is not valid UTF-8 | 7.1 | `InvalidUtf8` |
| A length cannot be addressed by the decoder | 7 | `LengthOverflow` |

Decoding a frame reads exactly its own bytes; bytes after it belong to whatever follows.

A decoder MAY decode lazily, checking only the outermost frame and nested frames as they
are visited. Input from an untrusted source SHOULD be validated in full before its
contents are trusted, and decoders SHOULD bound container nesting depth: the format sets
no limit, and unbounded recursion over hostile input is a denial of service.

## 11. Reserved and unspecified

- **TYPE `0xD` and `0xE`** are reserved. Encoders MUST NOT emit them and decoders MUST
  reject them. They are held for future types; see [`ROADMAP.md`](ROADMAP.md).
- **EXTENDED MOD 1–15** is reserved (section 8).
- **There is no unsigned integer type** beyond TINYUINT. Unsigned values above 15 are
  carried in the INT types; a value above 2⁶³ − 1 is written as INT64 with all 8 data
  bytes, and decodes as negative unless the reader reinterprets the 64 bits as unsigned.
  Applications that carry such values MUST agree to do so.
- **Indefinite-length frames** are not defined: every variable-length frame's length is
  known before its header is written. Data of unknown size must be buffered, or split
  into frames by an application-level convention.

## 12. Streams

On a byte stream, frames follow one another with no separator, padding or alignment. By
invariant 10, a reader can buffer exactly one frame: read the header, then the length
field if MOD says one follows, then L bytes.

A stream reader SHOULD cap the length it accepts. A length field is up to eight bytes an
untrusted peer controls, and allocating whatever it declares is a denial of service.
Readers SHOULD reject an oversized frame as soon as its length field is read, before
reading any data. After rejecting a frame, the reader is mid-frame and cannot
resynchronise without an application-level boundary.

## 13. Versioning

This is version 1.0. A version that adds types will assign the reserved TYPE values or
EXTENDED MOD values, and will not change the meaning of any frame valid in 1.0, so a 1.0
decoder rejects new types rather than misreading them. No version information is carried
on the wire; peers agree on the version out of band.

## Appendix A. Test vectors

Each row is a value and its encoding as a conforming encoder following section 9 writes
it. Every one decodes back to its value.

| Value | Encoding |
|---|---|
| null | `00` |
| false | `10` |
| true | `11` |
| 0 | `20` |
| 7 | `27` |
| 15 | `2F` |
| 16 | `31 10` |
| 200 | `42 00 C8` |
| 300 | `42 01 2C` |
| 65536 | `53 01 00 00` |
| −1 | `31 FF` |
| −300 | `42 FE D4` |
| −2⁶³ | `68 80 00 00 00 00 00 00 00` |
| 2⁶⁴ − 1, as unsigned (section 11) | `68 FF FF FF FF FF FF FF FF` |
| +0.0 (FLOAT64) | `80` |
| −0.0 (FLOAT64) | `88 80 00 00 00 00 00 00 00` |
| 1.5 (FLOAT64) | `88 3F F8 00 00 00 00 00 00` |
| 1.5 (FLOAT32) | `74 3F C0 00 00` |
| `""` | `90` |
| `"hi"` | `92 68 69` |
| `"abcdefg"` | `97 61 62 63 64 65 66 67` |
| `"abcdefgh"` | `98 08 61 62 63 64 65 66 67 68` |
| `"é"` | `92 C3 A9` |
| empty BUFFER | `C0` |
| 300-byte BUFFER | `C9 01 2C` followed by the 300 bytes |
| `[]` | `A0` |
| `{}` | `B0` |
| `[1, "a", null]` | `A4 21 91 61 00` |
| `{"z": true, "a": [2]}` | `B7 91 7A 11 91 61 A1 22` |
| EXTENDED, code `0x42`, payload `01 02 03` | `F0 42 01 02 03` |

These decode and are legal, though an encoder following section 9 would not write them:

| Encoding | Value | Why not shortest |
|---|---|---|
| `52 01 2C` | 300 | INT32 declared where INT16 suffices (section 5) |
| `07` | null | NULL with MOD ≠ 0, which decoders ignore (section 4) |
| `98 05 68 65 6C 6C 6F` | `"hello"` | extended length where inline suffices (section 7) |

These MUST be rejected:

| Encoding | Reason |
|---|---|
| `12` | BOOL with MOD 2 |
| `32` | INT8 with MOD 2, above its width |
| `72` | FLOAT32 with partial width |
| `D0`, `E0` | reserved TYPE |
| `F1 42` | EXTENDED with MOD ≠ 0 |
| `52 01` | INT32 declaring 2 data bytes, 1 present |
| `C9 01` | length field incomplete |
| `92 FF FE` | STRING that is not UTF-8, when read as text |
| `A2 94 61` | ARRAY child extending past the array |
| `B2 2A 2B` | OBJECT key that is not a STRING |
| `B3 92 68 69` | OBJECT key with no value |

## Appendix B. Mapping conventions (informative)

Not part of the wire format, but how the reference implementations in Zig, C, C++ and
Python map native values, so that each reads what the others write:

- a record (struct, dataclass) becomes an OBJECT keyed by field name, in declaration
  order; a positional variant becomes an ARRAY;
- an enumeration becomes its integer value; decoders also accept its name as a STRING;
- an optional value becomes NULL or the value;
- text becomes STRING and binary data BUFFER;
- non-negative integers use the unsigned encoding (TINYUINT for 0–15), negative ones the
  shortest INT type;
- on decoding, keys matching no field are ignored, and a missing field takes its default
  or is an error.

The golden frame `src/testdata/interop.tld` is encoded by all four implementations and
must decode and re-encode byte for byte in each.
