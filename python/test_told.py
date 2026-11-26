#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Claudio Russo (gattoarancione)

"""Unit tests for the Python package. Run via `zig build python-test`.

Takes an optional path to the shared library as the first argument, which is how the build
step points it at a freshly compiled one; anything after it goes to unittest.
"""

import io
import math
import os
import sys
import unittest
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Optional

if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    os.environ["TOLD_LIBRARY"] = sys.argv.pop(1)
sys.path.insert(0, str(Path(__file__).resolve().parent))

import told  # noqa: E402 -- after the path and library are set up

GOLDEN = Path(__file__).resolve().parent.parent / "src" / "testdata" / "interop.tld"


class RoundTrip(unittest.TestCase):
    CASES = [
        None, True, False, 0, 15, 16, 300, -1, -300, 2**40, -(2**40),
        1.5, "hello", "héllo wörld", "", b"", b"\x00\xff\x0a",
        [1, "two", None, [3.5]], {},
        {"seq": 4242, "live": True, "name": "node", "raw": b"\x0a\xff\x00",
         "tags": [1, 2.5, None]},
    ]

    def test_values_round_trip(self):
        for value in self.CASES:
            with self.subTest(value=value):
                encoded = told.dumps(value)
                told.validate(encoded)
                self.assertEqual(told.loads(encoded), value)

    def test_negative_zero_keeps_its_sign(self):
        self.assertEqual(math.copysign(1, told.loads(told.dumps(-0.0))), -1)

    def test_tuples_decode_as_lists(self):
        self.assertEqual(told.loads(told.dumps((1, 2))), [1, 2])

    def test_bytes_match_the_spec(self):
        self.assertEqual(told.dumps(True), b"\x11")
        self.assertEqual(told.dumps(7), b"\x27")
        self.assertEqual(told.dumps(300), b"\x42\x01\x2c")
        self.assertEqual(told.dumps(-1), b"\x31\xff")
        self.assertEqual(told.dumps("hi"), b"\x92hi")
        self.assertEqual(told.dumps(0.0), b"\x80")
        self.assertEqual(told.dumps(b"x" * 300)[:3], b"\xc9\x01\x2c")

    def test_encoder_is_reusable(self):
        with told.Encoder() as enc:
            enc.write(1)
            enc.reset()
            enc.write([True])
            self.assertEqual(enc.bytes(), b"\xa1\x11")

    def test_loads_ignores_trailing_bytes(self):
        self.assertEqual(told.loads(told.dumps(1) + b"junk"), 1)

    def test_64_bit_bounds_round_trip(self):
        self.assertEqual(told.loads(told.dumps(-(2**63))), -(2**63))
        self.assertEqual(told.loads(told.dumps(2**63 - 1)), 2**63 - 1)

    def test_values_above_i64_decode_as_signed(self):
        # The format has no unsigned family: the bits survive, the reader reinterprets.
        self.assertEqual(told.dumps(2**64 - 1), b"\x68" + b"\xff" * 8)
        self.assertEqual(told.loads(told.dumps(2**64 - 1)), -1)

    def test_integers_beyond_64_bits_are_refused_not_wrapped(self):
        for value in (2**64, -(2**63) - 1, 10**30):
            with self.subTest(value=value), self.assertRaises(OverflowError):
                told.dumps(value)


class Framing(unittest.TestCase):
    def test_frame_size_from_the_header_alone(self):
        tiny = told.dumps({"a": 1})
        self.assertEqual(told.frame_size(tiny[:1]), len(tiny))

    def test_frame_size_waits_for_the_length_field(self):
        big = told.dumps(b"x" * 300)
        self.assertIsNone(told.frame_size(big[:1]))
        self.assertIsNone(told.frame_size(big[:2]))
        self.assertEqual(told.frame_size(big[:3]), len(big))

    def test_validate_finds_buried_damage(self):
        nested_bad = bytes([0xB7, 0x92, ord("o"), ord("k"), 0xA3, 0x92, 0xFF, 0xFE])
        with self.assertRaises(told.ToldError) as caught:
            told.validate(nested_bad)
        self.assertEqual(caught.exception.status_name, "invalid utf-8")

    def test_validate_enforces_depth(self):
        deep = told.dumps([[[[[]]]]])
        told.validate(deep, max_depth=8)
        with self.assertRaises(told.ToldError):
            told.validate(deep, max_depth=2)


