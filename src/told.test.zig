// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

//! Tests for the TOLD v1.0 implementation. The byte-level cases are taken straight
//! from the specification's worked examples, so a regression shows up as a mismatch
//! against the spec rather than against a previous implementation's opinion.

const std = @import("std");
const told = @import("told.zig");
const testing = std.testing;

const allocator = testing.allocator;

/// Encode via `f` and assert the exact bytes produced.
fn expectEncoding(expected: []const u8, comptime f: fn (*told.Encoder) anyerror!void) !void {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);

    var enc = told.Encoder.init(allocator, &out);
    try f(&enc);
    try testing.expectEqualSlices(u8, expected, out.items);
}

// ---------------------------------------------------------------- section 3/4

test "header nibbles round-trip" {
    const h = told.Header.parse(0x52);
    try testing.expectEqual(told.Type.int32, h.type);
    try testing.expectEqual(@as(u4, 2), h.mod);
    try testing.expectEqual(@as(u8, 0x52), h.byte());
}

test "inline family is exactly one byte" {
    try expectEncoding(&.{0x00}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeNull();
        }
    }.f);
    try expectEncoding(&.{0x10}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeBool(false);
        }
    }.f);
    try expectEncoding(&.{0x11}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeBool(true);
        }
    }.f);
    try expectEncoding(&.{0x2F}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeUint(15);
        }
    }.f);

    for ([_][]const u8{ &.{0x00}, &.{0x10}, &.{0x11}, &.{0x2F} }) |bytes| {
        const frame = try told.decode(bytes);
        try testing.expectEqual(@as(usize, 1), frame.size());
        try testing.expectEqual(@as(usize, 0), frame.data.len);
    }
}

test "NULL ignores MOD on decode, BOOL rejects MOD above 1" {
    // Section 4: NULL's MOD is unused, must be 0 on encode, ignored on decode.
    const frame = try told.decode(&.{0x07});
    try testing.expect(frame.isNull());

    try testing.expectError(error.InvalidMod, told.decode(&.{0x12}));
    try testing.expectError(error.InvalidMod, told.decode(&.{0x1F}));
}

test "TINYUINT carries 0..15 in MOD" {
    for (0..16) |v| {
        var out: std.ArrayList(u8) = .empty;
        defer out.deinit(allocator);
        var enc = told.Encoder.init(allocator, &out);
        try enc.writeUint(v);

        try testing.expectEqual(@as(usize, 1), out.items.len);
        const frame = try told.decode(out.items);
        try testing.expectEqual(told.Type.tinyuint, frame.type);
        try testing.expectEqual(v, try frame.asUint());
    }
}

// ---------------------------------------------------------------- section 5

test "spec example: 0x52 0x01 0x2C is INT32 holding 300 in two bytes" {
    // The spec's example demonstrates that the type nibble is only a ceiling: an
    // INT32 carrying 300 needs just MOD=2 data bytes, for 3 bytes total.
    const frame = try told.decode(&.{ 0x52, 0x01, 0x2C });
    try testing.expectEqual(told.Type.int32, frame.type);
    try testing.expectEqual(@as(i64, 300), try frame.asInt());
    try testing.expectEqual(@as(usize, 3), frame.size());

    // `writeIntAs` reproduces those exact bytes when the declared width matters.
    try expectEncoding(&.{ 0x52, 0x01, 0x2C }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeIntAs(.int32, 300);
        }
    }.f);

    // `writeInt` minimises the declared type too, which is equally legal and the same
    // 3 bytes on the wire: INT16 with MOD=2.
    try expectEncoding(&.{ 0x42, 0x01, 0x2C }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeInt(300);
        }
    }.f);
    try testing.expectEqual(@as(i64, 300), try (try told.decode(&.{ 0x42, 0x01, 0x2C })).asInt());
}

test "sized ints use the fewest bytes, type nibble is only a ceiling" {
    // 3-byte values are legal as INT32 with MOD=3 (below the 4-byte ceiling).
    try expectEncoding(&.{ 0x53, 0x01, 0x00, 0x00 }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeInt(65536);
        }
    }.f);

    // Zero needs no data bytes at all.
    try expectEncoding(&.{0x30}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeInt(0);
        }
    }.f);
}

