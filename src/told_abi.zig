//! The C ABI shape of a decoded TLD frame, declared ONCE.
//!
//! This file exports nothing. That is the whole point: `tld_c.zig` and `qnnt_c.zig` are the
//! roots of two SEPARATE libraries, `qnnt_tld` and `qnnt_c`, and a program may link both.
//! If either imported the other to share this type, every `export fn` in the imported file
//! would be compiled into both archives and the link would fail on duplicate symbols. A
//! module with no exports can be imported by both safely.
//!
//! Before this, the layout existed three times -- here in Zig for the TLD library, again in
//! Zig for the node library's frame callback, and in C in `include/qnnt_tld.h` -- held
//! together by a comment reading "must match". Nothing checked it. A field reordered in one
//! place would have been a callback reading a length as a pointer, at runtime, silently.
//!
//! Two copies are now unavoidable rather than three: Zig's and C's, because each language
//! has to declare it. Both carry assertions, so a drift fails to compile on whichever side
//! moved rather than being discovered by a caller.

const std = @import("std");

/// Mirrors `qnnt_tld_frame` in include/qnnt_tld.h. Flat rather than opaque because the
/// zero-copy decoder already reduces a frame to a type, a mod and two borrowed spans.
pub const Frame = extern struct {
    type: u8,
    mod: u8,
    /// The extension type code, or -1 when this is not an EXTENDED frame.
    ext_code: i16,
    data: [*]const u8,
    data_len: usize,
    raw: [*]const u8,
    raw_len: usize,
};

// Expressed against pointer width rather than as literals, so the same assertions hold on
// a 32-bit target: there the whole struct is 20 bytes, not 40, and hard-coding either
// would have made this file wrong on half the machines qnnt cross-compiles for.
//
// The mirror of these lives in include/qnnt_tld.h as static assertions on the C struct.
// Change one and the other must be changed with it -- which is now enforced by both
// failing to compile rather than by anybody remembering.
comptime {
    const ptr = @sizeOf(usize);

    std.debug.assert(@offsetOf(Frame, "type") == 0);
    std.debug.assert(@offsetOf(Frame, "mod") == 1);
    std.debug.assert(@offsetOf(Frame, "ext_code") == 2);
    // The first pointer lands at pointer alignment, which on both widths is exactly one
    // pointer in -- the three scalars ahead of it are 4 bytes and never more.
    std.debug.assert(@offsetOf(Frame, "data") == ptr);
    std.debug.assert(@offsetOf(Frame, "data_len") == 2 * ptr);
    std.debug.assert(@offsetOf(Frame, "raw") == 3 * ptr);
    std.debug.assert(@offsetOf(Frame, "raw_len") == 4 * ptr);
    std.debug.assert(@sizeOf(Frame) == 5 * ptr);

    // A trailing pad would mean the C struct and this one disagree about the size even
    // with every field at the same offset.
    std.debug.assert(@alignOf(Frame) == ptr);
}
