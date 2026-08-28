"""TLD codec with a json-like interface: dumps/loads over Python natives.

The mapping is the obvious one, and it is not symmetric everywhere -- worth knowing
before round-tripping:

    None            <-> NULL
    bool            <-> BOOL
    int             <-> TINYUINT / INT8..INT64, shortest that fits
    float           <-> FLOAT64
    str             <-> STRING   (UTF-8, validated both ways)
    bytes/bytearray <-> BUFFER   (opaque; the right choice for anything not text)
    list/tuple       -> ARRAY    (decodes back as list)
    dict             -> OBJECT   (decodes back as dict; keys must be str)

Encoding and decoding both go through the C ABI rather than reimplementing the spec in
Python, so there is one implementation to keep correct. The cost is a few FFI calls per
node while decoding, which is fine for message-sized data and would not be for megabytes.
"""

from __future__ import annotations

import ctypes as C
from typing import Any

from . import _lib
from ._lib import Error, Frame, Iter

# Type nibbles, mirroring qnnt_tld.h.
NULL, BOOL, TINYUINT = 0x0, 0x1, 0x2
INT8, INT16, INT32, INT64 = 0x3, 0x4, 0x5, 0x6
FLOAT32, FLOAT64 = 0x7, 0x8
STRING, ARRAY, OBJECT, BUFFER, EXTENDED = 0x9, 0xA, 0xB, 0xC, 0xF

_INTS = (TINYUINT, INT8, INT16, INT32, INT64)

_lib_handle: C.CDLL | None = None


def _lib_ref() -> C.CDLL:
    global _lib_handle
    if _lib_handle is None:
        _lib_handle = _lib.declare(_lib.load())
    return _lib_handle


class TldError(Error):
    """A TLD encode or decode failed."""


def _check(status: int, context: str = "") -> None:
    if status != 0:
        lib = _lib_ref()
        raise TldError(status, lib.qnnt_tld_status_name(status).decode(), context)


def version() -> str:
    return _lib_ref().qnnt_tld_version().decode()


class Encoder:
    """Owns a native encode buffer. Reusable via `reset()`; close it or use `with`."""

    def __init__(self) -> None:
        self._lib = _lib_ref()
        handle = self._lib.qnnt_tld_encoder_create()
        if not handle:
            raise MemoryError("qnnt_tld_encoder_create failed")
        self._handle = C.c_void_p(handle)

    def __enter__(self) -> "Encoder":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        if self._handle:
            self._lib.qnnt_tld_encoder_destroy(self._handle)
            self._handle = C.c_void_p(None)

    __del__ = close

    def reset(self) -> None:
        self._lib.qnnt_tld_encoder_reset(self._handle)

    def bytes(self) -> bytes:
        """A copy, because the native buffer is recycled by the next write."""
        n = C.c_size_t()
        ptr = self._lib.qnnt_tld_encoder_bytes(self._handle, C.byref(n))
        return bytes(bytearray(ptr[0 : n.value])) if n.value else b""

    def write(self, value: Any) -> None:
        """Append one value, recursing into lists and dicts."""
        lib, handle = self._lib, self._handle

        if value is None:
            _check(lib.qnnt_tld_write_null(handle), "write null")
        # bool before int: bool is a subclass of int and must not encode as one.
        elif isinstance(value, bool):
            _check(lib.qnnt_tld_write_bool(handle, value), "write bool")
        elif isinstance(value, int):
            if value < 0:
                _check(lib.qnnt_tld_write_i64(handle, value), "write int")
            else:
                _check(lib.qnnt_tld_write_u64(handle, value), "write uint")
        elif isinstance(value, float):
            _check(lib.qnnt_tld_write_f64(handle, value), "write float")
        elif isinstance(value, str):
            raw = value.encode("utf-8")
            _check(lib.qnnt_tld_write_string(handle, raw, len(raw)), "write str")
        elif isinstance(value, (bytes, bytearray, memoryview)):
            raw = bytes(value)
            _check(lib.qnnt_tld_write_buffer(handle, raw, len(raw)), "write bytes")
        elif isinstance(value, (list, tuple)):
            mark = C.c_size_t()
            _check(lib.qnnt_tld_begin_array(handle, C.byref(mark)), "begin array")
            for item in value:
                self.write(item)
            _check(lib.qnnt_tld_end_array(handle, mark), "end array")
        elif isinstance(value, dict):
            mark = C.c_size_t()
            _check(lib.qnnt_tld_begin_object(handle, C.byref(mark)), "begin object")
            for key, item in value.items():
                if not isinstance(key, str):
                    raise TypeError(f"object keys must be str, got {type(key).__name__}")
                self.write(key)
                self.write(item)
            _check(lib.qnnt_tld_end_object(handle, mark), "end object")
        else:
            raise TypeError(f"cannot encode {type(value).__name__} as TLD")