test "L = 0 decodes to zero" {
    for ([_]u8{ 0x30, 0x40, 0x50, 0x60 }) |header| {
        const frame = try told.decode(&.{header});
        try testing.expectEqual(@as(i64, 0), try frame.asInt());
    }
}

test "sized int round-trip across magnitudes and signs" {
    // zig fmt: off
    const cases = [_]i64{
        0,                       1,                    -1,  127,  -128, 128,   -129,
        255,                     -256,                 300, -300, 32767, -32768,
        65535,                   -65536,               8388607,     -8388608,
        2147483647,              -2147483648,          4294967296,
        std.math.maxInt(i64),    std.math.minInt(i64),
    };
    // zig fmt: on

    for (cases) |v| {
        var out: std.ArrayList(u8) = .empty;
        defer out.deinit(allocator);
        var enc = told.Encoder.init(allocator, &out);
        try enc.writeInt(v);

        const frame = try told.decode(out.items);
        try testing.expectEqual(v, try frame.asInt());
        try testing.expectEqual(out.items.len, frame.size());
        // Sign extension must survive a short encoding.
        try testing.expect(frame.data.len <= 8);
    }
}

test "MOD above the type width is invalid" {
    try testing.expectError(error.InvalidMod, told.decode(&.{ 0x32, 0x00, 0x00 })); // INT8, MOD=2
    try testing.expectError(error.InvalidMod, told.decode(&.{ 0x43, 0, 0, 0 })); // INT16, MOD=3
    try testing.expectError(error.InvalidMod, told.decode(&.{ 0x55, 0, 0, 0, 0, 0 })); // INT32, MOD=5
    try testing.expectError(error.InvalidMod, told.decode(&.{ 0x69, 0, 0, 0, 0, 0, 0, 0, 0, 0 })); // INT64, MOD=9
}

// ---------------------------------------------------------------- section 6

test "floats: MOD 0 is the one-byte zero, full width otherwise" {
    try expectEncoding(&.{0x70}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeFloat32(0.0);
        }
    }.f);
    try expectEncoding(&.{0x80}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeFloat64(0.0);
        }
    }.f);

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try enc.writeFloat64(1.5);
    try testing.expectEqual(@as(usize, 9), out.items.len);
    try testing.expectEqual(@as(u8, 0x88), out.items[0]);
    try testing.expectEqual(@as(f64, 1.5), try (try told.decode(out.items)).asFloat());
}

test "partial-width floats are rejected" {
    // Section 6 disallows truncating an IEEE-754 mantissa.
    for ([_]u4{ 1, 2, 3, 5, 8, 15 }) |mod| {
        var buf: [16]u8 = @splat(0);
        buf[0] = told.headerByte(.float32, mod);
        try testing.expectError(error.InvalidMod, told.decode(&buf));
    }
    for ([_]u4{ 1, 4, 7, 9, 15 }) |mod| {
        var buf: [16]u8 = @splat(0);
        buf[0] = told.headerByte(.float64, mod);
        try testing.expectError(error.InvalidMod, told.decode(&buf));
    }
}

test "negative zero keeps its sign instead of collapsing to the one-byte form" {
    // -0.0 == 0.0 is true, so a naive zero check would silently drop the sign bit.
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try enc.writeFloat64(-0.0);

    try testing.expectEqual(@as(usize, 9), out.items.len);
    const decoded = try (try told.decode(out.items)).asF64();
    try testing.expect(std.math.signbit(decoded));
}

test "float round-trip" {
    for ([_]f64{ 1.5, -1.5, 3.141592653589793, 1e300, -1e-300, std.math.inf(f64) }) |v| {
        var out: std.ArrayList(u8) = .empty;
        defer out.deinit(allocator);
        var enc = told.Encoder.init(allocator, &out);
        try enc.writeFloat64(v);
        try testing.expectEqual(v, try (try told.decode(out.items)).asFloat());
    }

    var nan_out: std.ArrayList(u8) = .empty;
    defer nan_out.deinit(allocator);
    var nan_enc = told.Encoder.init(allocator, &nan_out);
    try nan_enc.writeFloat32(std.math.nan(f32));
    try testing.expect(std.math.isNan(try (try told.decode(nan_out.items)).asF32()));
}

