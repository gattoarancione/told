"""Locating and declaring the shared library. Nothing public lives here but ToldError."""

from __future__ import annotations

import ctypes as C
import os
import sys
from ctypes.util import find_library
from pathlib import Path

_SUFFIX = {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")
_NAME = "told"


class ToldError(Exception):
    """A TOLD encode or decode failed. Carries the numeric status alongside the message."""

    def __init__(self, status: int, name: str, context: str = "") -> None:
        super().__init__(f"{context}: {name}" if context else name)
        self.status = status
        self.status_name = name


def _candidates() -> list[Path]:
    """Where to look, most explicit first."""
    found: list[Path] = []

    override = os.environ.get("TOLD_LIBRARY")
    if override:
        found.append(Path(override))

    here = Path(__file__).resolve().parent

    # A checkout's own build comes first, ahead of the bundled copy. The bundled file is a
    # build artifact that `zig build wheel` drops in, and it goes stale silently: once it
    # exists, it would shadow the freshly compiled library and calls would fail with dlsym
    # errors for symbols added since. An installed wheel has no zig-out above it, so it
    # still loads its bundled copy -- only a checkout is affected, and there the live build
    # is what you meant.
    found.append(here.parents[1] / "zig-out" / "lib" / f"lib{_NAME}{_SUFFIX}")

    # Bundled inside the package, which is how a wheel ships it.
    found.append(here / "_lib" / f"lib{_NAME}{_SUFFIX}")

    system = find_library(_NAME)
    if system:
        found.append(Path(system))

    return found


def load() -> C.CDLL:
    tried = _candidates()
    for path in tried:
        if path.exists() or not path.is_absolute():
            try:
                return C.CDLL(str(path))
            except OSError:
                continue
    raise ToldError(
        -1,
        "library not found",
        "looked in " + ", ".join(str(p) for p in tried) + " (set TOLD_LIBRARY to override)",
    )


class Frame(C.Structure):
    """Mirrors told_frame in told.h, and src/told_abi.zig."""

    _fields_ = [
        ("type", C.c_uint8),
        ("mod", C.c_uint8),
        ("ext_code", C.c_int16),
        ("data", C.POINTER(C.c_uint8)),
        ("data_len", C.c_size_t),
        ("raw", C.POINTER(C.c_uint8)),
        ("raw_len", C.c_size_t),
    ]


class Iter(C.Structure):
    """Mirrors told_iter in told.h."""

    _fields_ = [("rest", C.POINTER(C.c_uint8)), ("rest_len", C.c_size_t)]


# told_read_fn: ptrdiff_t (*)(void *ctx, uint8_t *dst, size_t len)
READ_FN = C.CFUNCTYPE(C.c_ssize_t, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t)


def declare(lib: C.CDLL) -> C.CDLL:
    """Pin argtypes/restypes so ctypes converts rather than guessing.

    Without this, a Python int passed where the ABI wants uint64 is truncated to 32 bits
    on some platforms, silently.
    """
    lib.told_version.argtypes, lib.told_version.restype = [], C.c_char_p
    lib.told_status_name.argtypes, lib.told_status_name.restype = [C.c_int], C.c_char_p

    lib.told_encoder_create.argtypes, lib.told_encoder_create.restype = [], C.c_void_p
    lib.told_encoder_destroy.argtypes = [C.c_void_p]
    lib.told_encoder_reset.argtypes = [C.c_void_p]
    lib.told_encoder_bytes.argtypes = [C.c_void_p, C.POINTER(C.c_size_t)]
    lib.told_encoder_bytes.restype = C.POINTER(C.c_uint8)

    for name, extra in (
        ("told_write_null", []),
        ("told_write_bool", [C.c_bool]),
        ("told_write_u64", [C.c_uint64]),
        ("told_write_i64", [C.c_int64]),
        ("told_write_f64", [C.c_double]),
    ):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [C.c_void_p, *extra], C.c_int

    for name in ("told_write_string", "told_write_buffer"):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [C.c_void_p, C.c_char_p, C.c_size_t], C.c_int

    for name in ("told_begin_array", "told_begin_object"):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [C.c_void_p, C.POINTER(C.c_size_t)], C.c_int

    for name in ("told_end_array", "told_end_object"):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [C.c_void_p, C.c_size_t], C.c_int

    lib.told_decode.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Frame)]
    lib.told_decode.restype = C.c_int
    lib.told_frame_size.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(C.c_size_t)]
    lib.told_frame_size.restype = C.c_int
    lib.told_validate.argtypes = [C.c_char_p, C.c_size_t, C.c_size_t]
    lib.told_validate.restype = C.c_int
    lib.told_read_frame.argtypes = [READ_FN, C.c_void_p, C.POINTER(C.c_uint8), C.c_size_t,
                                    C.POINTER(Frame)]
    lib.told_read_frame.restype = C.c_int

    lib.told_as_bool.argtypes = [C.POINTER(Frame), C.POINTER(C.c_bool)]
    lib.told_as_bool.restype = C.c_int
    lib.told_as_i64.argtypes = [C.POINTER(Frame), C.POINTER(C.c_int64)]
    lib.told_as_i64.restype = C.c_int
    lib.told_as_u64.argtypes = [C.POINTER(Frame), C.POINTER(C.c_uint64)]
    lib.told_as_u64.restype = C.c_int
    lib.told_as_f64.argtypes = [C.POINTER(Frame), C.POINTER(C.c_double)]
    lib.told_as_f64.restype = C.c_int

    for name in ("told_as_string", "told_as_buffer"):
        fn = getattr(lib, name)
        fn.argtypes = [C.POINTER(Frame), C.POINTER(C.POINTER(C.c_uint8)), C.POINTER(C.c_size_t)]
        fn.restype = C.c_int

    for name in ("told_array_iter", "told_object_iter"):
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = [C.POINTER(Frame), C.POINTER(Iter)], C.c_int

    lib.told_array_next.argtypes = [C.POINTER(Iter), C.POINTER(Frame), C.POINTER(C.c_bool)]
    lib.told_array_next.restype = C.c_int
    lib.told_object_next.argtypes = [
        C.POINTER(Iter), C.POINTER(C.POINTER(C.c_uint8)), C.POINTER(C.c_size_t),
        C.POINTER(Frame), C.POINTER(C.c_bool),
    ]
    lib.told_object_next.restype = C.c_int

    return lib
