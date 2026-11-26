//! TOLD (Typo Lenght Data) binary streaming protocol, v1.0.
//!
//! A frame is one header byte — high nibble TYPE, low nibble MOD — followed by an
//! optional length field and the data. What MOD means depends on the type family:
//! an inline value (NULL/BOOL/TINYUINT), a data byte count (sized ints/floats), or an
//! inline length / length-of-length (STRING/ARRAY/OBJECT/BUFFER).
//!
//! The decoding side is deliberately zero-copy and allocation-free: `decode` returns a
//! `Frame` borrowing the caller's bytes, and containers are walked with iterators that
//! decode children on demand. Nothing here needs an allocator except the `Encoder`
//! (which appends to a caller-owned list) and `readFrame` (which buffers one frame off
//! a stream).
//!
//! This module depends on nothing but std. It is the `told` module in build.zig; the C ABI
//! over it is `told_c.zig`.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;

pub const version = "1.0";

/// Type nibble. Spec section 3.
pub const Type = enum(u4) {
    null = 0x0,
    bool = 0x1,
    tinyuint = 0x2,
    int8 = 0x3,
    int16 = 0x4,
    int32 = 0x5,
    int64 = 0x6,
    float32 = 0x7,
    float64 = 0x8,
    string = 0x9,
    array = 0xA,
    object = 0xB,
    /// Raw opaque bytes, no encoding contract. The default when nothing more
    /// specific applies (section 7.4).
    buffer = 0xC,
    reserved_d = 0xD,
    reserved_e = 0xE,
    extended = 0xF,

    /// How MOD is to be interpreted, which is the only thing that varies between
    /// types once the nibble is known.
    pub const Family = enum {
        /// Whole value lives in MOD; the frame is exactly one byte (section 4).
        inline_value,
        /// MOD is the data byte count, 0..width (section 5).
        sized_int,
        /// MOD is 0 (value 0.0) or the full width; nothing between (section 6).
        sized_float,
        /// MOD <= 7 is the length; MOD >= 8 gives a length-of-length (section 7).
        variable,
        /// Nibbles 0xD/0xE, which conforming encoders must not emit (section 11).
        reserved,
        /// Escape to an application-defined type (section 8).
        extended,
    };

    pub fn family(t: Type) Family {
        return switch (t) {
            .null, .bool, .tinyuint => .inline_value,
            .int8, .int16, .int32, .int64 => .sized_int,
            .float32, .float64 => .sized_float,
            .string, .array, .object, .buffer => .variable,
            .reserved_d, .reserved_e => .reserved,
            .extended => .extended,
        };
    }

    /// Data-byte ceiling for the sized families; MOD may not exceed it.
    pub fn width(t: Type) u4 {
        return switch (t) {
            .int8 => 1,
            .int16 => 2,
            .int32 => 4,
            .int64 => 8,
            .float32 => 4,
            .float64 => 8,
            else => 0,
        };
    }
};

pub const Error = error{
    /// Fewer bytes present than the header/length field says the frame needs.
    Truncated,
    /// MOD is not a legal value for this type: BOOL above 1, a sized type above its
    /// width, or a partial-width float (sections 4-6).
    InvalidMod,
    /// Type nibble 0xD or 0xE.
    ReservedType,
    /// A container's contents do not line up with its declared length: a child frame
    /// crossing the end, an object key that is not a STRING, or a key with no value.
    MalformedContainer,
    /// STRING data is not valid UTF-8 (section 7.1).
    InvalidUtf8,
    /// A declared length does not fit this platform's usize.
    LengthOverflow,
    /// Container nesting deeper than the caller allowed.
    DepthExceeded,
    /// An accessor was used on a frame of the wrong type.
    TypeMismatch,
    /// Section 8 leaves each extension to define its own framing, so the generic
    /// decoder cannot tell where an EXTENDED frame ends.
    ExtensionFramingUnknown,
};

pub fn headerByte(t: Type, mod: u4) u8 {
    return (@as(u8, @intFromEnum(t)) << 4) | mod;
}

pub const Header = struct {
    type: Type,
    mod: u4,

    pub fn parse(b: u8) Header {
        return .{ .type = @enumFromInt(@as(u4, @truncate(b >> 4))), .mod = @truncate(b) };
    }

    pub fn byte(h: Header) u8 {
        return headerByte(h.type, h.mod);
    }
};