// ---------------------------------------------------------------- section 7

test "spec examples: variable-length framing" {
    // empty string -> 0x90
    try expectEncoding(&.{0x90}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeString("");
        }
    }.f);

    // "hi" -> 0x92 0x68 0x69
    try expectEncoding(&.{ 0x92, 0x68, 0x69 }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeString("hi");
        }
    }.f);

    // 7-byte string -> 0x97 + data (MOD=7, the inline maximum)
    try expectEncoding(&.{ 0x97, 'a', 'b', 'c', 'd', 'e', 'f', 'g' }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeString("abcdefg");
        }
    }.f);

    // 8-byte string -> 0x98 0x08 + data (MOD=8 -> LL=1, L=8)
    try expectEncoding(&.{ 0x98, 0x08, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' }, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeString("abcdefgh");
        }
    }.f);

    // empty buffer -> 0xC0
    try expectEncoding(&.{0xC0}, struct {
        fn f(e: *told.Encoder) anyerror!void {
            try e.writeBuffer("");
        }
    }.f);
}

test "spec example: 300-byte buffer -> 0xC9 0x01 0x2C + data" {
    const payload: [300]u8 = @splat(0xAB);

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try enc.writeBuffer(&payload);

    try testing.expectEqual(@as(usize, 303), out.items.len);
    try testing.expectEqualSlices(u8, &.{ 0xC9, 0x01, 0x2C }, out.items[0..3]);

    const frame = try told.decode(out.items);
    try testing.expectEqualSlices(u8, &payload, try frame.asBuffer());
    try testing.expectEqual(@as(usize, 303), frame.size());
}

test "inline/extended mode boundary is at 8 bytes" {
    for (0..24) |len| {
        const payload = try allocator.alloc(u8, len);
        defer allocator.free(payload);
        @memset(payload, 'x');

        var out: std.ArrayList(u8) = .empty;
        defer out.deinit(allocator);
        var enc = told.Encoder.init(allocator, &out);
        try enc.writeBuffer(payload);

        const expected_prefix: usize = if (len <= 7) 1 else 2; // 1 header, +1 LL byte
        try testing.expectEqual(expected_prefix + len, out.items.len);

        const frame = try told.decode(out.items);
        try testing.expectEqual(expected_prefix, frame.prefix_len);
        try testing.expectEqual(len, frame.data.len);
    }
}

test "STRING enforces UTF-8 on both sides" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try testing.expectError(error.InvalidUtf8, enc.writeString(&.{ 0xFF, 0xFE }));

    // A hand-built STRING frame with invalid bytes decodes, but reading it as text
    // fails: section 7.1 makes that a protocol error.
    const frame = try told.decode(&.{ 0x92, 0xFF, 0xFE });
    try testing.expectError(error.InvalidUtf8, frame.asString());
    // The same bytes are perfectly legal as a BUFFER.
    try testing.expectEqualSlices(u8, &.{ 0xFF, 0xFE }, try (try told.decode(&.{ 0xC2, 0xFF, 0xFE })).asBytes());
}

test "multi-byte UTF-8 measures in bytes, not codepoints" {
    const text = "héllo wörld"; // 13 bytes, 11 codepoints
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try enc.writeString(text);

    const frame = try told.decode(out.items);
    try testing.expectEqual(text.len, frame.data.len);
    try testing.expectEqualStrings(text, try frame.asString());
}

test "ARRAY holds concatenated frames and is skippable by L alone" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const mark = enc.beginArray();
    try enc.writeUint(1);
    try enc.writeBool(true);
    try enc.writeString("hi");
    try enc.writeNull();
    try enc.end(mark);

    const frame = try told.decode(out.items);
    try testing.expectEqual(told.Type.array, frame.type);

    // Skipping costs nothing: size() comes from the header alone.
    try testing.expectEqual(out.items.len, frame.size());

    var it = try frame.arrayIterator();
    try testing.expectEqual(@as(u64, 1), try (try it.next()).?.asUint());
    try testing.expectEqual(true, try (try it.next()).?.asBool());
    try testing.expectEqualStrings("hi", try (try it.next()).?.asString());
    try testing.expect((try it.next()).?.isNull());
    try testing.expectEqual(@as(?told.Frame, null), try it.next());
}

