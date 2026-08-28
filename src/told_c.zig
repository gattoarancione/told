//! C ABI for the TLD codec. Every symbol here is `export ... callconv(.c)` and every
//! signature is expressible in C: slices become (pointer, length) pairs, error unions
//! become status codes, and the one stateful type — the encoder — becomes an opaque
//! handle. The declarations must stay in step with `include/qnnt_tld.h`, which is
//! hand-written because Zig no longer emits C headers.
//!
//! TLD is the natural first thing to expose: `tld.zig` has no globals, no threads and no
//! `Io` outside `readFrame`, so this layer needs no lifecycle of its own.
//!
//! Decoding stays zero-copy across the boundary. A frame's `data`/`raw` pointers borrow
//! the caller's buffer, so they are valid exactly as long as that buffer is — the C side
//! owns the bytes throughout and nothing is copied in either direction.

const std = @import("std");
const tld = @import("tld.zig");
const allocator = @import("alloc.zig").allocator;

/// Mirrors qnnt_tld_status in the header. Values are part of the ABI: append, never
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
    /// `qnnt_tld_frame_size` only: the prefix is too short to tell yet, read more bytes.
    incomplete = 11,
    /// A null pointer where one was required.
    invalid_argument = 12,
    unexpected = 13,
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
        else => .unexpected,
    };
}

/// Mirrors qnnt_tld_frame. Flat by nature: the zero-copy decoder already reduces a frame
/// to a type, a mod and two borrowed spans, so no opaque handle is needed here.
const Frame = extern struct {
    type: u8,
    mod: u8,
    /// The extension type code, or -1 when this is not an EXTENDED frame.
    ext_code: i16,
    data: [*]const u8,
    data_len: usize,
    raw: [*]const u8,
    raw_len: usize,
};

fn toCFrame(f: tld.Frame) Frame {
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

fn fromCFrame(f: *const Frame) tld.Frame {
    return .{
        .type = @enumFromInt(f.type),
        .mod = @truncate(f.mod),
        .prefix_len = f.raw_len - f.data_len,
        .data = f.data[0..f.data_len],
        .raw = f.raw[0..f.raw_len],
        .ext_code = if (f.ext_code < 0) null else @intCast(f.ext_code),
    };
}

/// Mirrors qnnt_tld_iter: a cursor over the not-yet-consumed bytes of a container.
const Iter = extern struct {
    rest: [*]const u8,
    rest_len: usize,
};

// ---------------------------------------------------------------- decoding

export fn qnnt_tld_decode(bytes: ?[*]const u8, len: usize, out: ?*Frame) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const frame = tld.decode(src[0..len]) catch |err| return statusFor(err);
    dst.* = toCFrame(frame);
    return .ok;
}

/// Total size of the frame starting at `bytes`, or `incomplete` when the prefix is too
/// short to say. This is what lets a C reader buffer exactly one frame off a socket
/// without parsing it: at most 9 bytes are needed to decide.
export fn qnnt_tld_frame_size(bytes: ?[*]const u8, len: usize, out: ?*usize) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const size = tld.frameSize(src[0..len]) catch |err| return statusFor(err);
    dst.* = size orelse return .incomplete;
    return .ok;
}

/// Walk a whole frame, checking nested frames, UTF-8 in strings and object shape.
/// `qnnt_tld_decode` only validates the outermost frame, so use this on untrusted input.
export fn qnnt_tld_validate(bytes: ?[*]const u8, len: usize, max_depth: usize) callconv(.c) Status {
    const src = bytes orelse return .invalid_argument;
    _ = tld.validate(src[0..len], max_depth) catch |err| return statusFor(err);
    return .ok;
}

// ---------------------------------------------------------------- accessors

export fn qnnt_tld_as_bool(frame: ?*const Frame, out: ?*bool) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asBool() catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_as_i64(frame: ?*const Frame, out: ?*i64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asInt() catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_as_u64(frame: ?*const Frame, out: ?*u64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asUint() catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_as_f64(frame: ?*const Frame, out: ?*f64) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    dst.* = fromCFrame(f).asFloat() catch |err| return statusFor(err);
    return .ok;
}

/// STRING payload, UTF-8 validated. Borrows the caller's buffer; nothing is copied.
export fn qnnt_tld_as_string(frame: ?*const Frame, ptr: ?*[*]const u8, len: ?*usize) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const p = ptr orelse return .invalid_argument;
    const l = len orelse return .invalid_argument;

    const text = fromCFrame(f).asString() catch |err| return statusFor(err);
    p.* = text.ptr;
    l.* = text.len;
    return .ok;
}

/// BUFFER payload: opaque bytes, no encoding contract.
export fn qnnt_tld_as_buffer(frame: ?*const Frame, ptr: ?*[*]const u8, len: ?*usize) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const p = ptr orelse return .invalid_argument;
    const l = len orelse return .invalid_argument;

    const bytes = fromCFrame(f).asBuffer() catch |err| return statusFor(err);
    p.* = bytes.ptr;
    l.* = bytes.len;
    return .ok;
}

// ---------------------------------------------------------------- iteration