class Rejections(unittest.TestCase):
    def test_object_keys_must_be_strings(self):
        with self.assertRaises(TypeError):
            told.dumps({1: "int key"})

    def test_unsupported_types_are_refused(self):
        with self.assertRaises(TypeError):
            told.dumps(object())

    def test_reserved_nibble(self):
        with self.assertRaises(told.ToldError) as caught:
            told.loads(bytes([0xD0]))
        self.assertEqual(caught.exception.status_name, "reserved type")

    def test_deep_nesting_is_a_told_error_not_a_recursion_error(self):
        deep = b"\xa0"
        for _ in range(3000):
            n = len(deep)
            deep = (bytes([0xA0 | n]) if n <= 7
                    else bytes([0xA9]) + n.to_bytes(2, "big") if n > 255
                    else bytes([0xA8, n])) + deep
        with self.assertRaises(told.ToldError) as caught:
            told.loads(deep)
        self.assertEqual(caught.exception.status_name, "depth exceeded")

    def test_max_depth_counts_containers(self):
        nested = told.dumps([[[1]]])
        self.assertEqual(told.loads(nested, max_depth=3), [[[1]]])
        with self.assertRaises(told.ToldError):
            told.loads(nested, max_depth=2)
        self.assertEqual(told.loads(told.dumps(1), max_depth=0), 1)  # scalars need none

    def test_default_depth_allows_reasonable_nesting(self):
        value = 1
        for _ in range(told.MAX_DEPTH):
            value = [value]
        self.assertEqual(told.loads(told.dumps(value)), value)

    def test_truncated(self):
        with self.assertRaises(told.ToldError) as caught:
            told.loads(b"\x92h")
        self.assertEqual(caught.exception.status_name, "truncated")


# ---------------------------------------------------------------- struct mapping


class Mode(IntEnum):
    idle = 0
    running = 1
    faulted = 2


@dataclass
class Reading:
    channel: int
    value: float
    valid: bool = True


@dataclass
class Point:
    x: float
    y: float


@dataclass
class Sample:
    id: int
    name: str
    mode: Mode
    readings: list[Reading]
    raw: bytes
    note: Optional[str]
    origin: Point


@dataclass
class Tagged:
    tags: list[str] = field(default_factory=list)


CANONICAL = Sample(99, "sensor-a", Mode.faulted,
                   [Reading(1, 3.5, True), Reading(2, -0.25, False)],
                   bytes([0x00, 0xFF, 0x0A]), None, Point(1.5, -2.5))


class StructMapping(unittest.TestCase):
    def test_dataclass_round_trips(self):
        self.assertEqual(told.unpack(Sample, told.pack(CANONICAL)), CANONICAL)

    def test_same_bytes_as_the_c_and_cpp_mappings(self):
        # Pinned identically in tests/test_c.c and tests/test_cpp.cpp.
        self.assertEqual(
            told.pack(Reading(1, 0.0, True)),
            b"\xb8\x17\x97channel\x21\x95value\x80\x95valid\x11",
        )

    def test_unknown_keys_ignored_and_defaults_applied(self):
        data = told.dumps({"channel": 4, "value": 1.5, "added_later": "ignored"})
        self.assertEqual(told.unpack(Reading, data), Reading(4, 1.5, True))

    def test_default_factory_counts_as_a_default(self):
        self.assertEqual(told.unpack(Tagged, told.dumps({})), Tagged([]))

    def test_missing_field_without_default_is_refused(self):
        with self.assertRaises(told.ToldError) as caught:
            told.unpack(Reading, told.dumps({"channel": 4}))
        self.assertIn("Reading.value", str(caught.exception))

    def test_enum_by_value_and_by_name(self):
        self.assertIs(told.unpack(Mode, told.dumps(2)), Mode.faulted)
        self.assertIs(told.unpack(Mode, told.dumps("running")), Mode.running)

    def test_optional_carries_null_or_value(self):
        self.assertIsNone(told.unpack(Optional[str], told.dumps(None)))
        self.assertEqual(told.unpack(Optional[str], told.dumps("x")), "x")

    def test_wrong_type_is_refused(self):
        with self.assertRaises(told.ToldError):
            told.unpack(Point, told.dumps([1.0, 2.0]))

    def test_bool_is_not_an_int_or_a_float(self):
        for target in (int, float):
            with self.subTest(target=target), self.assertRaises(told.ToldError):
                told.unpack(target, told.dumps(True))
        with self.assertRaises(told.ToldError):
            told.unpack(Reading, told.dumps({"channel": True, "value": 1.0}))
        self.assertIs(told.unpack(bool, told.dumps(True)), True)
        self.assertEqual(told.unpack(float, told.dumps(2)), 2.0)  # int still widens