test "empty ARRAY and OBJECT are one byte" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const a = enc.beginArray();
    try enc.end(a);
    const o = enc.beginObject();
    try enc.end(o);

    try testing.expectEqualSlices(u8, &.{ 0xA0, 0xB0 }, out.items);
}

test "OBJECT preserves key order and nests" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const obj = enc.beginObject();
    try enc.writeKey("topic");
    try enc.writeString("test/topic-a");
    try enc.writeKey("seq");
    try enc.writeUint(4242);
    try enc.writeKey("tags");
    const arr = enc.beginArray();
    try enc.writeString("a");
    try enc.writeString("b");
    try enc.end(arr);
    try enc.writeKey("payload");
    try enc.writeBuffer(&.{ 0xDE, 0xAD, 0xBE, 0xEF });
    try enc.end(obj);

    const frame = try told.decode(out.items);
    try testing.expectEqual(out.items.len, frame.size());

    var it = try frame.objectIterator();

    const first = (try it.next()).?;
    try testing.expectEqualStrings("topic", first.key);
    try testing.expectEqualStrings("test/topic-a", try first.value.asString());

    const second = (try it.next()).?;
    try testing.expectEqualStrings("seq", second.key);
    try testing.expectEqual(@as(u64, 4242), try second.value.asUint());

    const third = (try it.next()).?;
    try testing.expectEqualStrings("tags", third.key);
    var tags = try third.value.arrayIterator();
    try testing.expectEqualStrings("a", try (try tags.next()).?.asString());
    try testing.expectEqualStrings("b", try (try tags.next()).?.asString());
    try testing.expectEqual(@as(?told.Frame, null), try tags.next());

    const fourth = (try it.next()).?;
    try testing.expectEqualStrings("payload", fourth.key);
    try testing.expectEqualSlices(u8, &.{ 0xDE, 0xAD, 0xBE, 0xEF }, try fourth.value.asBuffer());

    try testing.expectEqual(@as(?told.KeyValue, null), try it.next());
}

test "duplicate object keys are the encoding's business to allow" {
    // Section 7.3: permitted by the encoding, application-layer concern otherwise.
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const obj = enc.beginObject();
    try enc.writeKey("k");
    try enc.writeUint(1);
    try enc.writeKey("k");
    try enc.writeUint(2);
    try enc.end(obj);

    var it = try (try told.decode(out.items)).objectIterator();
    try testing.expectEqual(@as(u64, 1), try (try it.next()).?.value.asUint());
    try testing.expectEqual(@as(u64, 2), try (try it.next()).?.value.asUint());
}

test "container larger than the inline limit gets an extended length" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const arr = enc.beginArray();
    for (0..200) |_| try enc.writeUint(7); // 200 one-byte frames
    try enc.end(arr);

    try testing.expectEqual(@as(u8, told.headerByte(.array, 8)), out.items[0]); // LL=1
    try testing.expectEqual(@as(u8, 200), out.items[1]);

    var count: usize = 0;
    var it = try (try told.decode(out.items)).arrayIterator();
    while (try it.next()) |child| {
        try testing.expectEqual(@as(u64, 7), try child.asUint());
        count += 1;
    }
    try testing.expectEqual(@as(usize, 200), count);
}

test "malformed objects are caught" {
    // Key that is not a STRING.
    const bad_key = [_]u8{ told.headerByte(.object, 2), 0x2A, 0x2B };
    var it = try (try told.decode(&bad_key)).objectIterator();
    try testing.expectError(error.MalformedContainer, it.next());

    // Key with no value.
    const dangling = [_]u8{ told.headerByte(.object, 3), 0x92, 'h', 'i' };
    var it2 = try (try told.decode(&dangling)).objectIterator();
    try testing.expectError(error.MalformedContainer, it2.next());
}

test "a child frame overrunning its container is caught" {
    // ARRAY says L=2, but the child STRING claims 4 data bytes.
    const bytes = [_]u8{ told.headerByte(.array, 2), told.headerByte(.string, 4), 'a' };
    var it = try (try told.decode(&bytes)).arrayIterator();
    try testing.expectError(error.Truncated, it.next());
}

// ---------------------------------------------------------------- section 8/11

