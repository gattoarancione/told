//! The allocator the TLD C ABI uses, chosen by build mode.
//!
//! A deliberate copy of `alloc.zig`, not a shared import. The codec is meant to carve out
//! into a project of its own, and the file set that goes with it — `tld.zig`, `tld_c.zig`,
//! `tld_abi.zig` and the two headers — imports nothing from qnnt but this. Owning the
//! choice here is what makes that set self-contained.
//!
//! The copy is safe because no allocation crosses between the two. The only thing here
//! that allocates is the encoder's output buffer, and `qnnt_tld_encoder_create` and
//! `qnnt_tld_encoder_destroy` are both on this side of the line; `qnnt_tld_encoder_bytes`
//! hands out a borrowed pointer the caller never frees. So a program holding both
//! allocators — `libqnnt_c.a` does, since it pulls the codec's exports in — never frees
//! against the wrong one.
//!
//! What it costs is drift: change the policy in `alloc.zig` and this file does not follow.
//! Keep them in step deliberately, or let them diverge deliberately, but check both.
//!
//! The rationale for the choice itself is the one `alloc.zig` gives: small, short-lived
//! blocks dominate, and `page_allocator` rounds each to a page, measured at ~1.1 us per
//! alloc/free pair against 2-3 ns here. Debug and ReleaseSafe get `DebugAllocator`, which
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
