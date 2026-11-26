// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

//! C ABI for the TOLD codec. Every symbol here is `export ... callconv(.c)` and every
//! signature is expressible in C: slices become (pointer, length) pairs, error unions
//! become status codes, and the one stateful type — the encoder — becomes an opaque
//! handle. The declarations must stay in step with `include/told.h`, which is
//! hand-written because Zig no longer emits C headers.
//!
//! TOLD is the natural first thing to expose: `told.zig` has no globals, no threads and no
//! `Io` outside `readFrame`, so this layer needs no lifecycle of its own.
//!
//! Decoding stays zero-copy across the boundary. A frame's `data`/`raw` pointers borrow
//! the caller's buffer, so they are valid exactly as long as that buffer is — the C side
//! owns the bytes throughout and nothing is copied in either direction.

const std = @import("std");
const told = @import("told.zig");
const abi = @import("told_abi.zig");
const allocator = @import("told_alloc.zig").allocator;

/// Mirrors told_status in the header. Values are part of the ABI: append, never
/// renumber.
const Status = enum(c_int) {
    ok = 0,
    truncated = 1,
    invalid_mod = 2,
    reserved_type = 3,
    malformed_container = 4,
    invalid_utf8 = 5,
    length_overflow = 6,
    depth_exceeded = 7,
    type_mismatch = 8,
    extension_framing_unknown = 9,
    out_of_memory = 10,
    /// `told_frame_size` only: the prefix is too short to tell yet, read more bytes.
    incomplete = 11,
    /// A null pointer where one was required.
    invalid_argument = 12,
    unexpected = 13,
    /// `told_read_frame` only: the stream ended cleanly, between frames.
    end_of_stream = 14,
    /// `told_read_frame` only: the read callback reported an error.
    read_failed = 15,
    /// `told_read_frame` only: the frame does not fit the caller's buffer.
    frame_too_large = 16,
    /// Struct mapping: the value does not fit the member's type.
    out_of_range = 17,
};

fn statusFor(err: anyerror) Status {
    return switch (err) {
        error.Truncated => .truncated,
        error.InvalidMod => .invalid_mod,
        error.ReservedType => .reserved_type,
        error.MalformedContainer => .malformed_container,
        error.InvalidUtf8 => .invalid_utf8,
        error.LengthOverflow => .length_overflow,
        error.DepthExceeded => .depth_exceeded,
        error.TypeMismatch => .type_mismatch,
        error.ExtensionFramingUnknown => .extension_framing_unknown,
        error.OutOfMemory => .out_of_memory,
        error.OutOfRange => .out_of_range,
        else => .unexpected,
    };
}

/// The C ABI frame shape, from the one file that declares it. See src/told_abi.zig for why
/// it is not declared here, and why it cannot simply be imported from the node library.
const Frame = abi.Frame;

fn toCFrame(f: told.Frame) Frame {
    return .{
        .type = @intFromEnum(f.type),
        .mod = f.mod,
        .ext_code = if (f.ext_code) |code| @intCast(code) else -1,
        .data = f.data.ptr,
        .data_len = f.data.len,
        .raw = f.raw.ptr,
        .raw_len = f.raw.len,
    };
}

fn fromCFrame(f: *const Frame) told.Frame {
    return .{
        .type = @enumFromInt(f.type),
        .mod = @truncate(f.mod),
        .prefix_len = f.raw_len - f.data_len,
        .data = f.data[0..f.data_len],
        .raw = f.raw[0..f.raw_len],
        .ext_code = if (f.ext_code < 0) null else @intCast(f.ext_code),
    };
}

/// Mirrors told_iter: a cursor over the not-yet-consumed bytes of a container.
const Iter = extern struct {
    rest: [*]const u8,
    rest_len: usize,
};

// ---------------------------------------------------------------- decoding

export fn told_decode(bytes: ?[*]const u8, len: usize, out: ?*Frame) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const frame = told.decode(src[0..len]) catch |err| return statusFor(err);
    dst.* = toCFrame(frame);
    return .ok;
}