test "reserved type nibbles are refused" {
    try testing.expectError(error.ReservedType, told.decode(&.{0xD0}));
    try testing.expectError(error.ReservedType, told.decode(&.{0xE0}));
    try testing.expectError(error.ReservedType, told.frameSize(&.{0xD5}));
}

test "EXTENDED exposes its code but not its extent" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);
    try enc.writeExtended(0x42, &.{ 1, 2, 3 });

    try testing.expectEqualSlices(u8, &.{ 0xF0, 0x42, 1, 2, 3 }, out.items);

    const frame = try told.decode(out.items);
    try testing.expectEqual(@as(?u8, 0x42), frame.ext_code);

    // Section 8 does not standardize extension framing, so a generic reader cannot
    // know where the frame ends -- unlike every other type.
    try testing.expectError(error.ExtensionFramingUnknown, told.frameSize(out.items));

    // MOD is reserved and must be 0.
    try testing.expectError(error.InvalidMod, told.decode(&.{ 0xF1, 0x42 }));
}

// ---------------------------------------------------------------- streaming

test "frameSize needs at most 9 bytes and reports incomplete prefixes" {
    try testing.expectEqual(@as(?usize, null), try told.frameSize(&.{}));

    // Inline and sized types are decided by the header alone.
    try testing.expectEqual(@as(?usize, 1), try told.frameSize(&.{0x00}));
    try testing.expectEqual(@as(?usize, 3), try told.frameSize(&.{0x52}));
    try testing.expectEqual(@as(?usize, 9), try told.frameSize(&.{0x88}));

    // A variable type in extended mode needs its length field before it can say.
    try testing.expectEqual(@as(?usize, null), try told.frameSize(&.{0xC9}));
    try testing.expectEqual(@as(?usize, null), try told.frameSize(&.{ 0xC9, 0x01 }));
    try testing.expectEqual(@as(?usize, 303), try told.frameSize(&.{ 0xC9, 0x01, 0x2C }));

    // Widest possible prefix: 1 header + 8 length bytes.
    const widest = [_]u8{ told.headerByte(.buffer, 15), 0, 0, 0, 0, 0, 0, 0, 5 };
    try testing.expectEqual(@as(?usize, 14), try told.frameSize(&widest));
}

test "truncated frames are refused, not guessed at" {
    try testing.expectError(error.Truncated, told.decode(&.{}));
    try testing.expectError(error.Truncated, told.decode(&.{0x52})); // INT32 MOD=2, no data
    try testing.expectError(error.Truncated, told.decode(&.{ 0x52, 0x01 })); // one byte short
    try testing.expectError(error.Truncated, told.decode(&.{ 0x98, 0x08, 'a' })); // 8 declared, 1 present
    try testing.expectError(error.Truncated, told.decode(&.{0xC9})); // length field missing
    try testing.expectError(error.Truncated, told.decode(&.{ 0xC9, 0x01 })); // length field short
}

test "readFrame pulls one frame at a time across read boundaries" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    // Three frames back to back, the middle one long enough to need a length field.
    try enc.writeUint(9);
    try enc.writeString("a longer string that needs extended length");
    const obj = enc.beginObject();
    try enc.writeKey("k");
    try enc.writeBool(true);
    try enc.end(obj);

    // A deliberately tiny reader buffer, so frames span refills the way they do on TCP.
    var reader_buf: [8]u8 = undefined;
    var reader: std.Io.Reader = .fixed(out.items);
    _ = &reader_buf;

    var frame_buf: std.ArrayList(u8) = .empty;
    defer frame_buf.deinit(allocator);

    const first = try told.readFrame(&reader, allocator, &frame_buf, 4096);
    try testing.expectEqual(@as(u64, 9), try first.asUint());

    const second = try told.readFrame(&reader, allocator, &frame_buf, 4096);
    try testing.expectEqualStrings("a longer string that needs extended length", try second.asString());

    const third = try told.readFrame(&reader, allocator, &frame_buf, 4096);
    var it = try third.objectIterator();
    const pair = (try it.next()).?;
    try testing.expectEqualStrings("k", pair.key);
    try testing.expectEqual(true, try pair.value.asBool());

    try testing.expectError(error.EndOfStream, told.readFrame(&reader, allocator, &frame_buf, 4096));
}

