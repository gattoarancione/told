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


try:
    with qnnt.Node(registry_port=DEMO_PORT) as first, qnnt.Node(registry_port=DEMO_PORT) as second:
        check('a second Node is refused', False)
except RuntimeError as err:
    check('a second Node is refused', True, str(err))

print()
print('FAILURES:', failures) if failures else print('all checks passed')
sys.exit(1 if failures else 0)
