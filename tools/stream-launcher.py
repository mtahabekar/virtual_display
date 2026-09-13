#!/usr/bin/python3
"""Wait for virtual output readiness, then exec the C++ streaming process."""
import json
import os
from pathlib import Path
import sys
import time


def encoder_arguments(config):
    # Existing configs retain the known-working NVENC choice. New installs use
    # explicit auto + fallback policy from host/config/default.json.
    backend = config.get('encoder', 'nvenc')
    fallback = config.get('allow_nvenc_fallback', False)
    memory = config.get('capture_memory', 'auto')
    render = config.get('intel_render_node', '')
    gop, vbv = config.get('gop_frames', 60), config.get('vbv_ms', 50)
    if backend not in ('auto', 'intel-vaapi', 'intel-qsv', 'nvenc') or type(fallback) is not bool:
        raise ValueError('Invalid encoder or allow_nvenc_fallback')
    if memory not in ('auto', 'cpu', 'dmabuf') or type(render) is not str:
        raise ValueError('Invalid capture_memory or intel_render_node')
    if type(gop) is not int or not 1 <= gop <= 600 or type(vbv) is not int or not 10 <= vbv <= 1000:
        raise ValueError('Invalid gop_frames or vbv_ms')
    return ['--encoder', backend, '--capture-memory', memory, '--gop', str(gop), '--vbv-ms', str(vbv),
            *(['--intel-render-node', render] if render else []),
            *(['--allow-nvenc-fallback'] if fallback else [])]


def main():
    config_path = Path.home() / ".config/quest-displays/config.json"
    try:
        config = json.loads(config_path.read_text())
        bitrate, port = config["bitrate_mbps"], config["port"]
        laptop_off = config.get("laptop_off_when_connected", True)
        encoder_args = encoder_arguments(config)
        if type(bitrate) is not int or not 1 <= bitrate <= 150 or type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("Invalid bitrate_mbps or port")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"[stream] Invalid config {config_path}: {exc}", file=sys.stderr)
        return 2
    runtime = Path(os.environ["XDG_RUNTIME_DIR"]) / "quest-displays"
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        try:
            state = json.loads((runtime / "state.json").read_text())
            if state.get("ready") and Path(f"/proc/{state['pid']}/exe").resolve(strict=True).name == "quest-displays":
                binary = Path(__file__).resolve().parent / "quest-streams"
                os.execv(str(binary), [str(binary), "--no-record", "--listen", str(port), "--bitrate", str(bitrate),
                                      "--output", str(runtime / "streams"), *encoder_args,
                                      *(["--laptop-off-when-connected"] if laptop_off is True else [])])
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.1)
    print("[stream] Timed out waiting for display host readiness", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
