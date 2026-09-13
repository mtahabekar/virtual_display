#!/usr/bin/env python3
"""Own the Quest monitors only while a Quest headset is attached over USB.

Attach: reapply the ADB reverse tunnel, start quest-displays.service, launch the Quest client.
Detach (absent for UNPLUG_GRACE seconds): stop quest-displays.service, which removes the
virtual monitors; the streaming service's stop hook restores the laptop display first.
"""
import json
import os
from pathlib import Path
import select
import subprocess
import time

CONFIG = Path.home() / ".config/quest-displays/config.json"
PACKAGE = "com.example.questlinuxvirtualdesktop"
MONITOR_COUNT = 2  # keep in sync with host/monitors.h
UNPLUG_GRACE = 3.0


def log(message):
    print(f"[headset-watch] {message}", flush=True)


def run(*args, timeout=60):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return result.returncode, (result.stdout + result.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 1, str(exc)


def quest_serials(listing):
    # One line per device: "SERIAL<TAB>device usb:2-1 product:eureka model:Quest_3 device:eureka transport_id:2"
    serials = set()
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "device" and any(f.startswith("model:Quest") for f in fields[2:]):
            serials.add(fields[0])
    return serials


def attach(serial, config):
    port = config.get("port", 27183)
    code, output = run("adb", "-s", serial, "reverse", f"tcp:{port}", f"tcp:{port}")
    log(f"{serial} attached; reverse tunnel exit {code} {output}")
    code, output = run("systemctl", "--user", "start", "quest-displays.service")
    log(f"start quest-displays exit {code} {output}")
    if config.get("launch_app_on_connect", True) is True:
        code, output = run("adb", "-s", serial, "shell", "am", "start", "-S", "-n",
                           f"{PACKAGE}/.ImmersiveActivity", "--ei", "streams", str(MONITOR_COUNT))
        log(f"launch Quest client exit {code}")


def detach():
    code, output = run("systemctl", "--user", "stop", "quest-displays.service")
    log(f"headset detached; stop quest-displays exit {code} {output}")


def main():
    try:
        config = json.loads(CONFIG.read_text())
    except (OSError, ValueError):
        config = {}
    attached = None          # serial currently owning the monitors
    missing_since = time.monotonic()
    code, _ = run("systemctl", "--user", "is-active", "--quiet", "quest-displays.service")
    displays_running = code == 0
    while True:
        tracker = subprocess.Popen(["adb", "track-devices", "-l"], stdout=subprocess.PIPE)
        fd, buffer = tracker.stdout.fileno(), b""
        while tracker.poll() is None:
            if select.select([fd], [], [], 0.5)[0]:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buffer += chunk
                # Each update is a 4-digit hex length followed by the full device listing.
                while len(buffer) >= 4:
                    size = int(buffer[:4], 16)
                    if len(buffer) < 4 + size:
                        break
                    present = quest_serials(buffer[4:4 + size].decode(errors="replace"))
                    buffer = buffer[4 + size:]
                    if attached in present:
                        if missing_since is not None:
                            # A brief USB drop loses the reverse tunnel; the client reconnects once it is back.
                            code, output = run("adb", "-s", attached, "reverse", f"tcp:{config.get('port', 27183)}", f"tcp:{config.get('port', 27183)}")
                            log(f"{attached} returned; reverse tunnel exit {code} {output}")
                        missing_since = None
                    elif present:
                        attached, missing_since = sorted(present)[0], None
                        attach(attached, config)
                        displays_running = True
                    elif missing_since is None:
                        missing_since = time.monotonic()
            if (attached or displays_running) and missing_since is not None and \
                    time.monotonic() - missing_since >= UNPLUG_GRACE:
                detach()
                attached, displays_running = None, False
        tracker.kill()
        tracker.wait()
        log("adb track-devices ended; restarting")
        time.sleep(2)


if __name__ == "__main__":
    main()