/// One decoded frame. `data` borrows the input bytes; nothing is copied.
pub const Frame = struct {
    type: Type,
    mod: u4,
    /// Header byte plus the extended length field, if any: 1, or 1 + LL.
    prefix_len: usize,
    /// The L data bytes. Empty for the inline family.
    data: []const u8,
    /// The complete frame, prefix included — what to write to forward it verbatim,
    /// hash it, or stash it. Same bytes `size()` counts.
    raw: []const u8,
    /// The extension type code, for EXTENDED frames only.
    ext_code: ?u8 = null,

    /// Total bytes the frame occupies on the wire.
    pub fn size(f: Frame) usize {
        return f.prefix_len + f.data.len;
    }

    pub fn isNull(f: Frame) bool {
        return f.type == .null;
    }

    pub fn asBool(f: Frame) Error!bool {
        if (f.type != .bool) return error.TypeMismatch;
        return f.mod == 1;
    }

    /// TINYUINT and the INT* family, sign-extended per section 5.
    pub fn asInt(f: Frame) Error!i64 {
        return switch (f.type) {
            .tinyuint => f.mod,
            .int8, .int16, .int32, .int64 => signExtend(f.data),
            else => error.TypeMismatch,
        };
    }

    /// The same value read as unsigned, for producers following section 11's
    /// "carry unsigned in INT* and reinterpret at the application layer".
    pub fn asUint(f: Frame) Error!u64 {
        return switch (f.type) {
            .tinyuint => f.mod,
            .int8, .int16, .int32, .int64 => @bitCast(signExtend(f.data)),
            else => error.TypeMismatch,
        };
    }

    pub fn asF32(f: Frame) Error!f32 {
        if (f.type != .float32) return error.TypeMismatch;
        if (f.data.len == 0) return 0.0;
        return @bitCast(std.mem.readInt(u32, f.data[0..4], .big));
    }

    pub fn asF64(f: Frame) Error!f64 {
        if (f.type != .float64) return error.TypeMismatch;
        if (f.data.len == 0) return 0.0;
        return @bitCast(std.mem.readInt(u64, f.data[0..8], .big));
    }

    /// Either float width, widened to f64.
    pub fn asFloat(f: Frame) Error!f64 {
        return switch (f.type) {
            .float32 => try f.asF32(),
            .float64 => try f.asF64(),
            else => error.TypeMismatch,
        };
    }

    /// STRING data, UTF-8 validated (section 7.1).
    pub fn asString(f: Frame) Error![]const u8 {
        if (f.type != .string) return error.TypeMismatch;
        if (!std.unicode.utf8ValidateSlice(f.data)) return error.InvalidUtf8;
        return f.data;
    }

    /// BUFFER data, opaque by contract (section 7.4).
    pub fn asBuffer(f: Frame) Error![]const u8 {
        if (f.type != .buffer) return error.TypeMismatch;
        return f.data;
    }

    /// Payload of any length-prefixed type without validating it. Useful for
    /// forwarding or hashing, where the UTF-8 contract is not being relied on.
    pub fn asBytes(f: Frame) Error![]const u8 {
        return switch (f.type) {
            .string, .buffer, .array, .object => f.data,
            else => error.TypeMismatch,
        };
    }

    pub fn arrayIterator(f: Frame) Error!Iterator {
        if (f.type != .array) return error.TypeMismatch;
        return .{ .rest = f.data };
    }

    pub fn objectIterator(f: Frame) Error!ObjectIterator {
        if (f.type != .object) return error.TypeMismatch;
        return .{ .rest = f.data };
    }
};

/// Walks an ARRAY's children. Elements are decoded one at a time, so a large array
/// costs nothing until it is read (section 7.2).
pub const Iterator = struct {
    rest: []const u8,

    pub fn next(it: *Iterator) Error!?Frame {
        if (it.rest.len == 0) return null;
        const frame = try decode(it.rest);
        const n = frame.size();
        if (n > it.rest.len) return error.MalformedContainer;
        it.rest = it.rest[n..];
        return frame;
    }
};

pub const KeyValue = struct {
    key: []const u8,
    value: Frame,
};

/// Walks an OBJECT's key/value pairs in wire order, which section 7.3 defines as
/// significant.
pub const ObjectIterator = struct {
    rest: []const u8,

    pub fn next(it: *ObjectIterator) Error!?KeyValue {
        if (it.rest.len == 0) return null;

        const key_frame = try decode(it.rest);
        if (key_frame.type != .string) return error.MalformedContainer;
        const key = try key_frame.asString();
        if (key_frame.size() > it.rest.len) return error.MalformedContainer;
        it.rest = it.rest[key_frame.size()..];

        if (it.rest.len == 0) return error.MalformedContainer; // key with no value
        const value = try decode(it.rest);
        if (value.size() > it.rest.len) return error.MalformedContainer;
        it.rest = it.rest[value.size()..];

        return .{ .key = key, .value = value };
    }
};

