#!/usr/bin/env python3
"""Read-only host inspection and strict Milestone 1 verification (stdlib only)."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from gpu_devices import discover as discover_gpus
MONITOR_COUNT = 2  # keep in sync with host/monitors.h
EXPECTED = {f"QUEST-{i + 1}" for i in range(MONITOR_COUNT)}


def command(args, timeout=10, env=None):
    if not shutil.which(str(args[0])):
        return {"ok": False, "error": f"Not installed: {args[0]}"}
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, env=env)
        return {"ok": result.returncode == 0, "returncode": result.returncode,
                "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"ok": False, "error": str(exc)}


def canonical(name):
    return name.removeprefix("Virtual-")


def verify_outputs(config):
    """Check current physical mode, not an advertised or logical screen size."""
    errors, monitors = [], []
    outputs = [o for o in config.get("outputs", []) if canonical(o.get("name", "")).startswith("QUEST-")]
    names = [canonical(o.get("name", "")) for o in outputs]
    if len(names) != MONITOR_COUNT or set(names) != EXPECTED:
        errors.append(f"Expected exactly {sorted(EXPECTED)} (optional Virtual- prefix); found {names}")
    for output in outputs:
        name = output["name"]
        current = str(output.get("currentModeId", ""))
        modes = [m for m in output.get("modes", []) if str(m.get("id")) == current]
        mode = modes[0] if len(modes) == 1 else {}
        size = mode.get("size", {})
        if not output.get("connected") or not output.get("enabled"):
            errors.append(f"{name} is disconnected or disabled")
        if (size.get("width"), size.get("height")) != (2560, 1440):
            errors.append(f"{name} current mode is not 2560x1440: {size}")
        if not 59 <= mode.get("refreshRate", 0) <= 61:
            errors.append(f"{name} current refresh is not approximately 60 Hz")
        if "id" not in output:
            errors.append(f"{name} has no KScreen output ID")
        monitors.append({"name": name, "kscreen_id": output.get("id"), "current_mode": mode,
                         "position": output.get("pos"), "scale": output.get("scale")})
    ids = [m["kscreen_id"] for m in monitors]
    if len(ids) != len(set(ids)):
        errors.append("KScreen output IDs are not unique")
    return errors, monitors


def decode(result):
    if not result.get("ok"):
        return None
    try:
        return json.loads(result["stdout"])
    except (ValueError, KeyError):
        return None


def kscreen_environment():
    env = {**os.environ, "QT_QPA_PLATFORM": "wayland"}
    mesa = "/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    if Path(mesa).exists():
        env.update(__EGL_VENDOR_LIBRARY_FILENAMES=mesa,
                   LIBGL_ALWAYS_SOFTWARE="1", QT_QUICK_BACKEND="software")
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print full machine-readable evidence")
    parser.add_argument("--binary", type=Path, default=Path.home() / ".local/libexec/quest-displays")
    parser.add_argument("--state", type=Path, default=Path(os.environ.get("XDG_RUNTIME_DIR", "/nonexistent")) / "quest-displays/state.json")
    args = parser.parse_args()
    if not args.binary.exists() and (ROOT / "build/quest-displays").exists():
        args.binary = ROOT / "build/quest-displays"
    checks, evidence = {}, {}

    def record(name, ok, detail):
        checks[name] = {"ok": bool(ok), "detail": detail}

    record("wayland_session", os.environ.get("XDG_SESSION_TYPE") == "wayland" and bool(os.environ.get("WAYLAND_DISPLAY")),
           {k: os.environ.get(k) for k in ("XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY")})
    bus = command(["busctl", "--user", "call", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "NameHasOwner", "s", "org.kde.KWin"])
    kwin = bus.get("ok") and bus.get("stdout") == "b true"
    record("kwin_running", kwin, bus)
    pw = command(["pw-dump"], timeout=15)
    nodes = decode(pw)
    record("pipewire_connection", isinstance(nodes, list), pw if not isinstance(nodes, list) else "pw-dump connected")
    gpu = command(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"])
    inventory = discover_gpus()
    evidence['gpu_devices'] = inventory
    evidence['nvidia_optional'] = gpu
    intel = any(d['vendor'] == '0x8086' and 'render' in d and Path(d['render']).exists() for d in inventory)
    record('hardware_gpu_available', intel or gpu.get('ok'),
           {'intel_render_available': intel, 'nvidia_available': gpu.get('ok'),
            'note': 'Inventory only; use encoder-smoke for the selected backend. NVIDIA is not required for Intel encoding.'})

    versions = {"kernel": os.uname().release, "os_release": Path("/etc/os-release").read_text()}
    ffmpeg = ["python3", str(ROOT / "tools/media.py"), "ffmpeg"] if (ROOT / ".deps/root/usr/bin/ffmpeg").exists() else ["ffmpeg"]
    for label, cmd in {
        "kwin": ["kwin_wayland", "--version"], "pipewire": ["pipewire", "--version"],
        "gstreamer": ["gst-launch-1.0", "--version"], "ffmpeg": [*ffmpeg, "-version"],
        "packages": ["dpkg-query", "-W", "-f=${binary:Package}\t${db:Status-Abbrev}\t${Version}\n",
                     "plasma-workspace", "kwin-wayland", "qtbase5-dev", "libwayland-dev",
                     "libpipewire-0.3-dev", "libgstreamer1.0-dev", "libgstreamer-plugins-base1.0-dev"],
    }.items():
        versions[label] = command(cmd)
    evidence["versions"] = versions
    evidence["hardware_encoder_capabilities"] = {
        "pipewiresrc": command(["gst-inspect-1.0", "pipewiresrc"]),
        "nvh264enc": command(["gst-inspect-1.0", "nvh264enc"]),
        "libavcodec_h264_nvenc": command([*ffmpeg, "-hide_banner", "-h", "encoder=h264_nvenc"]),
        "libavcodec_h264_vaapi": command([*ffmpeg, "-hide_banner", "-h", "encoder=h264_vaapi"]),
        "ffmpeg_nvenc": command(["ffmpeg", "-hide_banner", "-encoders"]),
    }

    probe_result = command([str(args.binary), "--probe"])
    probe = decode(probe_result)
    evidence["wayland_probe"] = probe if probe is not None else probe_result
    record("kwin_virtual_output_api", isinstance(probe, dict) and probe.get("screencast_bound_version", 0) >= 2,
           "Requires KWin plus the matching desktop registration; plain wayland-info is not authorized for this private interface")
    kscreen_result = command(["kscreen-doctor", "-j"], env=kscreen_environment()) if kwin else {"ok": False, "error": "Skipped: KWin not running; fallback QScreen output would be misleading"}
    kscreen = decode(kscreen_result)
    evidence["kscreen"] = kscreen if kscreen is not None else kscreen_result
    errors, monitors = verify_outputs(kscreen) if isinstance(kscreen, dict) else (["No valid KScreen configuration"], [])
    record("quest_monitors", not errors, errors or monitors)

    state_errors = []
    try:
        state = json.loads(args.state.read_text())
        evidence["host_state"] = state
        if state.get("schema_version") != 1 or not state.get("ready"):
            state_errors.append("Host state is not ready or has an unsupported schema")
        pid = int(state["pid"])
        if pid <= 0 or Path(f"/proc/{pid}/exe").resolve(strict=True).name != "quest-displays":
            state_errors.append("State owner is not a running quest-displays executable")
        if state.get("wayland_display") != os.environ.get("WAYLAND_DISPLAY"):
            state_errors.append("State belongs to a different Wayland display")
        feeds = state.get("monitors", [])
        if len(feeds) != MONITOR_COUNT or {f.get("id") for f in feeds} != set(range(MONITOR_COUNT)):
            state_errors.append("Expected exactly the fixed stream IDs")
        node_ids = [f.get("pipewire_node") for f in feeds]
        if len(set(node_ids)) != MONITOR_COUNT:
            state_errors.append("PipeWire node IDs must be distinct")
        live_node_ids = {n.get("id") for n in nodes or [] if n.get("type") == "PipeWire:Interface:Node"}
        live_outputs = {o["name"]: o for o in (probe or {}).get("outputs", [])}
        for feed in feeds:
            if feed.get("name") != f"QUEST-{feed.get('id', -2) + 1}":
                state_errors.append("Incorrect ID to monitor name mapping")
            if canonical(feed.get("output_name", "")) != feed.get("name"):
                state_errors.append("Incorrect connector to monitor mapping")
            if feed.get("pipewire_node") not in live_node_ids:
                state_errors.append(f"{feed.get('name')}: PipeWire node is absent")
            live = live_outputs.get(feed.get("output_name"))
            if not live or live.get("wayland_global") != feed.get("wayland_global"):
                state_errors.append(f"{feed.get('name')}: Wayland output identifier does not match")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        state_errors.append(f"Cannot validate runtime state: {exc}")
    record("live_stream_mapping", not state_errors, state_errors or "Live output-to-node mappings verified")
    report = {"milestone": 1, "passed": all(v["ok"] for v in checks.values()), "checks": checks, "evidence": evidence}
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        for name, result in checks.items():
            print(f"{'PASS' if result['ok'] else 'FAIL'} {name}: {json.dumps(result['detail'])}")
        print("Milestone 1 automated checks: " + ("PASS (also verify visually in KDE)" if report["passed"] else "NOT YET PASSED"))
        print("Use --json for versions, encoder availability, and full evidence.")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