@unittest.skipUnless(GOLDEN.exists(), "no source tree here (running from an installed wheel)")
class Golden(unittest.TestCase):
    """The frame the Zig mapping produced. Python has to read it and reproduce it exactly,
    or a Python producer and a Zig consumer have quietly diverged."""

    def test_golden_frame_decodes(self):
        self.assertEqual(told.unpack(Sample, GOLDEN.read_bytes()), CANONICAL)

    def test_python_reencodes_it_byte_for_byte(self):
        self.assertEqual(told.pack(CANONICAL), GOLDEN.read_bytes())


# ---------------------------------------------------------------- stream reading


def three_frames() -> bytes:
    return told.dumps({"seq": 4242}) + told.dumps(b"x" * 300) + told.dumps(7)


class OneByteAtATime(io.RawIOBase):
    """A stream that never returns more than one byte, to split every boundary."""

    def __init__(self, data: bytes) -> None:
        self._data, self._pos, self.calls = data, 0, 0

    def readable(self) -> bool:
        return True

    def readinto(self, b) -> int:
        self.calls += 1
        if self._pos == len(self._data):
            return 0
        b[0] = self._data[self._pos]
        self._pos += 1
        return 1


class ReadOnly:
    """Has read() but no readinto(), like many hand-written stream wrappers."""

    def __init__(self, data: bytes) -> None:
        self._buf = io.BytesIO(data)

    def read(self, n: int) -> bytes:
        return self._buf.read(n)


class Reader(unittest.TestCase):
    def test_frames_come_out_in_order_and_the_rest_is_untouched(self):
        stream = io.BytesIO(three_frames() + b"tail")
        reader = told.Reader(stream)
        self.assertEqual(reader.load(), {"seq": 4242})
        self.assertEqual(reader.load(), b"x" * 300)
        self.assertEqual(reader.load(), 7)
        self.assertEqual(stream.read(), b"tail")

    def test_iteration_stops_at_the_end(self):
        self.assertEqual(list(told.Reader(io.BytesIO(three_frames()))),
                         [{"seq": 4242}, b"x" * 300, 7])

    def test_read_frame_returns_raw_bytes_then_none(self):
        reader = told.Reader(io.BytesIO(told.dumps("hi")))
        self.assertEqual(reader.read_frame(), b"\x92hi")
        self.assertIsNone(reader.read_frame())

    def test_load_raises_eof_at_the_end(self):
        with self.assertRaises(EOFError):
            told.Reader(io.BytesIO(b"")).load()

    def test_short_reads(self):
        stream = OneByteAtATime(three_frames())
        self.assertEqual(len(list(told.Reader(stream))), 3)
        self.assertEqual(stream.calls, len(three_frames()) + 1)

    def test_streams_with_only_read(self):
        self.assertEqual(list(told.Reader(ReadOnly(three_frames()))),
                         [{"seq": 4242}, b"x" * 300, 7])

    def test_module_level_helpers(self):
        stream = io.BytesIO(told.dumps(1) + told.dumps(2))
        self.assertEqual(told.read_frame(stream), b"\x21")
        self.assertEqual(told.load(stream), 2)
        self.assertIsNone(told.read_frame(stream))

    def test_a_cut_stream_is_truncated(self):
        with self.assertRaises(told.ToldError) as caught:
            told.Reader(io.BytesIO(three_frames()[:3])).read_frame()
        self.assertEqual(caught.exception.status_name, "truncated")

    def test_a_frame_bigger_than_max_size_is_refused_early(self):
        stream = io.BytesIO(told.dumps(b"x" * 300))
        with self.assertRaises(told.ToldError) as caught:
            told.Reader(stream, max_size=64).read_frame()
        self.assertEqual(caught.exception.status_name, "frame too large")
        self.assertEqual(stream.tell(), 3)  # the length field, and not a byte of data

    def test_the_streams_exception_comes_through(self):
        class Broken:
            def readinto(self, b):
                raise ConnectionResetError("peer gone")

        with self.assertRaises(ConnectionResetError):
            told.load(Broken())

    def test_a_non_blocking_stream_without_data_is_an_error_not_an_end(self):
        class Empty:
            def readinto(self, b):
                return None

        with self.assertRaises(BlockingIOError):
            told.load(Empty())

    def test_max_size_must_be_positive(self):
        with self.assertRaises(ValueError):
            told.Reader(io.BytesIO(b""), max_size=0)


if __name__ == "__main__":
    print(f"told python unit tests (wire format v{told.version()})")
    unittest.main(verbosity=2)