fn signExtend(data: []const u8) i64 {
    if (data.len == 0) return 0; // L = 0 decodes to 0 (section 5)
    // Seed with the sign bit repeated, then shift the bytes in: for L < 8 that leaves
    // the untouched high bits correctly extended.
    var v: u64 = if (data[0] & 0x80 != 0) ~@as(u64, 0) else 0;
    for (data) |b| v = (v << 8) | b;
    return @bitCast(v);
}

fn readUintBe(bytes: []const u8) Error!usize {
    var v: u64 = 0;
    for (bytes) |b| v = (v << 8) | b;
    if (v > std.math.maxInt(usize)) return error.LengthOverflow;
    return @intCast(v);
}

/// Decode the frame starting at `bytes[0]`. `bytes` may extend past the frame — the
/// returned `Frame.size()` says where it ends, which is how container contents and
/// back-to-back stream frames are walked.
pub fn decode(bytes: []const u8) Error!Frame {
    if (bytes.len == 0) return error.Truncated;

    const header = Header.parse(bytes[0]);

    // Work out the frame's extent first, then slice it once at the end, so every type
    // family agrees on how `prefix_len`, `data` and `raw` relate.
    var prefix_len: usize = 1;
    var data_len: usize = 0;
    var ext_code: ?u8 = null;

    switch (header.type.family()) {
        .reserved => return error.ReservedType,

        .inline_value => {
            // NULL's MOD is ignored on decode by section 4; BOOL's is constrained.
            if (header.type == .bool and header.mod > 1) return error.InvalidMod;
        },

        .sized_int => {
            if (header.mod > header.type.width()) return error.InvalidMod;
            data_len = header.mod;
        },

        .sized_float => {
            // Section 6: 0 or full width only. Truncating a mantissa would be lossy,
            // so partial widths are rejected rather than tolerated.
            const w = header.type.width();
            data_len = if (header.mod == 0) 0 else if (header.mod == w) w else return error.InvalidMod;
        },

        .variable => {
            data_len = header.mod;
            if (header.mod > 7) {
                const ll: usize = @as(usize, header.mod) - 7; // 1..8
                if (bytes.len < 1 + ll) return error.Truncated;
                data_len = try readUintBe(bytes[1 .. 1 + ll]);
                prefix_len = 1 + ll;
            }
        },

        .extended => {
            if (header.mod != 0) return error.InvalidMod; // reserved (section 8)
            if (bytes.len < 2) return error.Truncated;
            // The extension code is readable, but its framing is defined elsewhere, so
            // the frame is reported as ending after the code byte: that is all that is
            // knowable without the extension's own spec.
            prefix_len = 2;
            ext_code = bytes[1];
        },
    }

    const total = std.math.add(usize, prefix_len, data_len) catch return error.LengthOverflow;
    if (bytes.len < total) return error.Truncated;

    return .{
        .type = header.type,
        .mod = header.mod,
        .prefix_len = prefix_len,
        .data = bytes[prefix_len..total],
        .raw = bytes[0..total],
        .ext_code = ext_code,
    };
}

/// Total size of the frame starting at `bytes[0]`, or null when `bytes` is a prefix
/// too short to say yet. This is what design invariant 10 buys: a reader can buffer
/// exactly one frame without parsing, needing at most 9 bytes to decide.
pub fn frameSize(bytes: []const u8) Error!?usize {
    if (bytes.len == 0) return null;

    const header = Header.parse(bytes[0]);

    switch (header.type.family()) {
        .reserved => return error.ReservedType,
        .inline_value => {
            if (header.type == .bool and header.mod > 1) return error.InvalidMod;
            return 1;
        },
        .sized_int => {
            if (header.mod > header.type.width()) return error.InvalidMod;
            return 1 + @as(usize, header.mod);
        },
        .sized_float => {
            const w = header.type.width();
            if (header.mod == 0) return 1;
            if (header.mod != w) return error.InvalidMod;
            return 1 + @as(usize, w);
        },
        .variable => {
            if (header.mod <= 7) return 1 + @as(usize, header.mod);
            const ll: usize = @as(usize, header.mod) - 7;
            if (bytes.len < 1 + ll) return null; // length field still incomplete
            const l = try readUintBe(bytes[1 .. 1 + ll]);
            return std.math.add(usize, 1 + ll, l) catch error.LengthOverflow;
        },
        .extended => return error.ExtensionFramingUnknown,
    }
}

/// Walk a whole frame, checking every nested frame, UTF-8 in strings and object shape.
/// `decode` alone only validates the outermost frame, so this is what to reach for
/// before trusting untrusted input.
pub fn validate(bytes: []const u8, max_depth: usize) Error!Frame {
    const frame = try decode(bytes);
    try validateFrame(frame, max_depth);
    return frame;
}