def dumps(value: Any) -> bytes:
    """Encode one Python value as a single TLD frame."""
    with Encoder() as enc:
        enc.write(value)
        return enc.bytes()


def _decode_frame(lib: C.CDLL, frame: Frame, buffer: bytes) -> Any:
    kind = frame.type

    if kind == NULL:
        return None
    if kind == BOOL:
        out = C.c_bool()
        _check(lib.qnnt_tld_as_bool(C.byref(frame), C.byref(out)), "as bool")
        return out.value
    if kind in _INTS:
        # Signed is the safe reading: a producer carrying unsigned values above the i64
        # range has to reinterpret them itself, since the spec has no UINT family.
        out = C.c_int64()
        _check(lib.qnnt_tld_as_i64(C.byref(frame), C.byref(out)), "as int")
        return out.value
    if kind in (FLOAT32, FLOAT64):
        out = C.c_double()
        _check(lib.qnnt_tld_as_f64(C.byref(frame), C.byref(out)), "as float")
        return out.value
    if kind == STRING:
        ptr, n = C.POINTER(C.c_uint8)(), C.c_size_t()
        _check(lib.qnnt_tld_as_string(C.byref(frame), C.byref(ptr), C.byref(n)), "as str")
        return bytes(bytearray(ptr[0 : n.value])).decode("utf-8") if n.value else ""
    if kind == BUFFER:
        ptr, n = C.POINTER(C.c_uint8)(), C.c_size_t()
        _check(lib.qnnt_tld_as_buffer(C.byref(frame), C.byref(ptr), C.byref(n)), "as bytes")
        return bytes(bytearray(ptr[0 : n.value])) if n.value else b""
    if kind == ARRAY:
        it = Iter()
        _check(lib.qnnt_tld_array_iter(C.byref(frame), C.byref(it)), "array iter")
        items = []
        while True:
            child, have = Frame(), C.c_bool()
            _check(lib.qnnt_tld_array_next(C.byref(it), C.byref(child), C.byref(have)), "array next")
            if not have.value:
                return items
            items.append(_decode_frame(lib, child, buffer))
    if kind == OBJECT:
        it = Iter()
        _check(lib.qnnt_tld_object_iter(C.byref(frame), C.byref(it)), "object iter")
        result: dict[str, Any] = {}
        while True:
            kp, kl, value, have = C.POINTER(C.c_uint8)(), C.c_size_t(), Frame(), C.c_bool()
            _check(
                lib.qnnt_tld_object_next(
                    C.byref(it), C.byref(kp), C.byref(kl), C.byref(value), C.byref(have)
                ),
                "object next",
            )
            if not have.value:
                return result
            key = bytes(bytearray(kp[0 : kl.value])).decode("utf-8")
            result[key] = _decode_frame(lib, value, buffer)
    if kind == EXTENDED:
        # Section 8 leaves extension framing to each extension, so there is nothing
        # generic to return but the code.
        return {"__extension__": frame.ext_code}

    raise TldError(3, "reserved type", f"type nibble 0x{kind:X}")


def loads(data: bytes) -> Any:
    """Decode one TLD frame into Python natives. Ignores trailing bytes."""
    lib = _lib_ref()
    frame = Frame()
    _check(lib.qnnt_tld_decode(data, len(data), C.byref(frame)), "decode")
    return _decode_frame(lib, frame, data)


def frame_size(data: bytes) -> int | None:
    """Bytes the frame at the start of `data` occupies, or None if it is not all here yet."""
    lib = _lib_ref()
    out = C.c_size_t()
    status = lib.qnnt_tld_frame_size(data, len(data), C.byref(out))
    if status == 11:  # QNNT_TLD_INCOMPLETE
        return None
    _check(status, "frame_size")
    return out.value


def validate(data: bytes, max_depth: int = 32) -> None:
    """Check a whole frame, nested contents included. Raises on anything malformed."""
    _check(_lib_ref().qnnt_tld_validate(data, len(data), max_depth), "validate")
