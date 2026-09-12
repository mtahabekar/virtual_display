# Quest Displays — Ubuntu host

The host prototype now creates three KDE virtual monitors, captures each independently through PipeWire, hardware-encodes each with NVIDIA NVENC, and serves three H.264 streams over localhost TCP. A Linux test receiver has received and decoded all three streams, including after reconnecting. No Quest connection is needed for this host-side workflow.

**Current limitation: the 3 × 1440p @ 60 FPS performance target is not met.** The final moving-pattern test delivered 36.8 / 37.6 / 38.0 FPS, with zero host queue drops. A single monitor delivered about 53 FPS. This is a functioning prototype, not a completed performance optimization.

## Current installation and everyday commands

The user services are installed and `quest-displays.service` is enabled for Plasma login. They are running now. No root service, kernel, driver, desktop environment, or system package was changed.

```bash
systemctl --user status quest-displays quest-streams
systemctl --user start quest-displays
systemctl --user stop quest-displays       # stops encoding and removes virtual monitors
systemctl --user restart quest-displays    # recreates monitors and restarts streaming
journalctl --user -u quest-displays -u quest-streams -f
```

`quest-displays.service` owns the monitors and starts `quest-streams.service`. The latter waits for verified output state, then execs the C++ streaming process. Encoder failures can restart streaming without removing the monitors. Stopping the display service also stops streaming. Autostart is tied to `plasma-workspace.target`; session checks require Wayland and a live KWin instance. Login/reboot recreation is configured, but an actual logout/reboot has not been tested in this session. Manual and service restarts have been tested.

Streaming defaults: `127.0.0.1:27183`, H.264 High profile, 4:2:0, target 24 Mbps VBR per stream, no B-frames, 60-frame GOP, NVENC preset p4 / low-latency tuning. Static text does not consume the full bitrate budget. The service does not record video to disk. Edit `~/.config/quest-displays/config.json` to change bitrate or port, then restart `quest-streams`.

## Monitors and diagnostics

| Stream ID | Logical name | KWin 5.27 connector | Mode |
|---|---|---|---|
| 0 | QUEST-1 | Virtual-QUEST-1 | 2560×1440 @ 60 Hz, scale 1 |
| 1 | QUEST-2 | Virtual-QUEST-2 | 2560×1440 @ 60 Hz, scale 1 |
| 2 | QUEST-3 | Virtual-QUEST-3 | 2560×1440 @ 60 Hz, scale 1 |

KWin adds the `Virtual-` prefix. The number and modes are fixed; KDE can arrange the outputs in **System Settings → Display and Monitor → Display Configuration**. You confirmed all three appear there. Ordinary 2D test windows were placed on each output and their contents verified through capture and decoding.

The saved layout places the three virtual monitors to the right of the laptop and HDMI displays. Physical keyboard and mouse remain the only input. Keep Quest outputs enabled at scale 100%; disabling or changing a required mode causes the display owner to exit and the service to recreate it. Arrangement was restored across host restarts.

```bash
cd /home/taha/virtual_display
python3 host/diagnostics/doctor.py
python3 host/diagnostics/doctor.py --json > /tmp/quest-diagnostics.json
python3 tools/kscreen-doctor.py -o
cat "$XDG_RUNTIME_DIR/quest-displays/state.json"
```

The doctor checks Wayland, KWin, PipeWire, NVIDIA, protocol access, exactly three enabled Quest outputs with current 2560×1440 modes, output IDs and live node mappings. The state file is session-local. PipeWire IDs, serials and Wayland globals change on recreation; do not hard-code them. The capture process verifies the node's KWin media name and resolves its object serial before connecting.

## Inspected platform and dependencies

Tested September 11, 2026: Ubuntu 24.04.4 LTS; kernel 7.0.0-31-generic; Plasma 5.27.12 / KWin 5.27.11 on Wayland; RTX 5070 Laptop GPU with driver 580.173.02; PipeWire 1.0.5; GStreamer 1.24.2; Qt 5.15.13; FFmpeg libraries 6.1.1; ADB 34.0.4-debian.