fn validateFrame(frame: Frame, depth_left: usize) Error!void {
    switch (frame.type) {
        .string => _ = try frame.asString(),
        .array => {
            if (depth_left == 0) return error.DepthExceeded;
            var it = try frame.arrayIterator();
            while (try it.next()) |child| try validateFrame(child, depth_left - 1);
        },
        .object => {
            if (depth_left == 0) return error.DepthExceeded;
            var it = try frame.objectIterator();
            while (try it.next()) |pair| try validateFrame(pair.value, depth_left - 1);
        },
        else => {},
    }
}

/// Smallest number of bytes whose sign-extension reproduces `v` (section 5).
fn minimalIntLen(v: i64) u4 {
    if (v == 0) return 0;
    inline for (1..8) |l| {
        const Narrow = std.meta.Int(.signed, l * 8);
        if (v >= std.math.minInt(Narrow) and v <= std.math.maxInt(Narrow)) return l;
    }
    return 8;
}

/// Build the header (and length field) for a variable-length type into `out`,
/// returning how many bytes were written. Picks the shortest legal encoding.
fn writeVarPrefix(out: *[9]u8, t: Type, len: usize) usize {
    if (len <= 7) {
        out[0] = headerByte(t, @intCast(len));
        return 1;
    }

    var be: [8]u8 = undefined;
    std.mem.writeInt(u64, &be, @intCast(len), .big);

    // Drop leading zero bytes: any LL in 1..8 is legal, the shortest is the obvious
    // choice for an encoder.
    var ll: usize = 8;
    while (ll > 1 and be[8 - ll] == 0) ll -= 1;

    out[0] = headerByte(t, @intCast(ll + 7));
    @memcpy(out[1 .. 1 + ll], be[8 - ll ..]);
    return 1 + ll;
}