/// Total size of the frame starting at `bytes`, or `incomplete` when the prefix is too
/// short to say. This is what lets a C reader buffer exactly one frame off a socket
/// without parsing it: at most 9 bytes are needed to decide.
export fn told_frame_size(bytes: ?[*]const u8, len: usize, out: ?*usize) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const size = told.frameSize(src[0..len]) catch |err| return statusFor(err);
    dst.* = size orelse return .incomplete;
    return .ok;
}

/// Walk a whole frame, checking nested frames, UTF-8 in strings and object shape.
/// `told_decode` only validates the outermost frame, so use this on untrusted input.
export fn told_validate(bytes: ?[*]const u8, len: usize, max_depth: usize) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    _ = told.validate(src[0..len], max_depth) catch |err| return statusFor(err);
    return .ok;
}

// ---------------------------------------------------------------- stream reading
//
// `readFrame` takes a std.Io.Reader and an allocator, neither of which C has. The C shape
// is a read callback and a caller-owned buffer: no allocation, and the frame borrows the
// buffer exactly as `told_decode`'s borrows its input.

/// Mirrors told_read_fn: fill up to `len` bytes of `dst`, returning the count (short reads
/// are fine), 0 at end of stream, or a negative value on error.
const ReadFn = *const fn (ctx: ?*anyopaque, dst: [*]u8, len: usize) callconv(.c) isize;

const FillError = error{ EndOfStream, ReadFailed };

/// Call `read` until `dst` is full. Never asks for more than `dst.len`, which is what keeps
/// the reader from consuming any byte past the current frame.
fn fill(read: ReadFn, ctx: ?*anyopaque, dst: []u8) FillError!void {
    var got: usize = 0;
    while (got < dst.len) {
        const n = read(ctx, dst[got..].ptr, dst.len - got);
        if (n < 0) return error.ReadFailed;
        if (n == 0) return error.EndOfStream;
        // A callback claiming more than it was asked for has already written past `dst`;
        // the least that can be done is not to believe it.
        if (@as(usize, @intCast(n)) > dst.len - got) return error.ReadFailed;
        got += @intCast(n);
    }
}

/// End of stream once the header byte is in is a frame cut short, not a clean end.
fn midFrame(err: FillError) Status {
    return switch (err) {
        error.EndOfStream => .truncated,
        error.ReadFailed => .read_failed,
    };
}

/// Read exactly one frame into `buf` and decode it. At most `cap` bytes are written, and
/// no byte past the frame is requested, so the next call starts on the next header and the
/// stream is left positioned for whatever else shares it.
///
/// After `frame_too_large`, `read_failed` or `truncated` the stream is mid-frame, and
/// there is no way back into step short of the application's own framing: close it.
export fn told_read_frame(
    read: ?ReadFn,
    ctx: ?*anyopaque,
    buf: ?[*]u8,
    cap: usize,
    out: ?*Frame,
) callconv(.c) Status {
    const read_fn = read orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    const ptr = buf orelse return .invalid_argument;
    if (cap == 0) return .invalid_argument;
    const b = ptr[0..cap];

    // End of stream before the header byte is the clean case: the peer stopped between
    // frames, which is how a stream of frames normally ends.
    fill(read_fn, ctx, b[0..1]) catch |err| return switch (err) {
        error.EndOfStream => .end_of_stream,
        error.ReadFailed => .read_failed,
    };

    var have: usize = 1;
    const total = while (true) {
        const size = told.frameSize(b[0..have]) catch |err| return statusFor(err);
        if (size) |n| break n;
        // Only a long length field leaves the size open: MOD 8..15 is 1..8 more bytes.
        const need = 1 + @as(usize, told.Header.parse(b[0]).mod) - 7;
        if (need > cap) return .frame_too_large;
        fill(read_fn, ctx, b[have..need]) catch |err| return midFrame(err);
        have = need;
    };

    if (total > cap) return .frame_too_large;
    fill(read_fn, ctx, b[have..total]) catch |err| return midFrame(err);

    const frame = told.decode(b[0..total]) catch |err| return statusFor(err);
    dst.* = toCFrame(frame);
    return .ok;
}

// ---------------------------------------------------------------- accessors

export fn told_as_bool(frame: ?*const Frame, out: ?*bool) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asBool() catch |err| return statusFor(err);
    return .ok;
}

