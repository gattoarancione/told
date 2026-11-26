const std = @import("std");
const Build = std.Build;

pub fn build(b: *Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const c_flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" };
    const cpp_flags: []const []const u8 = &.{ "-std=c++20", "-Wall", "-Wextra", "-Werror" };

    // The codec itself, for Zig consumers:
    //
    //     const told = b.dependency("told", .{ .target = target, .optimize = optimize });
    //     exe_mod.addImport("told", told.module("told"));
    //
    // It needs no libc and no allocator beyond the one the caller passes to `Encoder`.
    const told_mod = b.addModule("told", .{
        .root_source_file = b.path("src/told.zig"),
        .target = target,
        .optimize = optimize,
    });

    // The C frame layout and nothing else. A downstream library that hands TOLD frames to
    // its own C callers imports this instead of told_c.zig, so the two archives do not both
    // carry the told_* exports. See src/told_abi.zig.
    _ = b.addModule("told_abi", .{
        .root_source_file = b.path("src/told_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    // The C ABI, built both ways: static for C/C++ callers that link, shared for anything
    // that loads it at runtime (ctypes/cffi will want the latter).
    const told_c_mod = b.createModule(.{
        .root_source_file = b.path("src/told_c.zig"),
        .target = target,
        .optimize = optimize,
    });

    var told_static: *Build.Step.Compile = undefined;
    for ([_]std.builtin.LinkMode{ .static, .dynamic }) |linkage| {
        const lib = b.addLibrary(.{
            .linkage = linkage,
            .name = "told",
            .root_module = told_c_mod,
        });
        lib.installHeader(b.path("include/told.h"), "told.h");
        lib.installHeader(b.path("include/told.hpp"), "told.hpp");
        b.installArtifact(lib);
        if (linkage == .static) told_static = lib;
    }

    const test_step = b.step("test", "Run the Zig tests and the C/C++ checks");

    const unit_tests = b.addTest(.{ .root_module = told_mod });
    test_step.dependOn(&b.addRunArtifact(unit_tests).step);

    // The C and C++ programs are the only thing that checks the hand-written headers
    // against the exports -- a mismatch there is invisible from Zig -- so every one of them
    // runs as part of `test`. The told-map pair also decode src/testdata/interop.tld, which
    // pins all languages to one golden frame. They read it by a relative path, hence the cwd.
    const Example = struct { name: []const u8, src: []const u8, cpp: bool, desc: []const u8 };
    const examples = [_]Example{
        .{ .name = "told-demo-c", .src = "examples/told_demo.c", .cpp = false, .desc = "Build and run the C example against the C ABI" },
        .{ .name = "told-demo-cpp", .src = "examples/told_demo.cpp", .cpp = true, .desc = "Build and run the C++ example against the wrapper" },
        .{ .name = "told-map-c", .src = "examples/told_map.c", .cpp = false, .desc = "Build and run the C struct-mapping example" },
        .{ .name = "told-map-cpp", .src = "examples/told_map.cpp", .cpp = true, .desc = "Build and run the C++ struct-mapping example" },
    };

    for (examples) |ex| {
        const mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .link_libcpp = ex.cpp,
        });
        mod.addCSourceFile(.{ .file = b.path(ex.src), .flags = if (ex.cpp) cpp_flags else c_flags });
        mod.addIncludePath(b.path("include"));
        mod.linkLibrary(told_static);

        const exe = b.addExecutable(.{ .name = ex.name, .root_module = mod });
        b.installArtifact(exe);

        const run = b.addRunArtifact(exe);
        run.setCwd(b.path("."));
        b.step(ex.name, ex.desc).dependOn(&run.step);
        test_step.dependOn(&run.step);
    }
}