/// Appends frames to a caller-owned list, so the caller keeps control of the
/// allocation and can hand the finished bytes straight to a socket.
pub const Encoder = struct {
    allocator: Allocator,
    out: *std.ArrayList(u8),

    pub const WriteError = Allocator.Error || error{InvalidUtf8};

    pub fn init(allocator: Allocator, out: *std.ArrayList(u8)) Encoder {
        return .{ .allocator = allocator, .out = out };
    }

    /// Bytes written so far — the frame being built, once it is complete.
    pub fn bytes(e: *const Encoder) []const u8 {
        return e.out.items;
    }

    fn header(e: *Encoder, t: Type, mod: u4) Allocator.Error!void {
        try e.out.append(e.allocator, headerByte(t, mod));
    }

    pub fn writeNull(e: *Encoder) Allocator.Error!void {
        try e.header(.null, 0);
    }

    pub fn writeBool(e: *Encoder, v: bool) Allocator.Error!void {
        try e.header(.bool, if (v) 1 else 0);
    }

    /// TINYUINT for 0..15, otherwise the shortest INT* that holds `v`. Values above
    /// i64's range go out as a full-width INT64 the reader must treat as unsigned,
    /// since section 11 defines no UINT family.
    pub fn writeUint(e: *Encoder, v: u64) Allocator.Error!void {
        if (v <= 15) return e.header(.tinyuint, @intCast(v));
        if (v <= std.math.maxInt(i64)) return e.writeInt(@intCast(v));

        try e.header(.int64, 8);
        var be: [8]u8 = undefined;
        std.mem.writeInt(u64, &be, v, .big);
        try e.out.appendSlice(e.allocator, &be);
    }

    /// Shortest sized-int encoding: both the data length and the declared type are
    /// minimised, so 300 goes out as INT16/MOD=2 rather than INT32/MOD=2. Both are
    /// legal and the same size on the wire — the type nibble is only a ceiling — so
    /// use `writeIntAs` when the declared width is part of a schema and must survive.
    pub fn writeInt(e: *Encoder, v: i64) Allocator.Error!void {
        const l = minimalIntLen(v);
        const t: Type = switch (l) {
            0, 1 => .int8,
            2 => .int16,
            3, 4 => .int32,
            else => .int64,
        };
        try e.writeIntAs(t, v);
    }

    /// Sized int with the declared type pinned, still trimming data bytes down to the
    /// minimum MOD. Fails if `v` does not fit `t`'s width, or if `t` is not an INT*.
    pub fn writeIntAs(e: *Encoder, t: Type, v: i64) Allocator.Error!void {
        std.debug.assert(t.family() == .sized_int);

        const l = minimalIntLen(v);
        std.debug.assert(l <= t.width());

        try e.header(t, l);
        var be: [8]u8 = undefined;
        std.mem.writeInt(i64, &be, v, .big);
        try e.out.appendSlice(e.allocator, be[8 - @as(usize, l) ..]);
    }

    pub fn writeFloat32(e: *Encoder, v: f32) Allocator.Error!void {
        const bits: u32 = @bitCast(v);
        // Only +0.0 gets the one-byte form: -0.0 compares equal to 0.0 but its sign
        // bit is real data, and section 6 defines MOD=0 as 0.0.
        if (bits == 0) return e.header(.float32, 0);

        try e.header(.float32, 4);
        var be: [4]u8 = undefined;
        std.mem.writeInt(u32, &be, bits, .big);
        try e.out.appendSlice(e.allocator, &be);
    }

    pub fn writeFloat64(e: *Encoder, v: f64) Allocator.Error!void {
        const bits: u64 = @bitCast(v);
        if (bits == 0) return e.header(.float64, 0);

        try e.header(.float64, 8);
        var be: [8]u8 = undefined;
        std.mem.writeInt(u64, &be, bits, .big);
        try e.out.appendSlice(e.allocator, &be);
    }

    fn varFrame(e: *Encoder, t: Type, payload: []const u8) Allocator.Error!void {
        var prefix: [9]u8 = undefined;
        const n = writeVarPrefix(&prefix, t, payload.len);
        try e.out.appendSlice(e.allocator, prefix[0..n]);
        try e.out.appendSlice(e.allocator, payload);
    }

    /// STRING. Rejects invalid UTF-8 rather than emitting a frame that section 7.1
    /// lets a decoder refuse.
    pub fn writeString(e: *Encoder, s: []const u8) WriteError!void {
        if (!std.unicode.utf8ValidateSlice(s)) return error.InvalidUtf8;
        try e.varFrame(.string, s);
    }

    /// BUFFER: raw bytes, no contract. The right choice for anything not known to be
    /// text (section 7.4).
    pub fn writeBuffer(e: *Encoder, b: []const u8) Allocator.Error!void {
        try e.varFrame(.buffer, b);
    }

    /// Section 8 escape. `payload` is written verbatim after the code byte; its
    /// framing is the extension's business.
    pub fn writeExtended(e: *Encoder, code: u8, payload: []const u8) Allocator.Error!void {
        try e.header(.extended, 0);
        try e.out.append(e.allocator, code);
        try e.out.appendSlice(e.allocator, payload);
    }

    /// Position of a container whose header has not been written yet.
    pub const Mark = struct {
        start: usize,
        type: Type,
    };

    /// Containers need L before the header can be written (section 7) but only know it
    /// afterwards. So children are appended first and the header is inserted in front
    /// of them at `end` — one memmove, versus encoding children into a scratch buffer
    /// and copying them in.
    pub fn beginArray(e: *Encoder) Mark {
        return .{ .start = e.out.items.len, .type = .array };
    }

    pub fn beginObject(e: *Encoder) Mark {
        return .{ .start = e.out.items.len, .type = .object };
    }

    pub fn end(e: *Encoder, mark: Mark) Allocator.Error!void {
        const len = e.out.items.len - mark.start;
        var prefix: [9]u8 = undefined;
        const n = writeVarPrefix(&prefix, mark.type, len);
        try e.out.insertSlice(e.allocator, mark.start, prefix[0..n]);
    }

    /// An object key. Same frame as any STRING; named for readability at call sites.
    pub fn writeKey(e: *Encoder, k: []const u8) WriteError!void {
        try e.writeString(k);
    }
};

pub const ReadError = Error || Io.Reader.Error || Allocator.Error || error{FrameTooLarge};

/// Read exactly one frame off a stream, appending its bytes to `out` (cleared first)
/// and returning the frame borrowing them. Frames may span read boundaries, which is
/// the point: the header says how much to expect, so nothing is guessed.
///
/// `max_data_len` caps the declared payload, since a length field is 8 bytes of
/// attacker-controlled allocation request otherwise.
pub fn readFrame(
    reader: *Io.Reader,
    allocator: Allocator,
    out: *std.ArrayList(u8),
    max_data_len: usize,
) ReadError!Frame {
    out.clearRetainingCapacity();

    const first = try reader.takeByte();
    try out.append(allocator, first);
    const header = Header.parse(first);

    var data_len: usize = 0;

    switch (header.type.family()) {
        .reserved => return error.ReservedType,

        .inline_value => {
            if (header.type == .bool and header.mod > 1) return error.InvalidMod;
        },

        .sized_int => {
            if (header.mod > header.type.width()) return error.InvalidMod;
            data_len = header.mod;
        },

        .sized_float => {
            const w = header.type.width();
            if (header.mod == 0) {
                data_len = 0;
            } else if (header.mod == w) {
                data_len = w;
            } else return error.InvalidMod;
        },

        .variable => {
            if (header.mod <= 7) {
                data_len = header.mod;
            } else {
                const ll: usize = @as(usize, header.mod) - 7;
                var be: [8]u8 = undefined;
                try reader.readSliceAll(be[0..ll]);
                try out.appendSlice(allocator, be[0..ll]);
                data_len = try readUintBe(be[0..ll]);
            }
        },

        .extended => return error.ExtensionFramingUnknown,
    }

    if (data_len > max_data_len) return error.FrameTooLarge;

    if (data_len > 0) {
        const dst = try out.addManyAsSlice(allocator, data_len);
        try reader.readSliceAll(dst);
    }

    const prefix_len = out.items.len - data_len;
    return .{
        .type = header.type,
        .mod = header.mod,
        .prefix_len = prefix_len,
        .data = out.items[prefix_len..],
        .raw = out.items,
    };
}

