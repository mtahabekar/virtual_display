#!/usr/bin/env python3
"""Switch physical outputs off while the headset streams, and restore exactly those outputs.

Usage: quest-laptop-display off|on
"off" never disables anything unless a Quest output is enabled, so the session always keeps a screen.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

STATE = Path(os.environ.get("XDG_RUNTIME_DIR", "/nonexistent")) / "quest-displays/laptop-display.json"


def kscreen(*args):
    env = {**os.environ, "QT_QPA_PLATFORM": "wayland"}
    mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    # The installed NVIDIA EGL driver crashes this Qt5 tool; keep Wayland, skip GPU init.
    if Path(mesa).exists():
        env.update(__EGL_VENDOR_LIBRARY_FILENAMES=mesa, LIBGL_ALWAYS_SOFTWARE="1", QT_QUICK_BACKEND="software")
    return subprocess.run(["kscreen-doctor", *args], capture_output=True, text=True, timeout=20, env=env)


def is_quest(output):
    return output.get("name", "").removeprefix("Virtual-").startswith("QUEST-")


def saved():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return []


def off():
    result = kscreen("-j")
    outputs = json.loads(result.stdout).get("outputs", []) if result.returncode == 0 else []
    if not any(is_quest(o) and o.get("enabled") for o in outputs):
        print("[laptop-display] no enabled Quest output; leaving physical outputs on", file=sys.stderr)
        return 1
    physical = [o["name"] for o in outputs if not is_quest(o) and o.get("connected") and o.get("enabled")]
    if not physical:
        return 0
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(sorted(set(saved()) | set(physical))))
    result = kscreen(*[f"output.{name}.disable" for name in physical])
    print(f"[laptop-display] disabled {physical}: exit {result.returncode}", file=sys.stderr)
    return result.returncode


def on():
    names = saved()
    if not names:
        return 0
    result = kscreen(*[f"output.{name}.enable" for name in names])
    print(f"[laptop-display] enabled {names}: exit {result.returncode}", file=sys.stderr)
    if result.returncode == 0:
        STATE.unlink(missing_ok=True)
    return result.returncode


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("on", "off"):
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(off() if sys.argv[1] == "off" else on())
