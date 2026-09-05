#!/usr/bin/env python3
"""Exercises the Python package end to end. Run via `zig build python-demo`.

Takes an optional path to the shared library, which is how the build step points it at a
freshly compiled one.
"""

import os
import sys
import time
from pathlib import Path

if len(sys.argv) > 1:
    os.environ["QNNT_LIBRARY"] = sys.argv[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import qnnt
from qnnt import tld

failures = 0


def check(name, ok, detail=""):
    global failures
    if ok:
        print(f"  ok    {name}" + (f"  {detail}" if detail else ""))
    else:
        failures += 1
        print(f"  FAIL  {name}  {detail}")


print(f"qnnt python demo (TLD wire format v{qnnt.version()})")

print("\nTLD round-trips through Python natives:")
cases = [
    None, True, False, 0, 15, 16, 300, -1, -300, 2**40, -(2**40),
    1.5, -0.0, "hello", "héllo wörld", "", b"", b"\x00\xff\x0a",
    [1, "two", None, [3.5]], {},
    {"seq": 4242, "live": True, "name": "node", "raw": b"\x0a\xff\x00", "tags": [1, 2.5, None]},
]
for value in cases:
    encoded = tld.dumps(value)
    tld.validate(encoded)
    decoded = tld.loads(encoded)
    same = decoded == value
    if isinstance(value, float) and value == 0.0:
        # -0.0 == 0.0, so compare the sign bit explicitly: the encoder must not collapse it.
        import math
        same = math.copysign(1, decoded) == math.copysign(1, value)
    if isinstance(value, tuple):
        same = decoded == list(value)
    check(f"{value!r:.40}", same, f"{len(encoded)}B" + ("" if same else f" -> {decoded!r}"))

# A small frame carries its length in the header nibble, so one byte is already enough.
tiny = tld.dumps({"a": 1})
check("frame_size from the header alone", tld.frame_size(tiny[:1]) == len(tiny), f"{len(tiny)}B")

# Only an extended length field can be incomplete: this one needs 1 header + 2 length bytes.
big = tld.dumps(b"x" * 300)
check("frame_size is None while the length field is short",
      tld.frame_size(big[:1]) is None and tld.frame_size(big[:2]) is None)
check("frame_size once the length field is whole",
      tld.frame_size(big[:3]) == len(big), f"{len(big)}B")

print("\nrejections:")
try:
    tld.dumps({1: "int key"})
    check("int object key rejected", False)
except TypeError as err:
    check("int object key rejected", True, str(err))
try:
    tld.loads(bytes([0xD0]))
    check("reserved nibble rejected", False)
except tld.TldError as err:
    check("reserved nibble rejected", True, err.status_name)
try:
    tld.dumps(object())
    check("unsupported type rejected", False)
except TypeError:
    check("unsupported type rejected", True)

print("\nnode: publish and drain")
# Its own registry port, overridable: this demo is run twice by the build -- once from the
# source tree and once from an installed wheel -- and those two must not contend for one
# registry, since the build runs its steps in parallel.
DEMO_PORT = int(os.environ.get("QNNT_REGISTRY_PORT", "22302"))

with qnnt.Node(registry_port=DEMO_PORT, queue_capacity=8192) as node:
    node.publisher("py/text")
    node.publisher("py/frames")
    node.subscribe("py/text")
    node.subscribe("py/frames", frames=True)
    time.sleep(0.4)  # let discovery settle

    # Publish in rounds, draining as we go, until both topics have delivered enough.
    #
    # A burst sent straight after subscribing is not reliable, and not because of a bug:
    # `publish` reaches subscribers that are connected AT THAT MOMENT, and there is no
    # retention. A subscription whose first resolve loses the race against the publisher's
    # registration backs off a full probe interval before retrying, so a fixed sleep
    # before publishing is a bet on timing. Sending until the data arrives is what a real
    # producer of live telemetry does anyway.
    WANT = 300
    text, frames, polls, sent = [], [], 0, 0
    deadline = time.time() + 15.0
    while (len(text) < WANT or len(frames) < WANT) and time.time() < deadline:
        for _ in range(50):
            node.publish("py/text", f"line-{sent}\n".encode())
            node.publish_object("py/frames", {"seq": sent, "raw": b"\x0a\xff"})
            sent += 1
        for msg in node.poll(timeout=0.1):
            (text if msg.topic == "py/text" else frames).append(msg)
        polls += 1

    text_bodies = [m.text() for m in text]
    frame_values = [m.value() for m in frames]

    check("text messages", len(text_bodies) >= WANT, f"{len(text_bodies)}/{WANT} of {sent} sent")
    check("frame messages", len(frame_values) >= WANT, f"{len(frame_values)}/{WANT} of {sent} sent")
    check("text content intact", text_bodies[0].startswith("line-"), repr(text_bodies[:1]))
    check("frames decode to dicts",
          set(frame_values[0]) == {"seq", "raw"}, repr(frame_values[:1]))

    # Contiguous from wherever delivery began: a gap in the middle would mean the framing
    # lost sync, which is the property worth asserting. Messages published before a
    # subscriber connected are simply not there to receive.
    seqs = [v["seq"] for v in frame_values]
    check("no gaps in what arrived", seqs == list(range(seqs[0], seqs[0] + len(seqs))),
          f"first={seqs[0]} last={seqs[-1]} count={len(seqs)}")
    check("payload with 0x0a survived framing", all(v["raw"] == b"\x0a\xff" for v in frame_values))
    check("nothing dropped", node.dropped == 0, f"dropped={node.dropped}")
    check("batching amortises the crossing", len(text) + len(frames) > polls,
          f"{(len(text)+len(frames))/max(polls,1):.0f} messages per poll")


# ---------------------------------------------------------------- glob subscriptions
#
# One queue fed by every topic matching a pattern, including topics that did not exist when
# the subscription started -- which is the arrangement plain `subscribe` cannot express.

with qnnt.Node(registry_port=DEMO_PORT, queue_capacity=8192) as node:
    node.publisher("py/bus/alpha")
    node.publisher("py/busy-elsewhere")  # a near miss: 'py/bus/*' must not take it
    node.subscribe_pattern("py/bus/*", rescan=0.2)

    # Created AFTER the pattern is already running, so arriving at all proves the rescan
    # found it rather than the initial listing having caught it.
    node.publisher("py/bus/beta")

    got = {}
    deadline = time.time() + 15.0
    while len(got) < 2 and time.time() < deadline:
        for topic in ("py/bus/alpha", "py/bus/beta", "py/busy-elsewhere"):
            node.publish(topic, b"tick\n")
        for msg in node.poll(timeout=0.1):
            got.setdefault(msg.topic, 0)
            got[msg.topic] += 1

    check("glob joins a topic present at the start", got.get("py/bus/alpha", 0) > 0, str(got))
    check("glob joins a topic that appears later", got.get("py/bus/beta", 0) > 0, str(got))
    check("glob leaves a near miss alone", "py/busy-elsewhere" not in got, str(got))
    check("each message says which topic it came from", set(got) <= {"py/bus/alpha", "py/bus/beta"})

    # The same matcher, asked directly -- for a program that routes on the topic, or filters
    # its own traffic back out.
    check("the matcher is available to the caller",
          node.topic_matches("py/bus/*", "py/bus/alpha")
          and node.topic_matches("py/bus/*", "py/bus/deep/er")  # '*' crosses '/'
          and node.topic_matches("py/?us/a", "py/bus/a")
          and not node.topic_matches("py/bus/*", "py/busy-elsewhere")
          and not node.topic_matches("py/bus/*", "other/bus/a"))


# ---------------------------------------------------------------- struct mapping
#
# The counterpart of the TLD_DEFINE_MAP macros in C++ and of tld.pack/unpack in Zig. The
# golden frame at the end is the one that matters: it was produced by the Zig mapping, and
# this side has to both read it and reproduce it exactly, or a Python producer and a Zig
# consumer have quietly diverged.

from dataclasses import dataclass
from enum import IntEnum
from typing import Optional


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

packed = tld.pack(CANONICAL)
check('a dataclass round trips', tld.unpack(Sample, packed) == CANONICAL,
      f"{len(packed)} bytes")

# A field the frame omits falls back to the dataclass default; an unknown key is ignored.
lenient = tld.dumps({'channel': 4, 'value': 1.5, 'added_later': 'ignored'})
check('unknown keys ignored, defaults applied',
      tld.unpack(Reading, lenient) == Reading(4, 1.5, True))

try:
    tld.unpack(Reading, tld.dumps({'channel': 4}))
    check('a missing field without a default is refused', False)
except tld.TldError as err:
    # str(err), not err.args[1]: Error collapses status, message and context into one
    # message, so args has a single element.
    check('a missing field without a default is refused', True, str(err))

# Enums travel as their value, and a name is accepted too, as in Zig.
check('enum by value and by name',
      tld.unpack(Mode, tld.dumps(2)) is Mode.faulted
      and tld.unpack(Mode, tld.dumps('running')) is Mode.running)

golden_path = Path(__file__).resolve().parent.parent / 'lib' / 'testdata' / 'interop.tld'
if golden_path.exists():
    golden = golden_path.read_bytes()
    check('the golden frame from Zig decodes here',
          tld.unpack(Sample, golden) == CANONICAL)
    check('and Python re-encodes it byte for byte', packed == golden,
          f"{len(packed)} vs {len(golden)} bytes")
else:
    # Absent when this demo runs from an installed wheel, which carries the package and the
    # shared library but not the repository's test data. Not a failure: the golden frame is
    # a source-tree check, and the build runs this same script both ways.
    print('  skip  golden frame check (no source tree here)')

print()


try:
    with qnnt.Node(registry_port=DEMO_PORT) as first, qnnt.Node(registry_port=DEMO_PORT) as second:
        check('a second Node is refused', False)
except RuntimeError as err:
    check('a second Node is refused', True, str(err))

print()
print('FAILURES:', failures) if failures else print('all checks passed')
sys.exit(1 if failures else 0)
