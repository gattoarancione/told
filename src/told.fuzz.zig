// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

//! Fuzz tests for `decode` and `validate`, the two functions that take untrusted bytes.
//!
//! Two drivers feed the same checks:
//!
//!   * A seeded mutational fuzzer, below. It mutates the seed corpus and randomly generated
//!     valid frames -- bit flips, interesting header bytes, inserts, deletes, splices,
//!     truncation -- and needs nothing from the toolchain. `zig build test` runs 10K inputs
//!     every time; `zig build fuzz -Dfuzz-iterations=10M -Dfuzz-seed=N` runs as many as you
//!     like, and a failure prints the seed, iteration and input needed to replay it.
//!
//!   * `std.testing.fuzz`, the coverage-guided fuzzer behind `zig build test --fuzz`. Its
//!     corpus pass runs with every `zig build test`. The fuzzing itself does not build with
//!     Zig 0.16.0, whose test runner fails to compile with -ffuzz (a StackTrace type
//!     mismatch in lib/compiler/test_runner.zig); it is kept so it works once that is fixed.
//!
//! Not crashing is the least these check. Each input is held to properties that must hold
//! for any bytes at all:
//!
//!   decode    the frame lies inside the input and its parts add up; frameSize agrees with
//!             it; every accessor and iterator on it returns a value or an error, never traps
//!   validate  whatever it accepts is safe to walk to that depth and to stringify; and
//!             re-encoding it yields the same values, and a second re-encoding the same bytes

const std = @import("std");
const told = @import("told.zig");
const testing = std.testing;

const max_input = 4096;
const max_depth = 16;