/// Render a frame as JSON-ish text for logs and human inspection. BUFFER has no text
/// contract, so it is shown as a length and a short hex preview rather than pretended
/// to be a string.
pub fn stringify(frame: Frame, allocator: Allocator, out: *std.ArrayList(u8), max_depth: usize) (Error || Allocator.Error)!void {
    switch (frame.type) {
        .null => try out.appendSlice(allocator, "null"),
        .bool => try out.appendSlice(allocator, if (try frame.asBool()) "true" else "false"),
        .tinyuint => try out.print(allocator, "{d}", .{frame.mod}),
        .int8, .int16, .int32, .int64 => try out.print(allocator, "{d}", .{try frame.asInt()}),
        .float32, .float64 => try out.print(allocator, "{d}", .{try frame.asFloat()}),
        .string => try out.print(allocator, "\"{s}\"", .{try frame.asString()}),

        .array => {
            if (max_depth == 0) return error.DepthExceeded;
            try out.append(allocator, '[');
            var it = try frame.arrayIterator();
            var first = true;
            while (try it.next()) |child| {
                if (!first) try out.appendSlice(allocator, ",");
                first = false;
                try stringify(child, allocator, out, max_depth - 1);
            }
            try out.append(allocator, ']');
        },

        .object => {
            if (max_depth == 0) return error.DepthExceeded;
            try out.append(allocator, '{');
            var it = try frame.objectIterator();
            var first = true;
            while (try it.next()) |pair| {
                if (!first) try out.appendSlice(allocator, ",");
                first = false;
                try out.print(allocator, "\"{s}\":", .{pair.key});
                try stringify(pair.value, allocator, out, max_depth - 1);
            }
            try out.append(allocator, '}');
        },

        .buffer => {
            const preview = frame.data[0..@min(frame.data.len, 8)];
            try out.print(allocator, "<buffer {d}B", .{frame.data.len});
            if (preview.len > 0) try out.print(allocator, " {x}", .{preview});
            if (preview.len < frame.data.len) try out.appendSlice(allocator, "...");
            try out.append(allocator, '>');
        },

        .extended => try out.print(allocator, "<ext 0x{x:0>2}>", .{frame.ext_code orelse 0}),
        .reserved_d, .reserved_e => return error.ReservedType,
    }
}

test {
    _ = @import("./told.test.zig");
}

// ---------------------------------------------------------------- struct mapping
//
// Packing a Zig value straight to TOLD and back, the way msgpack's pack/unpack work on a
// C++ object. Zig needs no macros and no descriptor tables for this: @typeInfo gives the
// field names and types at compile time, so `pack` and `unpack` below are ordinary
// recursive functions over the type rather than over a schema someone has to maintain.
//
// The mapping:
//
//   bool                      -> bool
//   unsigned int              -> tinyuint / int8..int64, smallest that fits
//   signed int                -> int8..int64
//   f32 / f64                 -> float32 / float64
//   enum                      -> its integer value (a name is also accepted on unpack)
//   optional                  -> null, or the child
//   []const u8                -> string (use `Bytes` to force a binary buffer)
//   Bytes                     -> buffer
//   slice / array of T        -> array of T
//   tuple                     -> array, positionally
//   struct                    -> object, field names as keys
//   tagged union              -> object with one key, the active tag
//
// Unpacking is deliberately lenient in one direction and strict in the other: unknown keys
// in the frame are ignored, so a producer may add fields without breaking older consumers,
// while a field missing from the frame is an error unless the struct gives it a default.

/// Wrap a byte slice to pack it as a TOLD buffer rather than a string. `[]const u8` alone
/// is ambiguous -- text far more often than not -- so this is how binary says so.
pub const Bytes = struct {
    data: []const u8,
};

pub const PackError = Encoder.WriteError;

