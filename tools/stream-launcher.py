#!/usr/bin/python3
"""Wait for virtual output readiness, then exec the C++ streaming process."""
import json
import os
from pathlib import Path
import sys
import time


def main():
    config_path = Path.home() / ".config/quest-displays/config.json"
    try:
        config = json.loads(config_path.read_text())
        bitrate, port = config["bitrate_mbps"], config["port"]
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
                                      "--output", str(runtime / "streams")])
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.1)
    print("[stream] Timed out waiting for display host readiness", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
