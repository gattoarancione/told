// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

//! Encoder benchmark, aimed at what container nesting costs: `zig build bench`.
//!
//! Each shape is encoded into one reused list, so allocation drops out after the first
//! round and what is left is the encoding itself. A shape runs for at least 300 ms and
//! the best of five such runs is reported, which keeps scheduler noise out of the figures.

const std = @import("std");
const told = @import("told");

const Shape = struct {
    name: []const u8,
    encode: *const fn (e: *told.Encoder) told.Encoder.WriteError!void,
};

const shapes = [_]Shape{
    .{ .name = "small object, 4 fields", .encode = small },
    .{ .name = "1000 structs in an array", .encode = telemetry },
    .{ .name = "10000 tiny arrays", .encode = tinyArrays },
    .{ .name = "1 MiB buffer, 3 deep", .encode = blob },
    .{ .name = "4 KiB string, 64 deep", .encode = deep },
};

var blob_bytes: [1 << 20]u8 = @splat(0xA5);
var text_bytes: [4096]u8 = @splat('x');

fn small(e: *told.Encoder) told.Encoder.WriteError!void {
    const obj = try e.beginObject();
    try e.writeKey("sensor");
    try e.writeString("t1");
    try e.writeKey("seq");
    try e.writeUint(4242);
    try e.writeKey("celsius");
    try e.writeFloat32(21.5);
    try e.writeKey("ok");
    try e.writeBool(true);
    try e.end(obj);
}

fn telemetry(e: *told.Encoder) told.Encoder.WriteError!void {
    const obj = try e.beginObject();
    try e.writeKey("id");
    try e.writeUint(7);
    try e.writeKey("readings");
    const arr = try e.beginArray();
    for (0..1000) |i| {
        const r = try e.beginObject();
        try e.writeKey("channel");
        try e.writeUint(i % 16);
        try e.writeKey("value");
        try e.writeFloat64(@as(f64, @floatFromInt(i)) * 0.1);
        try e.writeKey("valid");
        try e.writeBool(i % 7 != 0);
        try e.end(r);
    }
    try e.end(arr);
    try e.end(obj);
}

fn tinyArrays(e: *told.Encoder) told.Encoder.WriteError!void {
    const outer = try e.beginArray();
    for (0..10_000) |i| {
        const a = try e.beginArray();
        try e.writeUint(i & 15);
        try e.writeUint(2);
        try e.end(a);
    }
    try e.end(outer);
}

fn blob(e: *told.Encoder) told.Encoder.WriteError!void {
    const a = try e.beginObject();
    try e.writeKey("a");
    const b = try e.beginObject();
    try e.writeKey("b");
    const c = try e.beginObject();
    try e.writeKey("c");
    try e.writeBuffer(&blob_bytes);
    try e.end(c);
    try e.end(b);
    try e.end(a);
}

fn deep(e: *told.Encoder) told.Encoder.WriteError!void {
    var marks: [64]told.Encoder.Mark = undefined;
    for (&marks) |*m| m.* = try e.beginArray();
    try e.writeString(&text_bytes);
    var i: usize = marks.len;
    while (i > 0) {
        i -= 1;
        try e.end(marks[i]);
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const gpa = init.gpa;

    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);

    var stdout_buf: [1024]u8 = undefined;
    var stdout = std.Io.File.stdout().writer(io, &stdout_buf);
    const w = &stdout.interface;

    try w.print("{s:<26} {s:>9} {s:>14} {s:>14} {s:>9}\n", .{ "shape", "bytes", "fresh ns", "reused ns", "MB/s" });
    for (shapes) |shape| {
        const fresh = try measure(io, gpa, &out, shape, .fresh);
        const reused = try measure(io, gpa, &out, shape, .reused);
        // A benchmark of wrong output is worthless: every shape must still validate.
        _ = try told.validate(out.items, 128);
        const mbps = @as(f64, @floatFromInt(out.items.len)) / reused * 1000.0;
        try w.print("{s:<26} {d:>9} {d:>14.0} {d:>14.0} {d:>9.0}\n", .{ shape.name, out.items.len, fresh, reused, mbps });
        try w.flush();
    }
}

/// `fresh` builds an encoder per frame, so every frame starts from the default header
/// guesses; `reused` keeps one, as a long-lived publisher would.
fn measure(io: std.Io, gpa: std.mem.Allocator, out: *std.ArrayList(u8), shape: Shape, mode: enum { fresh, reused }) !f64 {
    var best: f64 = std.math.inf(f64);
    var e = told.Encoder.init(gpa, out);
    for (0..5) |_| {
        var rounds: u64 = 0;
        const start = std.Io.Timestamp.now(io, .awake);
        var elapsed: i96 = 0;
        while (elapsed < 300 * std.time.ns_per_ms) {
            out.clearRetainingCapacity();
            if (mode == .fresh) e = told.Encoder.init(gpa, out);
            try shape.encode(&e);
            std.mem.doNotOptimizeAway(out.items.ptr);
            rounds += 1;
            elapsed = start.durationTo(std.Io.Timestamp.now(io, .awake)).nanoseconds;
        }
        best = @min(best, @as(f64, @floatFromInt(elapsed)) / @as(f64, @floatFromInt(rounds)));
    }
    return best;
}