pub const UnpackError = Error || Allocator.Error || error{
    /// The frame's type cannot become the requested Zig type.
    TypeMismatch,
    /// A struct field with no default was absent from the object.
    MissingField,
    /// A fixed-size array was given a different number of elements.
    WrongArrayLength,
    /// An enum or union tag in the frame names nothing in the Zig type.
    UnknownTag,
    /// The value does not fit the target integer type.
    OutOfRange,
};

/// Encode `value` -- any Zig value the mapping above covers -- into `e`.
pub fn pack(e: *Encoder, value: anytype) PackError!void {
    const T = @TypeOf(value);

    if (T == Bytes) return e.writeBuffer(value.data);

    switch (@typeInfo(T)) {
        .bool => return e.writeBool(value),
        // A non-negative value takes the unsigned encoding whatever the Zig type's
        // signedness: TINYUINT holds 0..15 in one byte where INT8 needs two, and asInt
        // reads an unsigned frame back without complaint. It also makes the bytes match
        // what the C++ and Python sides emit for the same struct.
        .int => |info| {
            if (info.signedness == .unsigned) return e.writeUint(@intCast(value));
            return if (value >= 0) e.writeUint(@intCast(value)) else e.writeInt(@intCast(value));
        },
        .comptime_int => return e.writeInt(value),
        .float => |info| return switch (info.bits) {
            32 => e.writeFloat32(value),
            else => e.writeFloat64(@floatCast(value)),
        },
        .comptime_float => return e.writeFloat64(value),
        // The integer, not the tag name. C++ cannot turn a name back into an enumerator
        // without reflection it does not have, and this format has to be readable from
        // C, C++ and Python as well as Zig. Unpacking still accepts a name, so frames
        // written by hand or by an older build stay readable.
        .@"enum" => {
            const raw = @intFromEnum(value);
            return if (raw >= 0) e.writeUint(@intCast(raw)) else e.writeInt(@intCast(raw));
        },
        .optional => return if (value) |v| pack(e, v) else e.writeNull(),
        .null => return e.writeNull(),

        .pointer => |ptr| switch (ptr.size) {
            .one => return pack(e, value.*),
            .slice => {
                if (ptr.child == u8) return e.writeString(value);
                const mark = e.beginArray();
                for (value) |item| try pack(e, item);
                return e.end(mark);
            },
            else => @compileError("told.pack: cannot pack " ++ @typeName(T)),
        },

        .array => |arr| {
            if (arr.child == u8) return e.writeString(&value);
            const mark = e.beginArray();
            for (value) |item| try pack(e, item);
            return e.end(mark);
        },

        .@"struct" => |st| {
            // A tuple is positional by nature, so it becomes an array; a named struct
            // carries its field names into the object.
            if (st.is_tuple) {
                const mark = e.beginArray();
                inline for (st.fields) |f| try pack(e, @field(value, f.name));
                return e.end(mark);
            }
            const mark = e.beginObject();
            inline for (st.fields) |f| {
                try e.writeKey(f.name);
                try pack(e, @field(value, f.name));
            }
            return e.end(mark);
        },

        .@"union" => |un| {
            if (un.tag_type == null) @compileError("told.pack: untagged union " ++ @typeName(T));
            const mark = e.beginObject();
            switch (value) {
                inline else => |payload, tag| {
                    try e.writeKey(@tagName(tag));
                    try pack(e, payload);
                },
            }
            return e.end(mark);
        },

        .void => return e.writeNull(),

        else => @compileError("told.pack: unsupported type " ++ @typeName(T)),
    }
}