test "readFrame caps attacker-declared lengths" {
    // A header claiming a 4 GB payload must be refused before anything is allocated.
    const hostile = [_]u8{ told.headerByte(.buffer, 11), 0xFF, 0xFF, 0xFF, 0xFF };
    var reader: std.Io.Reader = .fixed(&hostile);

    var frame_buf: std.ArrayList(u8) = .empty;
    defer frame_buf.deinit(allocator);

    try testing.expectError(error.FrameTooLarge, told.readFrame(&reader, allocator, &frame_buf, 1024));
}

// ---------------------------------------------------------------- validation

test "validate walks the whole tree" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const obj = enc.beginObject();
    try enc.writeKey("ok");
    const arr = enc.beginArray();
    try enc.writeString("fine");
    try enc.end(arr);
    try enc.end(obj);

    _ = try told.validate(out.items, 8);

    // Bad UTF-8 buried inside a nested array is found, where plain decode would not
    // look at it.
    // zig fmt: off
    const nested_bad = [_]u8{
        told.headerByte(.object, 7),
        0x92, 'o',  'k',
        told.headerByte(.array, 3),
        0x92, 0xFF, 0xFE,
    };
    // zig fmt: on
    _ = try told.decode(&nested_bad); // outer frame alone is well-formed
    try testing.expectError(error.InvalidUtf8, told.validate(&nested_bad, 8));
}

test "validate enforces a depth limit" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    var marks: [16]told.Encoder.Mark = undefined;
    for (0..16) |i| marks[i] = enc.beginArray();
    var i: usize = 16;
    while (i > 0) {
        i -= 1;
        try enc.end(marks[i]);
    }

    _ = try told.validate(out.items, 32);
    try testing.expectError(error.DepthExceeded, told.validate(out.items, 4));
}

// ---------------------------------------------------------------- inspection

test "stringify renders a frame for humans" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const obj = enc.beginObject();
    try enc.writeKey("seq");
    try enc.writeUint(7);
    try enc.writeKey("live");
    try enc.writeBool(true);
    try enc.writeKey("name");
    try enc.writeString("node-0");
    try enc.writeKey("miss");
    try enc.writeNull();
    try enc.writeKey("raw");
    try enc.writeBuffer(&.{ 0x01, 0x02 });
    try enc.end(obj);

    var text: std.ArrayList(u8) = .empty;
    defer text.deinit(allocator);
    try told.stringify(try told.decode(out.items), allocator, &text, 8);

    try testing.expectEqualStrings(
        \\{"seq":7,"live":true,"name":"node-0","miss":null,"raw":<buffer 2B 0102>}
    , text.items);
}

test "stringify escapes strings and keys" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    const obj = enc.beginObject();
    try enc.writeKey("a\"b");
    try enc.writeString("line\n\"quoted\" \\ tab\t bell\x07 é");
    try enc.end(obj);

    var text: std.ArrayList(u8) = .empty;
    defer text.deinit(allocator);
    try told.stringify(try told.decode(out.items), allocator, &text, 8);

    try testing.expectEqualStrings(
        \\{"a\"b":"line\n\"quoted\" \\ tab\t bell\u0007 é"}
    , text.items);
}

test "encoder byte counts match the spec's compactness claims" {
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(allocator);
    var enc = told.Encoder.init(allocator, &out);

    // A small object: {"a":1,"b":true} in 8 bytes.
    const obj = enc.beginObject();
    try enc.writeKey("a");
    try enc.writeUint(1);
    try enc.writeKey("b");
    try enc.writeBool(true);
    try enc.end(obj);

    try testing.expectEqual(@as(usize, 7), out.items.len);
    try testing.expectEqualSlices(u8, &.{ 0xB6, 0x91, 'a', 0x21, 0x91, 'b', 0x11 }, out.items);
}

// -- struct mapping ---------------------------------------------------------------------

const Mode = enum { idle, running, faulted };

const Reading = struct {
    channel: u8,
    value: f64,
    valid: bool = true,
};

const Sample = struct {
    id: u64,
    name: []const u8,
    mode: Mode,
    readings: []const Reading,
    raw: told.Bytes,
    note: ?[]const u8,
    seq: [3]u16,
};