Build dependencies: `build-essential cmake pkg-config python3 qtbase5-dev libwayland-dev libwayland-bin libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libavcodec-dev libavutil-dev`. Runtime requires KDE/KWin Wayland, PipeWire with its GStreamer source, GStreamer base plugins, the NVIDIA driver/encode library and those shared media libraries. KDE utilities come from `libkf5screen-bin` and `libkf5service-bin`.

GStreamer's `nvh264enc` element is absent on this machine. Encoding uses **libavcodec's `h264_nvenc` directly**, which was verified by actual hardware encode and decode tests. No software-encoding fallback exists. No CUDA toolkit or GStreamer NVENC plugin needs installing for this implementation.

GStreamer headers and FFmpeg command-line tools were missing. Five Ubuntu packages were downloaded and extracted into `.deps/`, without installing them: `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `ffmpeg`, `libavdevice60`, and `libopenal1`. The last two satisfy CLI loader dependencies; the project implements no audio. Existing system libraries provide the C++ runtime.

## Build and install

The current checkout is already built. To reproduce the project-local dependency extraction and build:

```bash
cd /home/taha/virtual_display
python3 tools/bootstrap-local-deps.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The extraction script pins the versions tested on this Ubuntu installation. If those versions leave the configured apt repositories, review the replacement package versions instead of changing drivers or kernels. Alternatively, install the listed development packages normally. `-DQUEST_BUILD_MEDIA=OFF` builds only the display owner and diagnostic pattern.

The tests cover output-mode validation, protocol lifetime and failure rollback, fragmented TCP reads, header rejection, IDR gating, slow-client disconnection and reconnect keyframe requests. Tests require local IPC; run them in a normal user terminal when a sandbox blocks sockets.

For an update, stop the services first:

```bash
systemctl --user stop quest-displays
python3 tools/install-user-service.py
systemctl --user enable --now quest-displays
```

Installed files are in `~/.local/libexec/`, `~/.local/share/applications/org.questdisplays.Host.desktop`, and `~/.config/systemd/user/quest-{displays,streams}.service`. The installer preserves an existing configuration file. It registers the exact executable and only the required KDE screencast interface. Use `python3 tools/register-desktop.py --native-user-paths` for display-only registration; the flag avoids this Snap IDE's inherited XDG paths. KDE's custom interface-list property must not have an XDG-style trailing semicolon.

For a foreground display-only run, first stop the service, then run `~/.local/libexec/quest-displays --run`. Ctrl-C removes its three outputs. The lock and KScreen inventory reject duplicate ownership, including disabled Quest outputs.

## Capture locally — Milestone 2

Stop only the encoders when benchmarking; the display owner remains running. This prevents a second consumer from distorting results.

```bash
systemctl --user stop quest-streams
# Terminal A: labeled moving ordinary 2D windows, automatically close after 60 seconds
python3 tools/test-pattern.py all 60
# Terminal B: monitor 0, 12-second capture, PNGs plus a JSON measurement
./build/quest-capture-test 0 12 artifacts/my-capture
# Resume normal streaming afterward
systemctl --user start quest-streams
```

`first.png` and `latest.png` show real captured output content. `capture.json` reports delivered FPS, PTS-based FPS and exact drops in our bounded queue. Static desktops are damage-driven and need not yield 60 new frames per second. The pipeline negotiates system-memory BGRx at the exact output size, not a combined-desktop crop.

## Encode and measure — Milestones 3 and 4

With `quest-streams.service` stopped and the display service still running:

```bash
./build/quest-streams --monitor 0 --seconds 12 --output artifacts/my-encode
python3 tools/media.py ffprobe -v error -count_frames \
  -show_entries stream=codec_name,profile,width,height,pix_fmt,has_b_frames,nb_read_frames \
  -of json artifacts/my-encode/quest-0.h264
python3 tools/media.py ffmpeg -v error -y -i artifacts/my-encode/quest-0.h264 \
  -frames:v 1 artifacts/my-encode/decoded.png

# Run the moving patterns separately while measuring all three:
python3 tools/measure.py --output artifacts/my-three/resources.json -- \
  ./build/quest-streams --seconds 20 --output artifacts/my-three
systemctl --user start quest-streams
```

Each output directory gets independent `quest-0.h264`, `quest-1.h264`, `quest-2.h264` files for the selected monitors and a final `stats.json`. Files in that directory are overwritten by a later run. `--no-record` suppresses video files. `--seconds 0` runs until stopped. Runtime FPS, bitrate, queue drops and mean encode latency are logged every two seconds.

Capture has a two-frame queue. Encoding copies captured BGRx into a CPU frame and uploads it to NVENC, which converts RGB to 4:2:0. This is not a DMA-BUF/zero-copy path. Encode latency measures the CPU copy/upload through packet availability, not end-to-end display latency. Source timestamp anomalies are counted; wire timestamps use the host's monotonic timeline. The host repeats the most recent static frame about once a second, counted separately from capture FPS. PipeWire keepalives are disabled after they caused idle bursts in testing.

Raw `.h264` recordings do not preserve the TCP header timestamps; standalone players may assume the nominal 60 FPS. Use the wire timestamps for actual scheduling in the future Quest client.

## Receive streams — Milestone 5

With the normal user services running:

```bash
python3 tools/receiver.py --seconds 10 --output artifacts/my-received
python3 tools/media.py ffprobe -v error -count_frames -show_streams \
  artifacts/my-received/quest-0.h264
```

The receiver demultiplexes IDs 0–2, validates dimensions and monotonic timestamps, requires SPS/PPS plus an IDR on each new stream, and saves separate Annex-B files and `receiver.json`. Repeat with a new output directory to test reconnect. No Quest is needed.

For a foreground server, stop `quest-streams.service` and use:

```bash
./build/quest-streams --listen 27183 --no-record --output artifacts/manual-server
```

Only one receiver is supported. The server binds IPv4 loopback; it is not exposed to the LAN. Slow clients are disconnected instead of silently losing predictive frames. Full framing, field sizes, bounds, decoder startup and timestamp semantics are documented in [protocol.md](docs/protocol.md).

## USB preparation — Milestone 6

For **Android client → Linux localhost server**, use ADB **reverse**:

```bash
adb devices -l
adb -s DEVICE_SERIAL reverse tcp:27183 tcp:27183
adb -s DEVICE_SERIAL reverse --list
# Remove only this tunnel:
adb -s DEVICE_SERIAL reverse --remove tcp:27183
```

