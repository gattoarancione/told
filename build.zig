// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Claudio Russo (gattoarancione)

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
    _ = b.addModule("told", .{
        .root_source_file = b.path("src/told.zig"),
        .target = target,
        .optimize = optimize,
    });

    // ReleaseFast whatever -Doptimize says: a benchmark of a debug build measures the checks.
    const bench_mod = b.createModule(.{
        .root_source_file = b.path("bench/encode.zig"),
        .target = target,
        .optimize = .ReleaseFast,
    });
    bench_mod.addImport("told", b.createModule(.{
        .root_source_file = b.path("src/told.zig"),
        .target = target,
        .optimize = .ReleaseFast,
    }));
    const bench = b.addExecutable(.{ .name = "bench-encode", .root_module = bench_mod });
    b.step("bench", "Benchmark the encoder (ReleaseFast)").dependOn(&b.addRunArtifact(bench).step);

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
    var told_dynamic: *Build.Step.Compile = undefined;
    for ([_]std.builtin.LinkMode{ .static, .dynamic }) |linkage| {
        const lib = b.addLibrary(.{
            .linkage = linkage,
            .name = "told",
            .root_module = told_c_mod,
        });
        lib.installHeader(b.path("include/told.h"), "told.h");
        lib.installHeader(b.path("include/told.hpp"), "told.hpp");
        b.installArtifact(lib);
        if (linkage == .static) told_static = lib else told_dynamic = lib;
    }

    const test_step = b.step("test", "Run the Zig tests and the C/C++ checks");

    // The Zig tests get a module of their own rather than `told_mod`, so the fuzzer's
    // options import does not leak to consumers of the `told` module. Every `zig build test`
    // runs 10K mutational fuzz inputs from a fixed seed; `zig build fuzz` runs more.
    const unit_tests = b.addTest(.{ .root_module = toldTestModule(b, target, optimize, 10_000, 0) });
    test_step.dependOn(&b.addRunArtifact(unit_tests).step);

    // ReleaseSafe whatever -Doptimize says: the safety checks are what turn a bad input into
    // a caught panic, and the speed is what makes millions of inputs practical.
    const fuzz_iterations = b.option(u64, "fuzz-iterations", "Inputs for `zig build fuzz` (default 1M)") orelse 1_000_000;
    const fuzz_seed = b.option(u64, "fuzz-seed", "Seed for `zig build fuzz`; vary it to explore (default 1)") orelse 1;
    const fuzz_tests = b.addTest(.{
        .root_module = toldTestModule(b, target, .ReleaseSafe, fuzz_iterations, fuzz_seed),
        .filters = &.{"mutational fuzz"},
    });
    b.step("fuzz", "Run the mutational fuzzer on decode and validate").dependOn(&b.addRunArtifact(fuzz_tests).step);

    // The C-level fuzz target for told_read_frame, driven by tests/fuzz_main.c because no
    // libFuzzer runtime ships with Zig. It links a ReleaseSafe libtold of its own, so the
    // library's safety checks stay on whatever -Doptimize says, and the C side keeps the
    // UBSan checks Zig enables for C in safe modes.
    const fuzz_lib = b.addLibrary(.{
        .linkage = .static,
        .name = "told-fuzz",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/told_c.zig"),
            .target = target,
            .optimize = .ReleaseSafe,
        }),
    });
    const fuzz_c_mod = b.createModule(.{ .target = target, .optimize = .ReleaseSafe, .link_libc = true });
    fuzz_c_mod.addCSourceFiles(.{ .files = &.{ "tests/fuzz_read_frame.c", "tests/fuzz_main.c" }, .flags = c_flags });
    fuzz_c_mod.addIncludePath(b.path("include"));
    fuzz_c_mod.linkLibrary(fuzz_lib);
    const fuzz_c = b.addExecutable(.{ .name = "fuzz-read-frame", .root_module = fuzz_c_mod });

    // Every `zig build test` reads 10K streams from a fixed seed.
    const fuzz_c_quick = b.addRunArtifact(fuzz_c);
    fuzz_c_quick.addArgs(&.{ "--iterations", "10000", "--seed", "0" });
    test_step.dependOn(&fuzz_c_quick.step);

    // `zig build fuzz-c` runs as many as asked; `zig build fuzz-c -- FILE` replays one.
    // Crash inputs are saved in the repository root, where it runs.
    const fuzz_c_run = b.addRunArtifact(fuzz_c);
    fuzz_c_run.setCwd(b.path("."));
    if (b.args) |files| {
        fuzz_c_run.addArgs(files);
    } else {
        fuzz_c_run.addArgs(&.{ "--iterations", b.fmt("{d}", .{fuzz_iterations}), "--seed", b.fmt("{d}", .{fuzz_seed}) });
    }
    b.step("fuzz-c", "Fuzz told_read_frame through the C ABI").dependOn(&fuzz_c_run.step);

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
        .{ .name = "test-c", .src = "tests/test_c.c", .cpp = false, .desc = "Run the C unit tests" },
        .{ .name = "test-cpp", .src = "tests/test_cpp.cpp", .cpp = true, .desc = "Run the C++ unit tests" },
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

    // The Python package, checked against the shared library just built. The path is
    // passed as an argument so the script never has to guess which build it is testing.
    // It decodes the golden frame too, so it belongs in `test` with the C and C++ checks.
    const python_test = b.addSystemCommand(&.{ "python3", "python/test_told.py" });
    python_test.addFileArg(told_dynamic.getEmittedBin());
    b.step("python-test", "Run the Python package against the built library").dependOn(&python_test.step);
    test_step.dependOn(&python_test.step);

    // Bundle the shared library into the package, then build a wheel from it. The copy
    // lands in the source tree because that is where setuptools looks for package data;
    // `python/told/_lib/` is generated, not authored.
    const bundle = b.addUpdateSourceFiles();
    bundle.addCopyFileToSource(told_dynamic.getEmittedBin(), b.fmt("python/told/_lib/{s}", .{
        told_dynamic.out_filename,
    }));

    const wheel = b.addSystemCommand(&.{
        "python3",     "-m",            "pip",      "wheel", "--no-deps", "--quiet",
        "--wheel-dir", "zig-out/wheel", "./python",
    });
    wheel.step.dependOn(&bundle.step);
    b.step("wheel", "Bundle the shared library and build a Python wheel").dependOn(&wheel.step);

    // Installing the wheel into a throwaway venv and running the test from it is the only
    // check that the packaging works: it proves the library is inside the wheel and found
    // without TOLD_LIBRARY or a source checkout to fall back on.
    const wheel_test = b.addSystemCommand(&.{ "python3", "python/verify_wheel.py" });
    wheel_test.step.dependOn(&wheel.step);
    b.step("wheel-test", "Install the wheel in a venv and run the test from it").dependOn(&wheel_test.step);
}

fn toldTestModule(
    b: *Build,
    target: Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    fuzz_iterations: u64,
    fuzz_seed: u64,
) *Build.Module {
    const options = b.addOptions();
    options.addOption(u64, "iterations", fuzz_iterations);
    options.addOption(u64, "seed", fuzz_seed);
    const mod = b.createModule(.{
        .root_source_file = b.path("src/told.zig"),
        .target = target,
        .optimize = optimize,
    });
    mod.addOptions("fuzz_options", options);
    return mod;
}