test "pack and unpack a nested struct" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const readings = [_]Reading{
        .{ .channel = 1, .value = 3.5 },
        .{ .channel = 2, .value = -0.25, .valid = false },
    };

    const sent: Sample = .{
        .id = 99,
        .name = "sensor-a",
        .mode = .running,
        .readings = &readings,
        // Bytes on purpose: 0x00 and 0xFF would be a lie as text.
        .raw = .{ .data = &[_]u8{ 0x00, 0xFF, 0x0A } },
        .note = null,
        .seq = .{ 7, 8, 9 },
    };

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);
    try told.packTo(testing.allocator, &buf, sent);

    const got = try told.unpackFrom(Sample, buf.items, arena);

    try testing.expectEqual(sent.id, got.id);
    try testing.expectEqualStrings(sent.name, got.name);
    try testing.expectEqual(Mode.running, got.mode);
    try testing.expectEqual(@as(usize, 2), got.readings.len);
    try testing.expectEqual(@as(u8, 1), got.readings[0].channel);
    try testing.expectEqual(@as(f64, 3.5), got.readings[0].value);
    try testing.expect(got.readings[0].valid);
    try testing.expect(!got.readings[1].valid);
    try testing.expectEqualSlices(u8, sent.raw.data, got.raw.data);
    try testing.expect(got.note == null);
    try testing.expectEqualSlices(u16, &sent.seq, &got.seq);
}

test "a byte slice packs as a string and Bytes as a buffer" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    try told.packTo(testing.allocator, &buf, @as([]const u8, "text"));
    try testing.expectEqual(told.Type.string, (try told.decode(buf.items)).type);

    try told.packTo(testing.allocator, &buf, told.Bytes{ .data = "text" });
    try testing.expectEqual(told.Type.buffer, (try told.decode(buf.items)).type);
}

test "unpacking borrows from the frame when given no allocator" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    const Named = struct { name: []const u8 };
    try told.packTo(testing.allocator, &buf, Named{ .name = "borrowed" });

    const got = try told.unpackFrom(Named, buf.items, null);
    try testing.expectEqualStrings("borrowed", got.name);
    // Not a copy: it points into the frame we just encoded.
    const inside = @intFromPtr(got.name.ptr) >= @intFromPtr(buf.items.ptr) and
        @intFromPtr(got.name.ptr) < @intFromPtr(buf.items.ptr) + buf.items.len;
    try testing.expect(inside);
}

test "an unknown key is ignored and a missing field uses its default" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    // What a newer producer sends: an extra field, and `valid` left out.
    var e = told.Encoder.init(testing.allocator, &buf);
    const obj = e.beginObject();
    try e.writeKey("channel");
    try e.writeUint(4);
    try e.writeKey("value");
    try e.writeFloat64(1.5);
    try e.writeKey("added_later");
    try e.writeString("ignored");
    try e.end(obj);

    const got = try told.unpackFrom(Reading, buf.items, null);
    try testing.expectEqual(@as(u8, 4), got.channel);
    try testing.expectEqual(@as(f64, 1.5), got.value);
    try testing.expect(got.valid); // the struct's own default
}

test "a missing field with no default is an error, not a guess" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    var e = told.Encoder.init(testing.allocator, &buf);
    const obj = e.beginObject();
    try e.writeKey("channel");
    try e.writeUint(4);
    try e.end(obj); // `value` has no default

    try testing.expectError(error.MissingField, told.unpackFrom(Reading, buf.items, null));
}

test "an enum travels as an integer, and a name is still accepted" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    // What pack writes: the integer, so C, C++ and Python can read it without a name table.
    try told.packTo(testing.allocator, &buf, Mode.faulted);
    const as_int = try told.decode(buf.items);
    try testing.expectEqual(@as(i64, 2), try as_int.asInt());
    try testing.expectEqual(Mode.faulted, try told.unpackFrom(Mode, buf.items, null));

    // And what it tolerates: a name, for frames written by hand or by an older build.
    var e = told.Encoder.init(testing.allocator, &buf);
    buf.clearRetainingCapacity();
    try e.writeString("running");
    try testing.expectEqual(Mode.running, try told.unpackFrom(Mode, buf.items, null));

    // A value outside the enum is refused rather than cast into it.
    buf.clearRetainingCapacity();
    var e2 = told.Encoder.init(testing.allocator, &buf);
    try e2.writeInt(99);
    try testing.expectError(error.UnknownTag, told.unpackFrom(Mode, buf.items, null));
}