export fn told_as_i64(frame: ?*const Frame, out: ?*i64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asInt() catch |err| return statusFor(err);
    return .ok;
}

export fn told_as_u64(frame: ?*const Frame, out: ?*u64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asUint() catch |err| return statusFor(err);
    return .ok;
}

export fn told_as_f64(frame: ?*const Frame, out: ?*f64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asFloat() catch |err| return statusFor(err);
    return .ok;
}

/// STRING payload, UTF-8 validated. Borrows the caller's buffer; nothing is copied.
export fn told_as_string(frame: ?*const Frame, ptr: ?*[*]const u8, len: ?*usize) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const p = ptr orelse return .invalid_argument;
    const l = len orelse return .invalid_argument;

    const text = fromCFrame(f).asString() catch |err| return statusFor(err);
    p.* = text.ptr;
    l.* = text.len;
    return .ok;
}

/// BUFFER payload: opaque bytes, no encoding contract.
export fn told_as_buffer(frame: ?*const Frame, ptr: ?*[*]const u8, len: ?*usize) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const p = ptr orelse return .invalid_argument;
    const l = len orelse return .invalid_argument;

    const bytes = fromCFrame(f).asBuffer() catch |err| return statusFor(err);
    p.* = bytes.ptr;
    l.* = bytes.len;
    return .ok;
}

// ---------------------------------------------------------------- iteration

export fn told_array_iter(frame: ?*const Frame, out: ?*Iter) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const it = fromCFrame(f).arrayIterator() catch |err| return statusFor(err);
    dst.* = .{ .rest = it.rest.ptr, .rest_len = it.rest.len };
    return .ok;
}

export fn told_object_iter(frame: ?*const Frame, out: ?*Iter) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const it = fromCFrame(f).objectIterator() catch |err| return statusFor(err);
    dst.* = .{ .rest = it.rest.ptr, .rest_len = it.rest.len };
    return .ok;
}

/// Next array element. `*have` is false at the end, which is the loop condition; the
/// status is reserved for genuinely malformed input.
export fn told_array_next(iter: ?*Iter, out: ?*Frame, have: ?*bool) callconv(.c) Status {
    const cursor = iter orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    const flag = have orelse return .invalid_argument;

    var zig_it: told.Iterator = .{ .rest = cursor.rest[0..cursor.rest_len] };
    const next = zig_it.next() catch |err| {
        flag.* = false;
        return statusFor(err);
    };

    cursor.* = .{ .rest = zig_it.rest.ptr, .rest_len = zig_it.rest.len };
    if (next) |frame| {
        dst.* = toCFrame(frame);
        flag.* = true;
    } else {
        flag.* = false;
    }
    return .ok;
}

/// Next object key/value pair, in wire order, which section 7.3 makes significant.
export fn told_object_next(
    iter: ?*Iter,
    key: ?*[*]const u8,
    key_len: ?*usize,
    value: ?*Frame,
    have: ?*bool,
) callconv(.c) Status {
    const cursor = iter orelse return .invalid_argument;
    const k = key orelse return .invalid_argument;
    const kl = key_len orelse return .invalid_argument;
    const v = value orelse return .invalid_argument;
    const flag = have orelse return .invalid_argument;

    var zig_it: told.ObjectIterator = .{ .rest = cursor.rest[0..cursor.rest_len] };
    const next = zig_it.next() catch |err| {
        flag.* = false;
        return statusFor(err);
    };

    cursor.* = .{ .rest = zig_it.rest.ptr, .rest_len = zig_it.rest.len };
    if (next) |pair| {
        k.* = pair.key.ptr;
        kl.* = pair.key.len;
        v.* = toCFrame(pair.value);
        flag.* = true;
    } else {
        flag.* = false;
    }
    return .ok;
}

// ---------------------------------------------------------------- encoding

/// The encoder is the one piece with state, so it crosses as an opaque handle. It owns
/// its buffer; the caller borrows the bytes via `told_encoder_bytes` and must copy
/// them before the next write, since appending may reallocate.
const Encoder = struct {
    list: std.ArrayList(u8),
};

export fn told_encoder_create() callconv(.c) ?*Encoder {
    const enc = allocator.create(Encoder) catch return null;
    enc.* = .{ .list = .empty };
    return enc;
}

