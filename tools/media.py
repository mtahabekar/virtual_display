#!/usr/bin/env python3
"""Run project-local ffmpeg/ffprobe (or the system tool if locally absent)."""
import os
from pathlib import Path
import shutil
import sys

if len(sys.argv) < 2 or sys.argv[1] not in ("ffmpeg", "ffprobe"):
    raise SystemExit("Usage: python3 tools/media.py ffmpeg|ffprobe ARGS...")
root = Path(__file__).resolve().parents[1] / ".deps/root/usr"
binary = root / "bin" / sys.argv[1]
env = dict(os.environ)
if binary.exists():
    libraries = str(root / "lib/x86_64-linux-gnu")
    env["LD_LIBRARY_PATH"] = libraries + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
else:
    found = shutil.which(sys.argv[1])
    if not found:
        raise SystemExit("Run tools/bootstrap-local-deps.py, or install FFmpeg")
    binary = Path(found)
os.execve(str(binary), [str(binary), *sys.argv[2:]], env)
