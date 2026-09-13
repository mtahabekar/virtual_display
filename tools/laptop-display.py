#!/usr/bin/env python3
"""Switch physical outputs off while the headset streams, and restore exactly those outputs.

Usage: quest-laptop-display off|on|layout
"off"    never disables anything unless a Quest output is enabled, so the session always keeps a screen.
"on"     re-enables the saved outputs and lays every output out left to right.
"layout" only lays outputs out: physical displays from x=0, then QUEST-1, QUEST-2.
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


def outputs():
    result = kscreen("-j")
    try:
        return json.loads(result.stdout).get("outputs", []) if result.returncode == 0 else []
    except ValueError:
        return []


def is_quest(output):
    return output.get("name", "").removeprefix("Virtual-").startswith("QUEST-")


def logical_width(output):
    mode = next((m for m in output.get("modes", []) if str(m.get("id")) == str(output.get("currentModeId"))), {})
    width = mode.get("size", {}).get("width") or output.get("size", {}).get("width", 0)
    return round(width / (output.get("scale") or 1))


def saved():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return []


def positions(current, extra_enabled=()):
    # Plasma treats an output that shares its origin with another as redundant and gives it
    # no desktop (a black background), so outputs must never overlap, not even transiently.
    shown = [o for o in current if o.get("enabled") or o.get("name") in extra_enabled]
    ordered = sorted((o for o in shown if not is_quest(o)), key=lambda o: o["name"]) + \
              sorted((o for o in shown if is_quest(o)), key=lambda o: o["name"])
    args, x = [], 0
    for output in ordered:
        args.append(f"output.{output['name']}.position.{x},0")
        x += logical_width(output)
    return args


def off():
    current = outputs()
    if not any(is_quest(o) and o.get("enabled") for o in current):
        print("[laptop-display] no enabled Quest output; leaving physical outputs on", file=sys.stderr)
        return 1
    physical = [o["name"] for o in current if not is_quest(o) and o.get("connected") and o.get("enabled")]
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
    result = kscreen(*[f"output.{name}.enable" for name in names], *positions(outputs(), names))
    print(f"[laptop-display] enabled {names}: exit {result.returncode}", file=sys.stderr)
    if result.returncode == 0:
        STATE.unlink(missing_ok=True)
    return result.returncode


def layout():
    args = positions(outputs())
    result = kscreen(*args) if args else None
    print(f"[laptop-display] layout {args}: exit {result.returncode if result else 0}", file=sys.stderr)
    return result.returncode if result else 0


if __name__ == "__main__":
    commands = {"off": off, "on": on, "layout": layout}
    if len(sys.argv) != 2 or sys.argv[1] not in commands:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(commands[sys.argv[1]]())
