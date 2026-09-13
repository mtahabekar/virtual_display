# Quest Displays — Linux host

Exactly two KWin virtual displays: **QUEST-1 → stream 0**, **QUEST-2 → stream 1**, both 2560×1440 at nominal 60 Hz/FPS. `host/monitors.h` is the canonical count (`MonitorCount = 2`). The separate [Quest application](https://github.com/mtahabekar/Quest-Linux-Virtual-Desktop) is launched with `--ei streams 2`.

The host supports Intel VAAPI H.264 encoding and retains NVIDIA NVENC for explicit fallback/debug use. Intel DMA-BUF import and GPU RGB→NV12 conversion avoid raw-image CPU readback when negotiation succeeds. A CPU-memory capture fallback remains available. There is no software encoder fallback and no promise of sustained 60 FPS. See [Intel implementation and manual validation](docs/intel-host.md).

## Lifecycle and display safety

`quest-headset.service` is enabled for Plasma login. It uses `adb track-devices` to detect the Quest, recreate `adb reverse tcp:27183 tcp:27183`, start `quest-displays.service`, and launch the client with two streams. A brief USB disappearance retains the displays; a return recreates the tunnel. After the existing three-second disconnect grace, the watcher stops the displays and streaming.

**Do not enable quest-displays permanently at login.** The display service owns the virtual outputs and starts `quest-streams.service`. The stream launcher waits for verified state before starting capture/encoding.

After the client stays connected for 1.5 seconds, headset mode disables enabled physical outputs. It restores the saved physical outputs after four seconds disconnected, on normal streaming exit, and through both services' `ExecStopPost` hooks. The layout helper prevents overlapping output origins when physical displays return. Encoder failure does not remove these recovery hooks.

```bash
systemctl --user status quest-headset quest-displays quest-streams
journalctl --user -u quest-headset -u quest-streams -f
# Emergency physical-output recovery:
~/.local/libexec/quest-laptop-display on
# Manual display lifetime, if needed:
systemctl --user start quest-displays
systemctl --user stop quest-displays
```

## Build and install

Dependencies: C++17, CMake, Python 3, Qt 5.15 development libraries, Wayland client/server and scanner, GLib, PipeWire/SPA headers, FFmpeg libavcodec/libavutil headers, and **libva-dev >= 2.20**. Runtime Intel encoding requires the Intel media VAAPI driver (`iHD`, Ubuntu package `intel-media-va-driver`) and access to the Intel render node. NVENC requires NVIDIA only when selected.

The existing local-extraction workflow includes matching Ubuntu Noble `libva-dev` headers; it does not install system packages or replace graphics drivers.

```bash
python3 tools/bootstrap-local-deps.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

`-DQUEST_BUILD_MEDIA=OFF` builds only the display owner and diagnostic pattern. The media build uses installed runtime libraries and installed or project-local headers. Tests require normal local IPC access.

To deploy after saving work in virtual monitors:

```bash
systemctl --user stop quest-headset quest-displays
python3 tools/install-user-service.py
systemctl --user disable quest-displays
# Update the preserved config as described below before resuming:
systemctl --user enable --now quest-headset
```

The installer preserves `~/.config/quest-displays/config.json`, installs user units/binaries, and registers the exact display executable for KDE's private screencast interface. Existing configuration files without encoder keys **continue to use NVENC**; migrate explicitly to Intel.

## Configuration

Edit `~/.config/quest-displays/config.json` while retaining other existing keys:

```json
{
  "encoder": "auto",
  "allow_nvenc_fallback": false,
  "capture_memory": "auto",
  "intel_render_node": "",
  "bitrate_mbps": 60,
  "gop_frames": 60,
  "vbv_ms": 50,
  "port": 27183,
  "laptop_off_when_connected": true
}
```

- `auto`: Intel VAAPI first, NVENC only when `allow_nvenc_fallback` is explicitly true. The new-install default file permits that fallback; false is appropriate when NVIDIA must remain free.
- `intel-vaapi`: require Intel, fail if initialization fails.
- `nvenc`: explicitly use the preserved NVIDIA path.
- `intel-qsv`: recognized but not implemented; fails with instructions to use VAAPI. There is no silent substitution.
- `capture_memory`: `auto` prefers Intel DMA-BUF and allows CPU negotiation/retry; `cpu` forces system memory; `dmabuf` requires DMA-BUF and fails if unavailable.
- Empty `intel_render_node` discovers an Intel PCI/render node. An explicit path must resolve to an Intel device. Device numbers are never assumed.
- GOP and VBV values are bounded; both encoders use H.264, no B-frames and bounded VBR. VAAPI has one-frame async depth; NVENC retains p1/ULL/no lookahead.

Restart only streaming to apply a config change: `systemctl --user restart quest-streams`. This can briefly restore physical outputs through the existing safety hook.

## GPU selection and checks

```bash
python3 tools/verify-gpu.py
python3 tools/configure-kwin-gpu.py --dry-run
# Optional persistent Intel-first ordering, effective next Plasma login:
python3 tools/configure-kwin-gpu.py --apply
# Undo only the helper's managed file:
python3 tools/configure-kwin-gpu.py --revert
```

The helper resolves stable PCI card identities on each login, lists Intel first and NVIDIA second, and changes only its user-level Plasma environment file. It never logs out, restarts KWin, blacklists NVIDIA, or changes CUDA visibility. See [implementation details](docs/intel-host.md) for installed-version support and checks.

```bash
python3 host/diagnostics/doctor.py
python3 tools/kscreen-doctor.py -o
cat "$XDG_RUNTIME_DIR/quest-displays/state.json"
# Optional short, synthetic hardware initialization/AU checks:
./build/encoder-smoke intel-vaapi
./build/encoder-smoke nvenc
```

KWin adds the `Virtual-` prefix. Both outputs must stay enabled at their fixed mode; desktop scale and arrangement are configurable. Capture resolves live PipeWire object serials and verifies their KWin output names; runtime node IDs are never hardcoded. The hidden screencast cursor mode avoids drawing KWin's software pointer twice.

## Capture, encoding and transport

CPU path: PipeWire BGRx/BGRA → one owned CPU frame copy → Intel upload/VPP or NVENC upload. The encoder no longer copies into a second application-owned CPU frame. DMA-BUF path: packed RGB buffer → Intel VAAPI import → GPU VPP conversion into owned NV12 → H.264. Actual received memory type, pixel format, planes, stride, fourcc and modifier are logged at startup.

Producer buffers return in the native PipeWire process callback after the necessary copy/conversion. Encoding only receives owned frames. The raw queue is capped at two; consumers take the newest and count stale raw drops. Encoded predictive frames are never discarded to implement freshness. Static repeats retain owned frames and use monotonic wire timestamps.

QSTV v1 remains a 32-byte header plus one complete Annex-B H.264 access unit. Only IDs 0 and 1 are valid. Every IDR carries SPS/PPS; startup and reconnect request fresh IDRs. One loopback TCP connection, bounded packet queue and slow-client disconnection remain unchanged. Packet allocation/framing stays outside the shared transport queue lock. See [protocol.md](docs/protocol.md).

Manual recording (stop the normal streaming service first to avoid competing consumers):

```bash
./build/quest-streams --encoder intel-vaapi --capture-memory auto \
  --seconds 2 --output artifacts/manual-intel
# Files: quest-0.h264, quest-1.h264, stats.json
```

The test receiver is `python3 tools/receiver.py`. It validates IDs 0/1, dimensions, framing, timestamps and startup parameter sets. The Quest connects to device localhost through ADB reverse; no LAN exposure or Android changes are involved.

## Historical evidence and remaining validation

The [September 12 NVIDIA DMA-BUF evaluation](docs/dmabuf-evaluation.md) and [Quest end-to-end report](docs/quest-e2e-report.md) are historical. Older three-display JSON reports in `docs/` describe development runs, not the current product or Intel performance. Their original measurements are retained.

Sustained FPS, text/video quality, latency, thermal behavior, reconnect/idle endurance, and concurrent Isaac Sim/CUDA use require manual validation. No performance benchmark was conducted for the Intel implementation. Follow [MANUAL VALIDATION REMAINING](docs/intel-host.md#manual-validation-remaining).