/// Decode `frame` into a `T`.
///
/// `allocator` decides who owns the strings and slices in the result. Pass one and they are
/// copied out, so the result outlives the frame; pass null and they point into the frame's
/// own bytes, which is free but only valid while those bytes are.
///
/// There is no matching `free`: use an arena and drop it whole. Reference counting the
/// interior of a decoded struct is not a thing this wants to own.
pub fn unpack(comptime T: type, frame: Frame, allocator: ?Allocator) UnpackError!T {
    if (T == Bytes) return .{ .data = try dupeMaybe(try frame.asBytes(), allocator) };

    switch (@typeInfo(T)) {
        .bool => return frame.asBool() catch error.TypeMismatch,
        .int => |info| {
            if (info.signedness == .unsigned) {
                const v = frame.asUint() catch return error.TypeMismatch;
                return std.math.cast(T, v) orelse error.OutOfRange;
            }
            const v = frame.asInt() catch return error.TypeMismatch;
            return std.math.cast(T, v) orelse error.OutOfRange;
        },
        .float => {
            const v = frame.asFloat() catch return error.TypeMismatch;
            return @floatCast(v);
        },
        .@"enum" => {
            // Integers are what `pack` writes; a name is accepted too, so a frame from a
            // language that spells enums out, or from a human, still decodes.
            if (frame.asString()) |name| {
                return std.meta.stringToEnum(T, name) orelse error.UnknownTag;
            } else |_| {}
            const v = frame.asInt() catch return error.TypeMismatch;
            // Checked rather than @enumFromInt'd: a value outside the enum would otherwise
            // become an invalid tag that misbehaves far from here.
            inline for (@typeInfo(T).@"enum".fields) |f| {
                if (v == f.value) return @field(T, f.name);
            }
            return error.UnknownTag;
        },
        .optional => |opt| {
            if (frame.isNull()) return null;
            return try unpack(opt.child, frame, allocator);
        },

        .pointer => |ptr| switch (ptr.size) {
            .slice => {
                if (ptr.child == u8) return try dupeMaybe(try frame.asBytes(), allocator);

                // A slice of anything else needs storage for the elements, so unlike a
                // byte slice this cannot borrow from the frame.
                const alloc = allocator orelse return error.OutOfMemory;
                var list: std.ArrayList(ptr.child) = .empty;
                errdefer list.deinit(alloc);
                var it = frame.arrayIterator() catch return error.TypeMismatch;
                while (try it.next()) |item| {
                    try list.append(alloc, try unpack(ptr.child, item, allocator));
                }
                return try list.toOwnedSlice(alloc);
            },
            else => @compileError("told.unpack: cannot unpack into " ++ @typeName(T)),
        },

        .array => |arr| {
            var out: T = undefined;
            if (arr.child == u8) {
                const bytes = try frame.asBytes();
                if (bytes.len != arr.len) return error.WrongArrayLength;
                @memcpy(&out, bytes);
                return out;
            }
            var it = frame.arrayIterator() catch return error.TypeMismatch;
            var i: usize = 0;
            while (try it.next()) |item| : (i += 1) {
                if (i >= arr.len) return error.WrongArrayLength;
                out[i] = try unpack(arr.child, item, allocator);
            }
            if (i != arr.len) return error.WrongArrayLength;
            return out;
        },

        .@"struct" => |st| {
            var out: T = undefined;

            if (st.is_tuple) {
                var it = frame.arrayIterator() catch return error.TypeMismatch;
                inline for (st.fields) |f| {
                    const item = (try it.next()) orelse return error.WrongArrayLength;
                    @field(out, f.name) = try unpack(f.type, item, allocator);
                }
                return out;
            }

            // Track which fields the frame supplied, so a missing one can fall back to its
            // default and a missing one without a default can be named as an error.
            var seen = [_]bool{false} ** st.fields.len;

            var it = frame.objectIterator() catch return error.TypeMismatch;
            while (try it.next()) |pair| {
                inline for (st.fields, 0..) |f, idx| {
                    if (std.mem.eql(u8, pair.key, f.name)) {
                        @field(out, f.name) = try unpack(f.type, pair.value, allocator);
                        seen[idx] = true;
                    }
                }
                // Keys matching no field are ignored on purpose: it lets a producer add a
                // field without every consumer having to be rebuilt first.
            }

            inline for (st.fields, 0..) |f, idx| {
                if (!seen[idx]) {
                    const default = f.defaultValue() orelse return error.MissingField;
                    @field(out, f.name) = default;
                }
            }
            return out;
        },

        .@"union" => |un| {
            const Tag = un.tag_type orelse
                @compileError("told.unpack: untagged union " ++ @typeName(T));

            var it = frame.objectIterator() catch return error.TypeMismatch;
            const pair = (try it.next()) orelse return error.TypeMismatch;
            const tag = std.meta.stringToEnum(Tag, pair.key) orelse return error.UnknownTag;

            switch (tag) {
                inline else => |t| {
                    const Payload = @FieldType(T, @tagName(t));
                    return @unionInit(T, @tagName(t), try unpack(Payload, pair.value, allocator));
                },
            }
        },

        .void => return {},

        else => @compileError("told.unpack: unsupported type " ++ @typeName(T)),
    }
}

fn dupeMaybe(bytes: []const u8, allocator: ?Allocator) Allocator.Error![]const u8 {
    return if (allocator) |a| try a.dupe(u8, bytes) else bytes;
}

/// Pack `value` into `out`, which is cleared first. The convenience form of `pack` for
/// callers that just want the bytes.
pub fn packTo(allocator: Allocator, out: *std.ArrayList(u8), value: anytype) PackError!void {
    out.clearRetainingCapacity();
    var e = Encoder.init(allocator, out);
    try pack(&e, value);
}

/// Decode the frame at the start of `bytes` into a `T`.
pub fn unpackFrom(comptime T: type, bytes: []const u8, allocator: ?Allocator) UnpackError!T {
    return unpack(T, try decode(bytes), allocator);
}
