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
with qnnt.Node(registry_port=22302, queue_capacity=8192) as node:
    node.publisher("py/text")
    node.publisher("py/frames")
    node.subscribe("py/text")
    node.subscribe("py/frames", frames=True)
    time.sleep(0.4)  # let discovery settle

    for i in range(300):
        node.publish("py/text", f"line-{i}\n".encode())
        node.publish_object("py/frames", {"seq": i, "raw": b"\x0a\xff"})

    text, frames, polls = [], [], 0
    deadline = time.time() + 3.0
    while (len(text) < 300 or len(frames) < 300) and time.time() < deadline:
        batch = node.poll(timeout=0.2)
        polls += 1
        for msg in batch:
            if msg.topic == "py/text":
                text.append(msg.text())
            else:
                frames.append(msg.value())

    check("text messages", len(text) == 300, f"{len(text)}/300")
    check("frame messages", len(frames) == 300, f"{len(frames)}/300")
    check("text content intact", text[:1] == ["line-0\n"], repr(text[:1]))
    check("frames decode to dicts", frames[:1] == [{"seq": 0, "raw": b"\x0a\xff"}], repr(frames[:1]))
    check("sequence numbers complete", sorted(f["seq"] for f in frames) == list(range(300)))
    check("payload with 0x0a survived framing", all(f["raw"] == b"\x0a\xff" for f in frames))
    check("nothing dropped", node.dropped == 0, f"dropped={node.dropped}")
    check("batching amortises the crossing", len(text) + len(frames) > polls,
          f"{(len(text)+len(frames))/max(polls,1):.0f} messages per poll")


try:
    with qnnt.Node(registry_port=22302) as first, qnnt.Node(registry_port=22302) as second:
        check('a second Node is refused', False)
except RuntimeError as err:
    check('a second Node is refused', True, str(err))

print()
print('FAILURES:', failures) if failures else print('all checks passed')
sys.exit(1 if failures else 0)
