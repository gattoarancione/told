#!/usr/bin/env python3
"""Exercises the Python package end to end. Run via `zig build python-test`.

Takes an optional path to the shared library, which is how the build step points it at a
freshly compiled one.
"""

import math
import os
import sys
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Optional

if len(sys.argv) > 1:
    os.environ["TOLD_LIBRARY"] = sys.argv[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import told

failures = 0


def check(name, ok, detail=""):
    global failures
    if ok:
        print(f"  ok    {name}" + (f"  {detail}" if detail else ""))
    else:
        failures += 1
        print(f"  FAIL  {name}  {detail}")


print(f"told python test (wire format v{told.version()})")

print("\nround-trips through Python natives:")
cases = [
    None, True, False, 0, 15, 16, 300, -1, -300, 2**40, -(2**40),
    1.5, -0.0, "hello", "héllo wörld", "", b"", b"\x00\xff\x0a",
    [1, "two", None, [3.5]], {},
    {"seq": 4242, "live": True, "name": "node", "raw": b"\x0a\xff\x00", "tags": [1, 2.5, None]},
]
for value in cases:
    encoded = told.dumps(value)
    told.validate(encoded)
    decoded = told.loads(encoded)
    same = decoded == value
    if isinstance(value, float) and value == 0.0:
        # -0.0 == 0.0, so compare the sign bit explicitly: the encoder must not collapse it.
        same = math.copysign(1, decoded) == math.copysign(1, value)
    check(f"{value!r:.40}", same, f"{len(encoded)}B" + ("" if same else f" -> {decoded!r}"))

# A small frame carries its length in the header nibble, so one byte is already enough.
tiny = told.dumps({"a": 1})
check("frame_size from the header alone", told.frame_size(tiny[:1]) == len(tiny), f"{len(tiny)}B")

# Only an extended length field can be incomplete: this one needs 1 header + 2 length bytes.
big = told.dumps(b"x" * 300)
check("frame_size is None while the length field is short",
      told.frame_size(big[:1]) is None and told.frame_size(big[:2]) is None)
check("frame_size once the length field is whole",
      told.frame_size(big[:3]) == len(big), f"{len(big)}B")

print("\nrejections:")
try:
    told.dumps({1: "int key"})
    check("int object key rejected", False)
except TypeError as err:
    check("int object key rejected", True, str(err))
try:
    told.loads(bytes([0xD0]))
    check("reserved nibble rejected", False)
except told.ToldError as err:
    check("reserved nibble rejected", True, err.status_name)
try:
    told.dumps(object())
    check("unsupported type rejected", False)
except TypeError:
    check("unsupported type rejected", True)


# ---------------------------------------------------------------- struct mapping
#
# The counterpart of the TOLD_DEFINE_MAP macros in C++ and of told.pack/unpack in Zig. The
# golden frame at the end is the one that matters: it was produced by the Zig mapping, and
# this side has to both read it and reproduce it exactly, or a Python producer and a Zig
# consumer have quietly diverged.

print("\nstruct mapping:")


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


CANONICAL = Sample(99, 'sensor-a', Mode.faulted,
                   [Reading(1, 3.5, True), Reading(2, -0.25, False)],
                   bytes([0x00, 0xFF, 0x0A]), None, Point(1.5, -2.5))

packed = told.pack(CANONICAL)
check('a dataclass round trips', told.unpack(Sample, packed) == CANONICAL,
      f"{len(packed)} bytes")

# A field the frame omits falls back to the dataclass default; an unknown key is ignored.
lenient = told.dumps({'channel': 4, 'value': 1.5, 'added_later': 'ignored'})
check('unknown keys ignored, defaults applied',
      told.unpack(Reading, lenient) == Reading(4, 1.5, True))

try:
    told.unpack(Reading, told.dumps({'channel': 4}))
    check('a missing field without a default is refused', False)
except told.ToldError as err:
    check('a missing field without a default is refused', True, str(err))

# Enums travel as their value, and a name is accepted too, as in Zig.
check('enum by value and by name',
      told.unpack(Mode, told.dumps(2)) is Mode.faulted
      and told.unpack(Mode, told.dumps('running')) is Mode.running)

golden_path = Path(__file__).resolve().parent.parent / 'src' / 'testdata' / 'interop.tld'
if golden_path.exists():
    golden = golden_path.read_bytes()
    check('the golden frame from Zig decodes here',
          told.unpack(Sample, golden) == CANONICAL)
    check('and Python re-encodes it byte for byte', packed == golden,
          f"{len(packed)} vs {len(golden)} bytes")
else:
    # Absent when this runs from an installed wheel, which carries the package and the
    # shared library but not the repository's test data. Not a failure: the golden frame is
    # a source-tree check, and the build runs this same script both ways.
    print('  skip  golden frame check (no source tree here)')

print()
if failures:
    sys.exit(f"{failures} check(s) failed")
print("all Python checks passed")