test "tuples map to arrays and tagged unions to a single-key object" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    const Triple = struct { u8, []const u8, bool };
    try told.packTo(testing.allocator, &buf, Triple{ 5, "mid", true });
    try testing.expectEqual(told.Type.array, (try told.decode(buf.items)).type);
    const triple = try told.unpackFrom(Triple, buf.items, arena);
    try testing.expectEqual(@as(u8, 5), triple[0]);
    try testing.expectEqualStrings("mid", triple[1]);
    try testing.expect(triple[2]);

    const Command = union(enum) { stop: void, set_speed: u32, label: []const u8 };
    try told.packTo(testing.allocator, &buf, Command{ .set_speed = 1200 });
    const cmd = try told.unpackFrom(Command, buf.items, arena);
    try testing.expectEqual(@as(u32, 1200), cmd.set_speed);
}

test "out of range and type mismatch are reported, not truncated" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    try told.packTo(testing.allocator, &buf, @as(u32, 70000));
    try testing.expectError(error.OutOfRange, told.unpackFrom(u8, buf.items, null));

    try told.packTo(testing.allocator, &buf, @as([]const u8, "not a number"));
    try testing.expectError(error.TypeMismatch, told.unpackFrom(u32, buf.items, null));
}

// -- cross-language golden frame --------------------------------------------------------
//
// The bytes below were produced by packing the struct in `InteropSample` and verified, at
// the time of writing, to be byte-for-byte what the C++ (TOLD_DEFINE_MAP) and Python
// (dataclass) mappings produce for the same values, and to be readable by all three.
//
// It is here so that a change to any mapping which alters the wire format fails a test
// rather than quietly breaking a peer written in another language. If this test fails
// after a deliberate format change, regenerate it -- but check the other two languages
// still agree first, because that is the property being pinned.
const golden_frame = @embedFile("testdata/interop.tld");

const InteropMode = enum(i32) { idle = 0, running = 1, faulted = 2 };
const InteropReading = struct { channel: u8, value: f64, valid: bool = true };
const InteropPoint = struct { x: f64, y: f64 };

const InteropSample = struct {
    id: u64,
    name: []const u8,
    mode: InteropMode,
    readings: []const InteropReading,
    raw: told.Bytes,
    note: ?[]const u8,
    origin: InteropPoint,
};

fn interopCanonical() InteropSample {
    return .{
        .id = 99,
        .name = "sensor-a",
        .mode = .faulted,
        .readings = &[_]InteropReading{
            .{ .channel = 1, .value = 3.5, .valid = true },
            .{ .channel = 2, .value = -0.25, .valid = false },
        },
        .raw = .{ .data = &[_]u8{ 0x00, 0xFF, 0x0A } },
        .note = null,
        .origin = .{ .x = 1.5, .y = -2.5 },
    };
}

test "the golden cross-language frame still decodes" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const got = try told.unpackFrom(InteropSample, golden_frame, arena_state.allocator());
    const want = interopCanonical();

    try testing.expectEqual(want.id, got.id);
    try testing.expectEqualStrings(want.name, got.name);
    try testing.expectEqual(want.mode, got.mode);
    try testing.expectEqual(want.readings.len, got.readings.len);
    for (want.readings, got.readings) |a, b| {
        try testing.expectEqual(a.channel, b.channel);
        try testing.expectEqual(a.value, b.value);
        try testing.expectEqual(a.valid, b.valid);
    }
    try testing.expectEqualSlices(u8, want.raw.data, got.raw.data);
    try testing.expect(got.note == null);
    try testing.expectEqual(want.origin.x, got.origin.x);
    try testing.expectEqual(want.origin.y, got.origin.y);
}

test "and packing still produces exactly those bytes" {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(testing.allocator);

    try told.packTo(testing.allocator, &buf, interopCanonical());
    try testing.expectEqualSlices(u8, golden_frame, buf.items);
}