export fn told_encoder_destroy(handle: ?*Encoder) callconv(.c) void {
    const enc = handle orelse return; // free(NULL) is a no-op, so this one is too
    enc.list.deinit(allocator);
    allocator.destroy(enc);
}

/// Drop the contents, keeping the capacity — for reusing one encoder per message.
export fn told_encoder_reset(handle: ?*Encoder) callconv(.c) void {
    const enc = handle orelse return;
    enc.list.clearRetainingCapacity();
}

export fn told_encoder_bytes(handle: ?*const Encoder, len: ?*usize) callconv(.c) ?[*]const u8 {
    const enc = handle orelse return null;
    if (len) |l| l.* = enc.list.items.len;
    return enc.list.items.ptr;
}

fn writer(handle: ?*Encoder) ?told.Encoder {
    const enc = handle orelse return null;
    return told.Encoder.init(allocator, &enc.list);
}

export fn told_write_null(handle: ?*Encoder) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeNull() catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_bool(handle: ?*Encoder, value: bool) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeBool(value) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_u64(handle: ?*Encoder, value: u64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeUint(value) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_i64(handle: ?*Encoder, value: i64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeInt(value) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_f32(handle: ?*Encoder, value: f32) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeFloat32(value) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_f64(handle: ?*Encoder, value: f64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeFloat64(value) catch |err| return statusFor(err);
    return .ok;
}

/// A caller's (pointer, length) pair as a slice, treating NULL with length 0 as empty.
///
/// That combination is how C spells an empty buffer, and both an empty STRING and an empty
/// BUFFER are legal TOLD values. Refusing it made them unrepresentable from C++ without the
/// caller knowing to substitute a dummy pointer: `std::vector<T>::data()` on an empty
/// vector is permitted to return null, so `told::pack` of a struct with an empty bytes
/// field threw invalid_argument -- which for the CAN station meant a frame with no payload,
/// a legal frame with DLC 0, taking the whole window down with an uncaught exception.
fn inputSlice(bytes: ?[*]const u8, len: usize) ?[]const u8 {
    if (len == 0) return &.{};
    const src = bytes orelse return null;
    return src[0..len];
}

/// Rejects invalid UTF-8 rather than emitting a STRING a decoder is entitled to refuse.
export fn told_write_string(handle: ?*Encoder, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = inputSlice(bytes, len) orelse return .invalid_argument;
    enc.writeString(src) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_buffer(handle: ?*Encoder, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = inputSlice(bytes, len) orelse return .invalid_argument;
    enc.writeBuffer(src) catch |err| return statusFor(err);
    return .ok;
}

export fn told_write_extended(handle: ?*Encoder, code: u8, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = inputSlice(bytes, len) orelse return .invalid_argument;
    enc.writeExtended(code, src) catch |err| return statusFor(err);
    return .ok;
}

// A container's length is only known once its children are written, so the header is
// inserted at the end. The mark is just the offset where the container started — a plain
// size_t in C, with separate array/object closers so the type cannot be lost.
export fn told_begin_array(handle: ?*Encoder, mark: ?*usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const m = mark orelse return .invalid_argument;
    m.* = enc.beginArray().start;
    return .ok;
}

export fn told_begin_object(handle: ?*Encoder, mark: ?*usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const m = mark orelse return .invalid_argument;
    m.* = enc.beginObject().start;
    return .ok;
}

export fn told_end_array(handle: ?*Encoder, mark: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    // A mark past the end would underflow the length below: a trap in safe builds and
    // undefined behaviour in fast ones, where C callers are owed a status.
    if (mark > enc.out.items.len) return .invalid_argument;
    enc.end(.{ .start = mark, .type = .array }) catch |err| return statusFor(err);
    return .ok;
}

export fn told_end_object(handle: ?*Encoder, mark: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    if (mark > enc.out.items.len) return .invalid_argument;
    enc.end(.{ .start = mark, .type = .object }) catch |err| return statusFor(err);
    return .ok;
}

// ---------------------------------------------------------------- diagnostics

export fn told_status_name(status: Status) callconv(.c) [*:0]const u8 {
    return switch (status) {
        .ok => "ok",
        .truncated => "truncated",
        .invalid_mod => "invalid mod",
        .reserved_type => "reserved type",
        .malformed_container => "malformed container",
        .invalid_utf8 => "invalid utf-8",
        .length_overflow => "length overflow",
        .depth_exceeded => "depth exceeded",
        .type_mismatch => "type mismatch",
        .extension_framing_unknown => "extension framing unknown",
        .out_of_memory => "out of memory",
        .incomplete => "incomplete",
        .invalid_argument => "invalid argument",
        .unexpected => "unexpected",
        .end_of_stream => "end of stream",
        .read_failed => "read failed",
        .frame_too_large => "frame too large",
        .out_of_range => "out of range",
    };
}

export fn told_version() callconv(.c) [*:0]const u8 {
    return told.version;
}

// ---------------------------------------------------------------- struct mapping
//
// C has neither templates nor reflection, so a struct describes itself once as a table of
// fields and the generic pack/unpack below walk it. This is the same idea as the C++
// TOLD_DEFINE_MAP macros and the Zig comptime mapping, expressed the only way C allows:
// offsetof, a kind tag, and a size.
//
// Deliberate limits, because C has no container to decode into:
//
//   * strings are fixed char[N] buffers, packed up to the first NUL and unpacked
//     NUL-terminated and truncated to fit;
//   * byte buffers are fixed arrays plus a length member;
//   * arrays are fixed-length, of a scalar or of a nested struct;
//   * anything dynamic stays manual -- use the encoder and iterator directly for that.
//
// A field whose key is absent from the frame is left as the caller had it, so zero the
// struct first if that matters. Unknown keys in the frame are ignored, as everywhere else.

const FieldKind = enum(c_int) {
    boolean = 0,
    i8 = 1,
    i16 = 2,
    i32 = 3,
    i64 = 4,
    u8 = 5,
    u16 = 6,
    u32 = 7,
    u64 = 8,
    f32 = 9,
    f64 = 10,
    /// char[size], NUL-terminated on both sides.
    string = 11,
    /// uint8_t[size] paired with a length member named by `length_offset`.
    bytes = 12,
    /// A nested struct described by `nested`.
    nested = 13,
    /// A fixed-length array of `count` elements, each `element_size` bytes and described
    /// by `element_kind` (or `nested` when that is `.nested`).
    array = 14,
    _,
};

const FieldDesc = extern struct {
    key: ?[*:0]const u8,
    offset: usize,
    kind: FieldKind,
    /// Byte size of the member: the buffer length for `string`/`bytes`, otherwise unused.
    size: usize = 0,
    /// `bytes` only: offset of the size_t member holding the used length.
    length_offset: usize = 0,
    /// `nested`, and `array` of nested.
    nested: ?*const StructDesc = null,
    /// `array` only.
    element_kind: FieldKind = .boolean,
    element_size: usize = 0,
    count: usize = 0,
};

const StructDesc = extern struct {
    name: ?[*:0]const u8,
    fields: ?[*]const FieldDesc,
    field_count: usize,
    /// 1 for an object keyed by field name, 0 for a positional array.
    named: c_int,
};

// Spelled out rather than inferred: packStruct and packField call each other for nested
// structs, and Zig cannot infer an error set through that cycle.
const MapError = told.Error || told.Encoder.WriteError || error{
    OutOfRange,
    InvalidArgument,
};

fn fieldPtr(base: ?*anyopaque, offset: usize) [*]u8 {
    const raw: [*]u8 = @ptrCast(base.?);
    return raw + offset;
}

fn constFieldPtr(base: ?*const anyopaque, offset: usize) [*]const u8 {
    const raw: [*]const u8 = @ptrCast(base.?);
    return raw + offset;
}

fn readScalar(comptime T: type, ptr: [*]const u8) T {
    var out: T = undefined;
    @memcpy(std.mem.asBytes(&out), ptr[0..@sizeOf(T)]);
    return out;
}

fn writeScalar(comptime T: type, ptr: [*]u8, value: T) void {
    @memcpy(ptr[0..@sizeOf(T)], std.mem.asBytes(&value));
}

fn packScalar(enc: *told.Encoder, kind: FieldKind, ptr: [*]const u8) MapError!void {
    switch (kind) {
        .boolean => try enc.writeBool(readScalar(bool, ptr)),
        // Non-negative values take the unsigned encoding, matching the other languages so
        // the same struct produces the same bytes everywhere.
        .i8 => try packSigned(enc, readScalar(i8, ptr)),
        .i16 => try packSigned(enc, readScalar(i16, ptr)),
        .i32 => try packSigned(enc, readScalar(i32, ptr)),
        .i64 => try packSigned(enc, readScalar(i64, ptr)),
        .u8 => try enc.writeUint(readScalar(u8, ptr)),
        .u16 => try enc.writeUint(readScalar(u16, ptr)),
        .u32 => try enc.writeUint(readScalar(u32, ptr)),
        .u64 => try enc.writeUint(readScalar(u64, ptr)),
        .f32 => try enc.writeFloat32(readScalar(f32, ptr)),
        .f64 => try enc.writeFloat64(readScalar(f64, ptr)),
        else => return error.InvalidArgument,
    }
}

fn packSigned(enc: *told.Encoder, value: anytype) MapError!void {
    if (value >= 0) return enc.writeUint(@intCast(value));
    return enc.writeInt(@intCast(value));
}

fn unpackScalar(kind: FieldKind, frame: told.Frame, ptr: [*]u8) MapError!void {
    switch (kind) {
        .boolean => writeScalar(bool, ptr, try frame.asBool()),
        .i8 => writeScalar(i8, ptr, std.math.cast(i8, try frame.asInt()) orelse return error.OutOfRange),
        .i16 => writeScalar(i16, ptr, std.math.cast(i16, try frame.asInt()) orelse return error.OutOfRange),
        .i32 => writeScalar(i32, ptr, std.math.cast(i32, try frame.asInt()) orelse return error.OutOfRange),
        .i64 => writeScalar(i64, ptr, try frame.asInt()),
        .u8 => writeScalar(u8, ptr, std.math.cast(u8, try frame.asUint()) orelse return error.OutOfRange),
        .u16 => writeScalar(u16, ptr, std.math.cast(u16, try frame.asUint()) orelse return error.OutOfRange),
        .u32 => writeScalar(u32, ptr, std.math.cast(u32, try frame.asUint()) orelse return error.OutOfRange),
        .u64 => writeScalar(u64, ptr, try frame.asUint()),
        .f32 => writeScalar(f32, ptr, @floatCast(try frame.asFloat())),
        .f64 => writeScalar(f64, ptr, try frame.asFloat()),
        else => return error.InvalidArgument,
    }
}

fn packField(enc: *told.Encoder, field: *const FieldDesc, base: ?*const anyopaque) MapError!void {
    const ptr = constFieldPtr(base, field.offset);

    switch (field.kind) {
        .string => {
            const buf = ptr[0..field.size];
            const end = std.mem.indexOfScalar(u8, buf, 0) orelse field.size;
            try enc.writeString(buf[0..end]);
        },
        .bytes => {
            const used = readScalar(usize, constFieldPtr(base, field.length_offset));
            if (used > field.size) return error.InvalidArgument;
            try enc.writeBuffer(ptr[0..used]);
        },
        .nested => {
            const desc = field.nested orelse return error.InvalidArgument;
            try packStruct(enc, desc, @ptrCast(ptr));
        },
        .array => {
            const mark = enc.beginArray();
            var i: usize = 0;
            while (i < field.count) : (i += 1) {
                const element = ptr + i * field.element_size;
                if (field.element_kind == .nested) {
                    const desc = field.nested orelse return error.InvalidArgument;
                    try packStruct(enc, desc, @ptrCast(element));
                } else {
                    try packScalar(enc, field.element_kind, element);
                }
            }
            try enc.end(mark);
        },
        else => try packScalar(enc, field.kind, ptr),
    }
}

fn unpackField(field: *const FieldDesc, frame: told.Frame, base: ?*anyopaque) MapError!void {
    const ptr = fieldPtr(base, field.offset);

    switch (field.kind) {
        .string => {
            const text = try frame.asBytes();
            if (field.size == 0) return error.InvalidArgument;
            // Truncated rather than refused, and always NUL-terminated: a fixed buffer
            // cannot hold everything a producer might send, and a C caller will treat the
            // result as a string whatever we return.
            const copy = @min(text.len, field.size - 1);
            @memcpy(ptr[0..copy], text[0..copy]);
            ptr[copy] = 0;
        },
        .bytes => {
            const raw = try frame.asBytes();
            const copy = @min(raw.len, field.size);
            @memcpy(ptr[0..copy], raw[0..copy]);
            writeScalar(usize, fieldPtr(base, field.length_offset), copy);
        },
        .nested => {
            const desc = field.nested orelse return error.InvalidArgument;
            try unpackStruct(desc, frame, @ptrCast(ptr));
        },
        .array => {
            var it = try frame.arrayIterator();
            var i: usize = 0;
            while (try it.next()) |item| : (i += 1) {
                if (i >= field.count) break; // extra elements ignored, as extra keys are
                const element = ptr + i * field.element_size;
                if (field.element_kind == .nested) {
                    const desc = field.nested orelse return error.InvalidArgument;
                    try unpackStruct(desc, item, @ptrCast(element));
                } else {
                    try unpackScalar(field.element_kind, item, element);
                }
            }
        },
        else => try unpackScalar(field.kind, frame, ptr),
    }
}

fn packStruct(enc: *told.Encoder, desc: *const StructDesc, value: ?*const anyopaque) MapError!void {
    const fields = desc.fields orelse return error.InvalidArgument;

    if (desc.named != 0) {
        const mark = enc.beginObject();
        var i: usize = 0;
        while (i < desc.field_count) : (i += 1) {
            const field = &fields[i];
            const key = field.key orelse return error.InvalidArgument;
            try enc.writeKey(std.mem.span(key));
            try packField(enc, field, value);
        }
        try enc.end(mark);
        return;
    }

    const mark = enc.beginArray();
    var i: usize = 0;
    while (i < desc.field_count) : (i += 1) {
        try packField(enc, &fields[i], value);
    }
    try enc.end(mark);
}

fn unpackStruct(desc: *const StructDesc, frame: told.Frame, out: ?*anyopaque) MapError!void {
    const fields = desc.fields orelse return error.InvalidArgument;

    if (desc.named != 0) {
        var it = try frame.objectIterator();
        while (try it.next()) |pair| {
            var i: usize = 0;
            while (i < desc.field_count) : (i += 1) {
                const field = &fields[i];
                const key = field.key orelse continue;
                if (std.mem.eql(u8, pair.key, std.mem.span(key))) {
                    try unpackField(field, pair.value, out);
                    break;
                }
            }
        }
        return;
    }

    var it = try frame.arrayIterator();
    var i: usize = 0;
    while (try it.next()) |item| : (i += 1) {
        if (i >= desc.field_count) break;
        try unpackField(&fields[i], item, out);
    }
}

/// Append `value`, described by `desc`, to the encoder.
export fn told_pack_struct(
    handle: ?*Encoder,
    desc: ?*const StructDesc,
    value: ?*const anyopaque,
) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const d = desc orelse return .invalid_argument;
    if (value == null) return .invalid_argument;
    packStruct(&enc, d, value) catch |err| return statusFor(err);
    return .ok;
}

/// Fill `out`, described by `desc`, from a decoded frame.
export fn told_unpack_struct(
    frame: ?*const Frame,
    desc: ?*const StructDesc,
    out: ?*anyopaque,
) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const d = desc orelse return .invalid_argument;
    if (out == null) return .invalid_argument;
    unpackStruct(d, fromCFrame(f), out) catch |err| return statusFor(err);
    return .ok;
}

/// Decode `bytes` and fill `out` in one call, for the common case.
export fn told_unpack_struct_bytes(
    bytes: ?[*]const u8,
    len: usize,
    desc: ?*const StructDesc,
    out: ?*anyopaque,
) callconv(.c) Status {
    const raw = bytes orelse return .invalid_argument;
    const d = desc orelse return .invalid_argument;
    if (out == null) return .invalid_argument;
    const decoded = told.decode(raw[0..len]) catch |err| return statusFor(err);
    unpackStruct(d, decoded, out) catch |err| return statusFor(err);
    return .ok;
}
