#!/usr/bin/env python3
"""Measure a host command and KWin CPU plus whole-GPU load, once per second."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def ticks(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12])
    except (OSError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a command is required after --")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    hz = os.sysconf("SC_CLK_TCK")
    kwin = subprocess.run(["pgrep", "-x", "kwin_wayland"], capture_output=True, text=True).stdout.strip().splitlines()
    process = subprocess.Popen(command)
    pids = {"host": process.pid, **({"kwin": int(kwin[0])} if kwin else {})}
    previous = {name: ticks(pid) for name, pid in pids.items()}
    last = started = time.monotonic()
    samples = []
    try:
        while process.poll() is None:
            time.sleep(1)
            now = time.monotonic()
            sample = {"seconds": now - started}
            for name, pid in pids.items():
                current = ticks(pid)
                old = previous[name]
                sample[name + "_cpu_percent_one_core"] = ((current-old)/hz/(now-last)*100) if current is not None and old is not None else None
                previous[name] = current
            gpu = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,utilization.encoder,utilization.decoder,memory.used",
                                  "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=5)
            sample["gpu"] = gpu.stdout.strip() if gpu.returncode == 0 else gpu.stderr.strip()
            samples.append(sample)
            last = now
    except BaseException:
        process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        raise
    finally:
        args.output.write_text(json.dumps({"command": command, "returncode": process.poll(), "samples": samples,
            "cpu_note": "100% means one fully used logical CPU. GPU utilization includes the desktop and test pattern.",
            "gpu_columns": ["gpu_percent", "encoder_percent", "decoder_percent", "memory_MiB"]}, indent=2))
    return process.returncode


if __name__ == "__main__":
    raise SystemExit(main())
