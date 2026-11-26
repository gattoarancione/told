# TOLD

**TOLD** — *Typo Lenght Data*. Yes, the typo is the name: a joke on Type-Length-Data.

A compact binary serialization format in the TLV family, implemented in Zig, with a C ABI,
a header-only C++ wrapper and a Python package on top. It began life inside qnnt as its frame codec and is
carved out here as a library of its own.

- **Zero-copy, allocation-free decoding.** `decode` returns a `Frame` borrowing your bytes;
  containers are walked with iterators that decode children on demand.
- **Minimal encodings.** Small integers fit in the header byte, sized ints carry only
  the bytes they need, `0.0` is one byte, short strings need no length field.
- **Stream friendly.** At most 9 bytes of prefix tell a reader exactly how long a frame
  is (`frameSize`, `readFrame`), so frames can span read boundaries without guessing.
- **Struct mapping** in every language: `@typeInfo` in Zig, `TOLD_DEFINE_MAP` in C++,
  descriptor tables in C, dataclasses in Python. All of them are pinned to one golden frame,
  `src/testdata/interop.tld`.

## The wire format, in one screen

A frame is one header byte — high nibble **TYPE**, low nibble **MOD** — then an optional
length field, then the data. MOD's meaning depends on the type family:

| Nibble | Type | MOD means | Frame |
|---|---|---|---|
| `0x0` | NULL | ignored | 1 byte |
| `0x1` | BOOL | the value, 0 or 1 | 1 byte |
| `0x2` | TINYUINT | the value, 0..15 | 1 byte |
| `0x3`–`0x6` | INT8/16/32/64 | data byte count, 0..width; big-endian, sign-extended | 1 + MOD |
| `0x7`, `0x8` | FLOAT32/64 | 0 (the value +0.0) or the full width | 1 or 1 + width |
| `0x9` | STRING | ≤ 7: the length; ≥ 8: length-of-length `LL = MOD − 7` | 1 [+ LL] + L |
| `0xA` | ARRAY | as STRING; data is back-to-back child frames | |
| `0xB` | OBJECT | as STRING; data is STRING key / value pairs, in order | |
| `0xC` | BUFFER | as STRING; opaque bytes, no UTF-8 contract | |
| `0xD`, `0xE` | reserved | rejected | |
| `0xF` | EXTENDED | 0; next byte is an application type code | extension-defined |

So `true` is `0x11`, `7` is `0x27`, `300` is `0x42 0x01 0x2C`, and `"hi"` is
`0x92 'h' 'i'`.

## Using it

Requires **Zig 0.16.0**.

```sh
zig build            # libtold.a, libtold.{so,dylib}, told.h, told.hpp into zig-out/
zig build test       # Zig tests, the C/C++ programs that check the headers, Python
```

### Zig

Add it as a dependency (`zig fetch --save <url>` once it is published, or a `.path`
entry in `build.zig.zon` for now), then:

```zig
// build.zig
const told = b.dependency("told", .{ .target = target, .optimize = optimize });
exe_mod.addImport("told", told.module("told"));
```

```zig
const told = @import("told");

const Reading = struct { sensor: []const u8, seq: u32, celsius: f32, ok: bool = true };

var buf: std.ArrayList(u8) = .empty;
defer buf.deinit(gpa);
try told.packTo(gpa, &buf, Reading{ .sensor = "t1", .seq = 7, .celsius = 21.5 });
const back = try told.unpackFrom(Reading, buf.items, null); // null: borrow from buf

// Or by hand:
var e = told.Encoder.init(gpa, &buf);
const obj = e.beginObject();
try e.writeKey("seq");
try e.writeUint(4242);
try e.end(obj);

const frame = try told.validate(buf.items, 32); // decode + check every nested frame
var it = try frame.objectIterator();
while (try it.next()) |kv| std.debug.print("{s}\n", .{kv.key});
```

`decode` checks only the outermost frame; use `validate` on untrusted input.

### C and C++

Link `libtold` and include `told.h` (C, `told_*` functions) or `told.hpp` (C++20,
`namespace told`). See `examples/` — `told_demo.c` and `told_demo.cpp` walk the whole API,
`told_map.c` and `told_map.cpp` show struct mapping. Run any of them with
`zig build told-demo-c` and so on.

A downstream Zig library that exposes TOLD frames through its own C ABI should import the
`told_abi` module for the `told_frame` layout rather than `told_c.zig`, so the `told_*`
symbols are not compiled into both archives.

### Python

`python/told` is a ctypes binding over `libtold`, so the wire format has one
implementation. It needs Python 3.10+.

```python
from dataclasses import dataclass

import told

data = told.dumps({"seq": 4242, "raw": b"\x0a\xff"})  # bytes -> BUFFER, str -> STRING
told.validate(data)
value = told.loads(data)

@dataclass
class Reading:
    channel: int
    value: float
    valid: bool = True

back = told.unpack(Reading, told.pack(Reading(1, 3.5)))
```

`zig build wheel` bundles the library into a platform wheel in `zig-out/wheel/`, and
`zig build wheel-test` installs it in a clean venv and runs the tests from there. From a
checkout the package finds `zig-out/lib` on its own; `TOLD_LIBRARY` overrides the path.

## Layout

```
src/told.zig         the codec: Frame, decode, validate, Encoder, readFrame, pack/unpack
src/told.test.zig    its tests
src/told_c.zig       the C ABI (root of libtold)
src/told_abi.zig     the C frame layout, no exports
src/told_alloc.zig   the C ABI's allocator, chosen by build mode
src/testdata/        the golden cross-language frame
include/             told.h, told.hpp
examples/            C and C++ programs, all run by `zig build test`
python/told/         the Python package
python/test_told.py  its tests, also run by `zig build test`
```

## License

MIT — see `LICENSE`.