export fn qnnt_tld_array_iter(frame: ?*const Frame, out: ?*Iter) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const it = fromCFrame(f).arrayIterator() catch |err| return statusFor(err);
    dst.* = .{ .rest = it.rest.ptr, .rest_len = it.rest.len };
    return .ok;
}

export fn qnnt_tld_object_iter(frame: ?*const Frame, out: ?*Iter) callconv(.c) Status {
    const f = frame orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;

    const it = fromCFrame(f).objectIterator() catch |err| return statusFor(err);
    dst.* = .{ .rest = it.rest.ptr, .rest_len = it.rest.len };
    return .ok;
}

/// Next array element. `*have` is false at the end, which is the loop condition; the
/// status is reserved for genuinely malformed input.
export fn qnnt_tld_array_next(iter: ?*Iter, out: ?*Frame, have: ?*bool) callconv(.c) Status {
    const cursor = iter orelse return .invalid_argument;
    const dst = out orelse return .invalid_argument;
    const flag = have orelse return .invalid_argument;

    var zig_it: tld.Iterator = .{ .rest = cursor.rest[0..cursor.rest_len] };
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
export fn qnnt_tld_object_next(
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

    var zig_it: tld.ObjectIterator = .{ .rest = cursor.rest[0..cursor.rest_len] };
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
/// its buffer; the caller borrows the bytes via `qnnt_tld_encoder_bytes` and must copy
/// them before the next write, since appending may reallocate.
const Encoder = struct {
    list: std.ArrayList(u8),
};

export fn qnnt_tld_encoder_create() callconv(.c) ?*Encoder {
    const enc = allocator.create(Encoder) catch return null;
    enc.* = .{ .list = .empty };
    return enc;
}

export fn qnnt_tld_encoder_destroy(handle: ?*Encoder) callconv(.c) void {
    const enc = handle orelse return; // free(NULL) is a no-op, so this one is too
    enc.list.deinit(allocator);
    allocator.destroy(enc);
}

/// Drop the contents, keeping the capacity — for reusing one encoder per message.
export fn qnnt_tld_encoder_reset(handle: ?*Encoder) callconv(.c) void {
    const enc = handle orelse return;
    enc.list.clearRetainingCapacity();
}

export fn qnnt_tld_encoder_bytes(handle: ?*const Encoder, len: ?*usize) callconv(.c) ?[*]const u8 {
    const enc = handle orelse return null;
    if (len) |l| l.* = enc.list.items.len;
    return enc.list.items.ptr;
}

fn writer(handle: ?*Encoder) ?tld.Encoder {
    const enc = handle orelse return null;
    return tld.Encoder.init(allocator, &enc.list);
}

export fn qnnt_tld_write_null(handle: ?*Encoder) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeNull() catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_bool(handle: ?*Encoder, value: bool) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeBool(value) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_u64(handle: ?*Encoder, value: u64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeUint(value) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_i64(handle: ?*Encoder, value: i64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeInt(value) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_f32(handle: ?*Encoder, value: f32) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeFloat32(value) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_f64(handle: ?*Encoder, value: f64) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.writeFloat64(value) catch |err| return statusFor(err);
    return .ok;
}

/// Rejects invalid UTF-8 rather than emitting a STRING a decoder is entitled to refuse.
export fn qnnt_tld_write_string(handle: ?*Encoder, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = bytes orelse return .invalid_argument;
    enc.writeString(src[0..len]) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_buffer(handle: ?*Encoder, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = bytes orelse return .invalid_argument;
    enc.writeBuffer(src[0..len]) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_write_extended(handle: ?*Encoder, code: u8, bytes: ?[*]const u8, len: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const src = bytes orelse return .invalid_argument;
    enc.writeExtended(code, src[0..len]) catch |err| return statusFor(err);
    return .ok;
}

// A container's length is only known once its children are written, so the header is
// inserted at the end. The mark is just the offset where the container started — a plain
// size_t in C, with separate array/object closers so the type cannot be lost.
export fn qnnt_tld_begin_array(handle: ?*Encoder, mark: ?*usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const m = mark orelse return .invalid_argument;
    m.* = enc.beginArray().start;
    return .ok;
}

export fn qnnt_tld_begin_object(handle: ?*Encoder, mark: ?*usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    const m = mark orelse return .invalid_argument;
    m.* = enc.beginObject().start;
    return .ok;
}

export fn qnnt_tld_end_array(handle: ?*Encoder, mark: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.end(.{ .start = mark, .type = .array }) catch |err| return statusFor(err);
    return .ok;
}

export fn qnnt_tld_end_object(handle: ?*Encoder, mark: usize) callconv(.c) Status {
    var enc = writer(handle) orelse return .invalid_argument;
    enc.end(.{ .start = mark, .type = .object }) catch |err| return statusFor(err);
    return .ok;
}

// ---------------------------------------------------------------- diagnostics

export fn qnnt_tld_status_name(status: Status) callconv(.c) [*:0]const u8 {
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
    };
}

export fn qnnt_tld_version() callconv(.c) [*:0]const u8 {
    return tld.version;
}