/// One of every type, the spec's examples, and the edge cases the unit tests pin, so the
/// fuzzers start from inputs that already reach deep into the decoder.
const seeds = [_][]const u8{
    &.{0x00},
    &.{0x11},
    &.{0x2F},
    &.{ 0x52, 0x01, 0x2C },
    &.{ 0x68, 0x80, 0, 0, 0, 0, 0, 0, 0 },
    &.{0x70},
    &.{ 0x88, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0 },
    &.{ 0x82, 0x3F, 0xF8 }, // 1.5 without its trailing zeros (1.1)
    &.{ 0x81, 0x80 }, // -0.0
    &.{ 0x72, 0x3F, 0xC0 },
    &.{ 0x75, 0, 0, 0, 0, 0 }, // FLOAT32 above its width
    &.{ 0x92, 'h', 'i' },
    &.{ 0x98, 0x08, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' },
    &.{ 0xA4, 0x21, 0x91, 'a', 0x00 },
    &.{ 0xB7, 0x91, 'z', 0x11, 0x91, 'a', 0xA1, 0x22 },
    &.{ 0xB7, 0x92, 'o', 'k', 0xA3, 0x92, 0xFF, 0xFE }, // bad UTF-8, nested
    &.{ 0xA2, 0x94, 'a' }, // child overruns its container
    &.{ 0xB2, 0x2A, 0x2B }, // object key is not a string
    &.{ 0xC9, 0x01, 0x2C }, // a long length field with no data
    &.{ 0xCF, 0, 0, 0, 0, 0, 0, 0, 5, 1, 2, 3, 4, 5 }, // the widest prefix
    &.{ 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, // length overflow
    &.{ 0xF0, 0x42, 1, 2, 3 },
    &.{ 0xA4, 0xA3, 0xA2, 0xA1, 0xA0 }, // four arrays deep
    &.{0xD0},
};

/// The seeds as Smith reads them outside fuzz mode: `slice` takes a little-endian u32
/// length first, then the bytes.
const corpus = blk: {
    var entries: [seeds.len][]const u8 = undefined;
    for (seeds, &entries) |bytes, *entry| {
        var len: [4]u8 = undefined;
        std.mem.writeInt(u32, &len, bytes.len, .little);
        const prefixed = len ++ bytes[0..bytes.len].*;
        entry.* = &prefixed;
    }
    const final = entries;
    break :blk final;
};

test "fuzz decode" {
    try testing.fuzz({}, fuzzDecode, .{ .corpus = &corpus });
}

test "fuzz validate" {
    try testing.fuzz({}, fuzzValidate, .{ .corpus = &corpus });
}

fn fuzzDecode(_: void, smith: *testing.Smith) anyerror!void {
    var buf: [max_input]u8 = undefined;
    try checkDecode(buf[0..smith.slice(&buf)]);
}

fn fuzzValidate(_: void, smith: *testing.Smith) anyerror!void {
    var buf: [max_input]u8 = undefined;
    try checkValidate(testing.allocator, buf[0..smith.slice(&buf)]);
}

fn checkDecode(input: []const u8) !void {
    const frame = told.decode(input) catch |err| {
        // A prefix decode rejects as cut short must not be sized by frameSize as complete.
        if (err == error.Truncated) {
            if (told.frameSize(input)) |size| {
                if (size) |n| try testing.expect(n > input.len);
            } else |_| {}
        }
        return;
    };

    // The frame is a prefix of the input, and its parts add up.
    try testing.expectEqual(input.ptr, frame.raw.ptr);
    try testing.expect(frame.raw.len <= input.len);
    try testing.expectEqual(frame.raw.len, frame.size());
    try testing.expectEqual(frame.raw.len, frame.prefix_len + frame.data.len);
    try testing.expectEqual(frame.raw.ptr + frame.prefix_len, frame.data.ptr);

    // frameSize must say the same thing without decoding -- except for EXTENDED, whose
    // extent only the extension knows.
    if (frame.type == .extended) {
        try testing.expectError(error.ExtensionFramingUnknown, told.frameSize(input));
    } else {
        try testing.expectEqual(@as(?usize, frame.raw.len), try told.frameSize(input));
    }

    // Decoding the frame's own bytes, with nothing after them, is the same frame.
    const again = try told.decode(frame.raw);
    try testing.expectEqual(frame.raw.len, again.raw.len);

    touch(frame, max_depth);
}

/// Call every accessor and walk every container, ignoring the errors. A frame that has
/// only been decoded, not validated, may hold anything inside, and none of it may trap.
fn touch(frame: told.Frame, depth_left: usize) void {
    _ = frame.asBool() catch {};
    _ = frame.asInt() catch {};
    _ = frame.asUint() catch {};
    _ = frame.asFloat() catch {};
    _ = frame.asString() catch {};
    _ = frame.asBuffer() catch {};
    _ = frame.asBytes() catch {};
    if (depth_left == 0) return;

    if (frame.arrayIterator()) |it_const| {
        var it = it_const;
        while (it.next() catch null) |child| touch(child, depth_left - 1);
    } else |_| {}

    if (frame.objectIterator()) |it_const| {
        var it = it_const;
        while (it.next() catch null) |pair| touch(pair.value, depth_left - 1);
    } else |_| {}
}

fn checkValidate(allocator: std.mem.Allocator, input: []const u8) !void {
    const frame = told.validate(input, max_depth) catch return;

    // validate's promise: everything inside walks cleanly to this depth, so a walk that
    // trusts it (`try` on every step) must not fail.
    try walk(frame, max_depth);

    // And it renders. stringify fails only on depth, which validate already bounded.
    var text: std.ArrayList(u8) = .empty;
    defer text.deinit(allocator);
    try told.stringify(frame, allocator, &text, max_depth);

    // EXTENDED frames carry a payload only the extension can delimit, so inside a
    // container they make the walk diverge from what a re-encoding would write. They are
    // covered by the properties above; the round trip is for everything else.
    if (containsExtended(frame, max_depth)) return;

    // Re-encoding gives the same values in the encoder's own, canonical form...
    var first: std.ArrayList(u8) = .empty;
    defer first.deinit(allocator);
    var enc = told.Encoder.init(allocator, &first);
    try reencode(&enc, frame);
    const canonical = try told.validate(first.items, max_depth);
    try expectSameValue(frame, canonical);

    // ...and that form is a fixed point: encoding it again changes nothing, whether the
    // encoder starts from its default container header guesses...
    var second: std.ArrayList(u8) = .empty;
    defer second.deinit(allocator);
    var enc2 = told.Encoder.init(allocator, &second);
    try reencode(&enc2, canonical);
    try testing.expectEqualSlices(u8, first.items, second.items);

    // ...or from the ones the first pass left behind.
    var third: std.ArrayList(u8) = .empty;
    defer third.deinit(allocator);
    var enc3 = enc;
    enc3.out = &third;
    try reencode(&enc3, canonical);
    try testing.expectEqualSlices(u8, first.items, third.items);
}

fn walk(frame: told.Frame, depth_left: usize) !void {
    switch (frame.type) {
        .string => _ = try frame.asString(),
        .array => {
            var it = try frame.arrayIterator();
            while (try it.next()) |child| try walk(child, depth_left - 1);
        },
        .object => {
            var it = try frame.objectIterator();
            while (try it.next()) |pair| try walk(pair.value, depth_left - 1);
        },
        else => {},
    }
}

fn containsExtended(frame: told.Frame, depth_left: usize) bool {
    switch (frame.type) {
        .extended => return true,
        .array => {
            var it = frame.arrayIterator() catch return false;
            while (it.next() catch null) |child| {
                if (containsExtended(child, depth_left - 1)) return true;
            }
        },
        .object => {
            var it = frame.objectIterator() catch return false;
            while (it.next() catch null) |pair| {
                if (containsExtended(pair.value, depth_left - 1)) return true;
            }
        },
        else => {},
    }
    return false;
}

/// Write `frame` back out through the encoder, which picks the shortest legal form.
fn reencode(enc: *told.Encoder, frame: told.Frame) !void {
    switch (frame.type) {
        .null => try enc.writeNull(),
        .bool => try enc.writeBool(try frame.asBool()),
        .tinyuint => try enc.writeUint(try frame.asUint()),
        .int8, .int16, .int32, .int64 => try enc.writeInt(try frame.asInt()),
        .float32 => try enc.writeFloat32(try frame.asF32()),
        .float64 => try enc.writeFloat64(try frame.asF64()),
        .string => try enc.writeString(try frame.asString()),
        .buffer => try enc.writeBuffer(try frame.asBuffer()),
        .array => {
            const mark = try enc.beginArray();
            var it = try frame.arrayIterator();
            while (try it.next()) |child| try reencode(enc, child);
            try enc.end(mark);
        },
        .object => {
            const mark = try enc.beginObject();
            var it = try frame.objectIterator();
            while (try it.next()) |pair| {
                try enc.writeKey(pair.key);
                try reencode(enc, pair.value);
            }
            try enc.end(mark);
        },
        .extended, .reserved_d, .reserved_e => unreachable, // excluded or refused upstream
    }
}

/// Same value, by the reading a consumer would use: integers by value whatever their
/// declared width, floats by bit pattern so NaN payloads and -0.0 count.
fn expectSameValue(a: told.Frame, b: told.Frame) !void {
    switch (a.type) {
        .tinyuint, .int8, .int16, .int32, .int64 => {
            try testing.expectEqual(try a.asInt(), try b.asInt());
        },
        .float32 => try testing.expectEqual(
            @as(u32, @bitCast(try a.asF32())),
            @as(u32, @bitCast(try b.asF32())),
        ),
        .float64 => try testing.expectEqual(
            @as(u64, @bitCast(try a.asF64())),
            @as(u64, @bitCast(try b.asF64())),
        ),
        .null => try testing.expect(b.isNull()),
        .bool => try testing.expectEqual(try a.asBool(), try b.asBool()),
        .string => try testing.expectEqualStrings(try a.asString(), try b.asString()),
        .buffer => try testing.expectEqualSlices(u8, try a.asBuffer(), try b.asBuffer()),
        .array => {
            var ia = try a.arrayIterator();
            var ib = try b.arrayIterator();
            while (try ia.next()) |ca| try expectSameValue(ca, (try ib.next()).?);
            try testing.expect((try ib.next()) == null);
        },
        .object => {
            var ia = try a.objectIterator();
            var ib = try b.objectIterator();
            while (try ia.next()) |pa| {
                const pb = (try ib.next()).?;
                try testing.expectEqualStrings(pa.key, pb.key);
                try expectSameValue(pa.value, pb.value);
            }
            try testing.expect((try ib.next()) == null);
        },
        .extended, .reserved_d, .reserved_e => unreachable,
    }
}

// ---------------------------------------------------------------- mutational fuzzer

const options = @import("fuzz_options");

test "mutational fuzz of decode and validate" {
    var prng = std.Random.DefaultPrng.init(options.seed);
    const random = prng.random();

    var buf: [max_input]u8 = undefined;
    var scratch: std.ArrayList(u8) = .empty;
    defer scratch.deinit(testing.allocator);

    var i: u64 = 0;
    while (i < options.iterations) : (i += 1) {
        const next = try nextInput(random, &buf, &scratch);
        const input = next.bytes;
        // The encoder's own output must be valid. Without this a broken encoder would only
        // produce inputs validate rejects, which the checks below treat as uninteresting.
        if (next.must_validate) {
            _ = told.validate(input, max_depth) catch |err| return report(err, i, input);
        }
        checkDecode(input) catch |err| return report(err, i, input);
        checkValidate(testing.allocator, input) catch |err| return report(err, i, input);
    }
}

fn report(err: anyerror, iteration: u64, input: []const u8) anyerror {
    std.debug.print(
        "\nfuzz failure: error.{t} at iteration {d} of -Dfuzz-seed={d}\ninput ({d} bytes): {x}\n",
        .{ err, iteration, options.seed, input.len, input },
    );
    return err;
}

const Input = struct {
    bytes: []const u8,
    /// Straight from the encoder, unmutated, within the depth limit and free of EXTENDED
    /// frames (whose payload a generic decoder cannot delimit inside a container).
    must_validate: bool,
};

/// A seed, a freshly generated valid frame, or random bytes, then a few mutations. Valid
/// frames are sometimes left unmutated so the round-trip checks see plenty of them.
fn nextInput(random: std.Random, buf: *[max_input]u8, scratch: *std.ArrayList(u8)) !Input {
    var pristine = false;
    var len: usize = switch (random.uintLessThan(u8, 10)) {
        0...3 => copyIn(buf, seeds[random.uintLessThan(usize, seeds.len)]),
        4...7 => gen: {
            scratch.clearRetainingCapacity();
            var enc = told.Encoder.init(testing.allocator, scratch);
            var gen: Gen = .{ .random = random };
            const depth = random.uintAtMost(usize, max_depth + 2);
            try gen.value(&enc, depth);
            pristine = depth <= max_depth and !gen.saw_extended and scratch.items.len <= buf.len;
            break :gen copyIn(buf, scratch.items);
        },
        else => raw: {
            const n = random.uintAtMost(usize, 64);
            random.bytes(buf[0..n]);
            break :raw n;
        },
    };

    var rounds = random.uintAtMost(u8, 6);
    if (random.uintLessThan(u8, 4) == 0) rounds = 0;
    const must_validate = pristine and rounds == 0;
    while (rounds > 0) : (rounds -= 1) len = mutate(random, buf, len);
    return .{ .bytes = buf[0..len], .must_validate = must_validate };
}

fn copyIn(buf: *[max_input]u8, bytes: []const u8) usize {
    const n = @min(bytes.len, buf.len);
    @memcpy(buf[0..n], bytes[0..n]);
    return n;
}

/// Bytes a decoder is most likely to mishandle: the boundaries of every MOD range, and a
/// header for every type nibble.
const interesting = [_]u8{ 0x00, 0x01, 0x07, 0x08, 0x0F, 0x7F, 0x80, 0xFE, 0xFF };

fn mutate(random: std.Random, buf: *[max_input]u8, len: usize) usize {
    switch (random.uintLessThan(u8, 8)) {
        0 => if (len > 0) { // flip a bit
            buf[random.uintLessThan(usize, len)] ^= @as(u8, 1) << random.int(u3);
        },
        1 => if (len > 0) { // overwrite a byte
            buf[random.uintLessThan(usize, len)] = random.int(u8);
        },
        2 => if (len > 0) { // an interesting byte, or a header for any type and MOD
            buf[random.uintLessThan(usize, len)] = if (random.boolean())
                interesting[random.uintLessThan(usize, interesting.len)]
            else
                (@as(u8, random.int(u4)) << 4) | random.int(u4);
        },
        3 => if (len < buf.len) { // insert a byte
            const at = random.uintAtMost(usize, len);
            std.mem.copyBackwards(u8, buf[at + 1 .. len + 1], buf[at..len]);
            buf[at] = random.int(u8);
            return len + 1;
        },
        4 => if (len > 0) { // delete a byte
            const at = random.uintLessThan(usize, len);
            std.mem.copyForwards(u8, buf[at .. len - 1], buf[at + 1 .. len]);
            return len - 1;
        },
        5 => if (len > 0) { // duplicate a run, which is how lengths stop matching contents
            const start = random.uintLessThan(usize, len);
            const n = @min(random.uintAtMost(usize, len - start), buf.len - len);
            var run: [max_input]u8 = undefined;
            @memcpy(run[0..n], buf[start .. start + n]);
            const at = random.uintAtMost(usize, len);
            std.mem.copyBackwards(u8, buf[at + n .. len + n], buf[at..len]);
            @memcpy(buf[at .. at + n], run[0..n]);
            return len + n;
        },
        6 => if (len > 0) return random.uintLessThan(usize, len), // truncate
        else => { // splice in part of a seed
            const s = seeds[random.uintLessThan(usize, seeds.len)];
            const at = random.uintAtMost(usize, len);
            const n = @min(s.len, buf.len - at);
            @memcpy(buf[at .. at + n], s[0..n]);
            return @max(len, at + n);
        },
    }
    return len;
}

const alphabet = [_][]const u8{ "a", "z", "0", " ", "\"", "\\", "\n", "\t", "\x00", "é", "€", "𝄞" };

fn genString(random: std.Random, out: []u8) []const u8 {
    var n: usize = 0;
    var count = random.uintAtMost(usize, 12);
    while (count > 0) : (count -= 1) {
        const piece = alphabet[random.uintLessThan(usize, alphabet.len)];
        if (n + piece.len > out.len) break;
        @memcpy(out[n .. n + piece.len], piece);
        n += piece.len;
    }
    return out[0..n];
}

/// Generates random but valid values, noting whether it wrote an EXTENDED frame.
const Gen = struct {
    random: std.Random,
    saw_extended: bool = false,

    /// `depth` bounds nesting; it may exceed what the checks allow, so the depth limit gets
    /// exercised too.
    fn value(gen: *Gen, enc: *told.Encoder, depth: usize) !void {
        const random = gen.random;
        var text: [64]u8 = undefined;
        var bytes: [300]u8 = undefined;
        switch (random.uintLessThan(u8, if (depth == 0) 9 else 11)) {
            0 => try enc.writeNull(),
            1 => try enc.writeBool(random.boolean()),
            2 => try enc.writeUint(random.int(u64) >> random.int(u6)),
            3 => try enc.writeInt(random.int(i64) >> random.int(u6)),
            4 => try enc.writeFloat32(@bitCast(random.int(u32))),
            5 => try enc.writeFloat64(@bitCast(random.int(u64))),
            6 => try enc.writeString(genString(random, &text)),
            7 => {
                // Past 255 bytes the length field needs two bytes.
                const n = random.uintAtMost(usize, if (random.boolean()) 16 else bytes.len);
                random.bytes(bytes[0..n]);
                try enc.writeBuffer(bytes[0..n]);
            },
            8 => {
                const n = random.uintAtMost(usize, 4);
                random.bytes(bytes[0..n]);
                try enc.writeExtended(random.int(u8), bytes[0..n]);
                gen.saw_extended = true;
            },
            9 => {
                const mark = try enc.beginArray();
                var n = random.uintAtMost(usize, 6);
                while (n > 0) : (n -= 1) try gen.value(enc, depth - 1);
                try enc.end(mark);
            },
            else => {
                const mark = try enc.beginObject();
                var n = random.uintAtMost(usize, 5);
                while (n > 0) : (n -= 1) {
                    try enc.writeKey(genString(random, &text));
                    try gen.value(enc, depth - 1);
                }
                try enc.end(mark);
            },
        }
    }
};
