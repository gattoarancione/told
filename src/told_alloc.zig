// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

//! The allocator the TOLD C ABI uses, chosen by build mode.
//!
//! This is the only allocator in the C ABI, and no allocation crosses the line: the only
//! thing that allocates is the encoder's output buffer, and `told_encoder_create` and
//! `told_encoder_destroy` are both on this side of it; `told_encoder_bytes` hands out a
//! borrowed pointer the caller never frees. So a program that also links another Zig
//! library with its own allocator (qnnt's `libqnnt_c.a` does) never frees against the
//! wrong one.
//!
//! The choice itself: small, short-lived blocks dominate, and `page_allocator` rounds each
//! to a page, measured at ~1.1 us per alloc/free pair against 2-3 ns here. Debug and ReleaseSafe get `DebugAllocator`, which
//! is thread-safe in a multi-threaded build and tracks allocations. Faster modes get
//! `smp_allocator`, a thread-safe process singleton.
//!
//! A caller-supplied allocator would end the question, but it is an ABI break for every
//! existing C, C++ and Python consumer, and a two-function C vtable would cost the output
//! buffer its in-place growth. If an embedded target wants one, the additive shape that
//! fits this codec is a caller-supplied *buffer* rather than an allocator.

const std = @import("std");

var debug_allocator: std.heap.DebugAllocator(.{}) = .init;

pub const allocator: std.mem.Allocator = switch (@import("builtin").mode) {
    .Debug, .ReleaseSafe => debug_allocator.allocator(),
    .ReleaseFast, .ReleaseSmall => std.heap.smp_allocator,
};
