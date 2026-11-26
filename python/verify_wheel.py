#!/usr/bin/env python3
"""Install the freshly built wheel into a throwaway venv and run the test from it.

This is the only check that the packaging is right. Running the test from the source tree
proves nothing about the wheel: the loader falls back to ../zig-out/lib, so a wheel missing
the shared library entirely would still appear to work. Here there is no checkout to fall
back on and TOLD_LIBRARY is explicitly cleared.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
wheels = sorted(glob.glob(str(root / "zig-out" / "wheel" / "told-*.whl")))
if not wheels:
    sys.exit("no wheel found in zig-out/wheel -- run `zig build wheel` first")
wheel = wheels[-1]
print(f"wheel: {Path(wheel).name}")

# The platform tag must not be py3-none-any: the wheel carries a native library.
tag = Path(wheel).stem.split("-", 2)[2]
print(f"tag:   {tag}")
if "none-any" in tag:
    sys.exit("FAIL: wheel is tagged platform-independent but contains a native library")

with zipfile.ZipFile(wheel) as zf:
    names = zf.namelist()
libs = [n for n in names if "/_lib/" in n and n.rsplit(".", 1)[-1] in ("dylib", "so", "dll")]
print(f"bundled library: {libs or 'MISSING'}")
if not libs:
    sys.exit("FAIL: no shared library inside the wheel")

tmp = Path(tempfile.mkdtemp(prefix="told-wheel-"))
try:
    venv = tmp / "venv"
    subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    py = venv / "bin" / "python3"

    subprocess.run([str(py), "-m", "pip", "install", "--quiet", wheel], check=True)
    print("installed into a clean venv")

    # Copy the test somewhere with no source tree above it, so the loader cannot cheat.
    script = tmp / "test_told.py"
    shutil.copy(root / "python" / "test_told.py", script)

    env = dict(os.environ)
    env.pop("TOLD_LIBRARY", None)   # must be found inside the package
    env.pop("PYTHONPATH", None)

    result = subprocess.run([str(py), str(script)], cwd=tmp, env=env,
                            capture_output=True, text=True)
    tail = result.stdout.strip().splitlines()[-6:]
    for line in tail:
        print("  " + line)
    if result.returncode != 0:
        print(result.stderr.strip()[-2000:])
        sys.exit("FAIL: the test failed when run from the installed wheel")

    print("OK: wheel installs and works with no checkout and no TOLD_LIBRARY")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
