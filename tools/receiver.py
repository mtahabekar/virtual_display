#!/usr/bin/env python3
"""Receive QSTV v1 TCP frames and demultiplex into independent Annex-B H.264 files."""
import argparse
import json
from pathlib import Path
import socket
import struct
import time

HEADER = struct.Struct("!4sBBBBHHHHQII")
MAX_PAYLOAD = 8 * 1024 * 1024


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        block = sock.recv(size - len(data))
        if not block:
            raise EOFError("TCP connection ended inside a frame" if data else "TCP connection ended")
        data.extend(block)
    return bytes(data)


def parse_header(data):
    magic, version, kind, stream, codec, width, height, fps_num, fps_den, pts, size, flags = HEADER.unpack(data)
    if (magic, version, kind, codec) != (b"QSTV", 1, 1, 1):
        raise ValueError("Unsupported protocol header")
    if stream not in (0, 1, 2) or (width, height, fps_num, fps_den) != (2560, 1440, 60, 1):
        raise ValueError("Invalid stream configuration")
    if size < 1 or size > MAX_PAYLOAD or flags & ~1:
        raise ValueError("Invalid payload length or flags")
    return stream, pts, size, bool(flags & 1)


def annexb_types(payload):
    # Used only for the first access unit to verify independent decoder startup.
    import re
    return {payload[m.end()] & 31 for m in re.finditer(b"\x00\x00(?:\x00)?\x01", payload) if m.end() < len(payload)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27183)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--output", type=Path, default=Path("artifacts/received"))
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("seconds must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    streams, files = {}, {}
    error = None
    started = time.monotonic()
    try:
        with socket.create_connection((args.host, args.port), timeout=10) as sock:
            sock.settimeout(10)
            while time.monotonic() - started < args.seconds:
                stream, pts, size, key = parse_header(read_exact(sock, HEADER.size))
                payload = read_exact(sock, size)
                if stream not in streams:
                    if not key or not {5, 7, 8}.issubset(annexb_types(payload)):
                        raise ValueError(f"Stream {stream} did not begin with IDR + SPS + PPS")
                    files[stream] = (args.output / f"quest-{stream}.h264").open("wb")
                    streams[stream] = {"frames": 0, "bytes": 0, "first_pts_us": pts, "last_pts_us": -1, "keyframes": 0}
                stats = streams[stream]
                if pts <= stats["last_pts_us"]:
                    raise ValueError(f"Stream {stream} timestamps are not increasing")
                files[stream].write(payload)
                stats["last_pts_us"] = pts
                stats["frames"] += 1
                stats["bytes"] += size
                stats["keyframes"] += int(key)
    except (OSError, ValueError, EOFError) as exc:
        error = str(exc)
    finally:
        for file in files.values():
            file.close()
    if set(streams) != {0, 1, 2}:
        error = error or "Did not receive all three streams"
    result = {"success": error is None, "error": error, "seconds": time.monotonic() - started, "streams": streams}
    (args.output / "receiver.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    return 0 if error is None else 1


if __name__ == "__main__":
    raise SystemExit(main())