Replace `DEVICE_SERIAL` with the authorized USB device. The future Android client connects to `127.0.0.1:27183` on the device, and ADB carries the connection to the Linux loopback server. Recreate the mapping after USB reconnects if needed. This is ordinary ADB application data, not Quest Link. ADB forward serves the opposite connection direction. See the [official ADB manual](https://android.googlesource.com/platform/packages/modules/adb/+/HEAD/docs/user/adb.1.md).

No device tunnel, Android decoder or glass-to-glass latency has been tested; those require an attached Android/Quest device and the separately developed client. No Quest application, 3D renderer, OpenXR, tracking, input forwarding, audio, clipboard or file transfer has been implemented.

## Results and remaining work

Single-monitor capture: 52.7 FPS with zero host queue drops. Single-monitor live NVENC: 630/630 frames encoded and decoded, about 6.8 ms mean copy/upload/encode latency. Final three-stream moving-pattern benchmark:

| Monitor | Capture/encode FPS | Mean encode latency | Host queue drops |
|---|---:|---:|---:|
| QUEST-1 | 36.8 | 8.10 ms | 0 |
| QUEST-2 | 37.6 | 7.72 ms | 0 |
| QUEST-3 | 38.0 | 7.72 ms | 0 |

These are delivered sample rates, not a measurement of unique compositor presentations. Original source timestamps sometimes repeat or regress; upstream losses cannot be measured here. All three final streams had zero wire timestamp adjustments. The final three-stream resource samples averaged 85.5% of one CPU core in the host and 89.9% in KWin, with 47.8% GPU and 29.1% encoder utilization. GPU utilization includes the desktop and test windows. Pattern bitrate was around 1 Mbps per stream and does not predict browser/video workloads.

[Final benchmark and resource samples](docs/final-benchmark.json), [incremental milestone results](docs/milestone-results.json), and [twelve-file decoder validation](docs/video-validation.json) preserve the evidence. The final recordings decoded all 737 / 753 / 760 frames without errors. [Current host diagnostics](docs/current-host-report.json) also pass. Three distinct decoded test windows were visually inspected. Local initial-connect, reconnect, idle operation, service restart, and saved arrangement restoration passed. Autostart is enabled, but login/reboot testing remains outstanding.

Further host work is performance profiling toward 3×60 FPS, reducing frame readback/copy cost, longer-running stability tests, and representative text/browser workloads. HDR, zero-copy, HEVC/AV1 and 4:4:4 are not part of this prototype. H.264 hardware decoding on Quest and USB end-to-end performance remain client-stage validation.

## Architecture and troubleshooting

C++ handles the permanent display, capture, encoding and transport paths. Python is used for registration, a short service launcher, diagnostics and the test receiver. `host/virtual_display/` owns KWin stream lifetimes; `host/capture/` resolves live PipeWire serials and captures individual outputs; `host/encoder/` wraps libavcodec NVENC; `host/transport/` handles bounded TCP framing; `host/stream_main.cpp` runs one independent worker per monitor. Configuration lives under `host/config/`; [API findings](docs/api-findings.md) explain the version-specific KWin choice.

The selected KWin protocol is native but private/unstable. It is pinned and runtime-version checked for the installed 5.27 server; future Plasma versions require revalidation. KScreen remembers arrangement, while the owning process recreates output lifetimes.

- **KWin absent:** select Plasma (Wayland), not GNOME or X11. The host refuses to replace the compositor.
- **Protocol hidden:** rerun desktop registration for the exact installed executable. Do not disable KWin permission checks. Generic `wayland-info` lacks that app's interface declaration.
- **KScreen crashes:** use `tools/kscreen-doctor.py`. This machine's Qt5 diagnostic crashes in NVIDIA EGL; the wrapper scopes Mesa software EGL to that command while retaining real Wayland output enumeration. KWin and NVENC keep their NVIDIA rendering paths.
- **Snap IDE registration/cache failure:** use `--native-user-paths`; Plasma does not use the IDE's Snap-specific XDG directories.
- **Stale output state after updating:** restart the display service. Numeric identifiers are never persistent. Stop services before replacing a running binary.
- **No capture or encoder initialization failure:** inspect the journal and `doctor.py --json`. NVIDIA-SMI success or encoder enumeration alone is insufficient; actual encoding must pass. There is no software fallback.
- **Connection refused:** check both user services and the configured port. If restart limits were reached after a failure, fix it, then run `systemctl --user reset-failed quest-displays quest-streams` and start the display service.
- **Receiver disconnects under load:** check `slow_client_disconnects` in the server's shutdown statistics. Reconnect; the server will request fresh IDRs. Keep only one receiver attached.
- **Sandbox denies sockets/GPU:** use a normal terminal in the Plasma session; inaccessible resources are not necessarily missing packages.

To disable persistence without deleting the project: `systemctl --user disable --now quest-displays`. The virtual outputs disappear when their owner stops.
