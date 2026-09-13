# Intel host implementation

The current product has exactly two 2560×1440 outputs, QUEST-1/stream 0 and QUEST-2/stream 1. This implementation moves encoding and, when DMA-BUF negotiation succeeds, raw-image processing to Intel. It does not claim 60 FPS or reduced end-to-end latency.

## Devices and compositor

Inspection on September 13, 2026 found Intel Arrow Lake graphics, PCI `0000:00:02.0`, driver `i915`, and NVIDIA RTX 5070 Laptop, PCI `0000:02:00.0`, driver `nvidia`. At inspection, Intel was card1/renderD128 and NVIDIA card2/renderD129. These numbers are observations, not defaults in the code. Intel VAAPI initialized through `/dev/dri/by-path/pci-0000:00:02.0-render`.

KWin 5.27.11 already reported `OpenGL vendor string: Intel` and `Mesa Intel(R) Graphics (ARL)` in the current session. No restart was performed. `tools/gpu_devices.py` pairs DRM card/render entries through their sysfs PCI device and vendor; the C++ encoder independently validates Intel vendor `0x8086`. An explicit render-node override is also vendor-validated. No CUDA device visibility is changed.

`tools/configure-kwin-gpu.py --dry-run|--apply|--revert` manages only `~/.config/plasma-workspace/env/quest-intel-primary.sh`. It refuses to overwrite unmanaged content. Apply is idempotent and does not restart anything. The login script resolves stable PCI by-path card links each login and exports Intel-first, NVIDIA-second `KWIN_DRM_DEVICES`. Missing cards leave KWin's default selection intact. Revert removes only this managed file; the next session clears its effect. Other user/system GPU overrides may still take precedence and should be inspected manually if verification disagrees.

KWin 5.27 [upstream device parsing](https://github.com/KDE/kwin/blob/v5.27.11/src/backends/drm/drm_backend.cpp) supports an ordered colon-separated list with escaped colons. Resolving PCI symlinks to current card paths at login avoids embedding escaped PCI colons in that list. The helper is intended for this installed Plasma/KWin stack; revalidate after a compositor upgrade.

## Encoder and pixel paths

`VideoEncoder` in `host/encoder/video_encoder.*` owns H.264 timing, access-unit delivery and backend selection. `intel_vaapi.*` owns Intel surfaces/VPP; `nvenc.*` preserves shared-CUDA-device NVENC initialization and its existing low-latency settings. `auto` attempts Intel VAAPI, then NVENC only with explicit permission. Explicit backends fail instead of silently substituting. No x264 fallback exists. QSV is recognized as a configuration value but deliberately not implemented; it reports an actionable error rather than pretending to use QSV. VAAPI provides the required Linux hardware import/conversion path without another frame interop layer.

Intel uses H.264 High, NV12, no B-frames, one-frame async depth, VBR with a maximum rate, default 60-frame GOP and 50 ms VBV. `gop_frames`, `vbv_ms`, and bitrate are configurable. RGB input is full-range BT.709; Intel VAAPI VideoProc converts into limited-range BT.709 NV12. No CPU RGB→YUV conversion is implemented. NVENC retains its RGB-to-4:2:0 conversion settings.

| Capture/backend | Application full-frame CPU copies | Other transfers/conversions |
| --- | --- | --- |
| CPU → Intel | One PipeWire-to-owned-frame copy; no second encoder-frame memcpy | libavutil/libva RGB upload, then Intel VPP RGB→owned NV12 |
| CPU → NVENC | One PipeWire-to-owned-frame copy; reference-counted FFmpeg wrapper | NVENC upload and GPU RGB→4:2:0 |
| DMA-BUF → Intel | Zero raw-image CPU copies/readback in this host | VAAPI import, one GPU VPP copy/conversion into owned NV12 |

Upload drivers may perform CPU staging copies internally; the CPU path is not claimed to have exactly one total physical memory copy. Imported buffers must be compatible with the selected Intel device; cross-GPU zero-copy is not promised.

DMA-BUF negotiation prefers packed BGRA/BGRx with an explicit modifier. The converter advertises linear layout plus the modifier exported by an Intel-allocated RGB surface. It supports one packed plane/object, validates DMA-BUF size/layout, and imports a DRM PRIME 2 surface. Unsupported layouts/imports fall back to CPU in `auto`. Formats/modifiers not supported here are not silently interpreted as linear. GPU capture is reported only after an actual received DMA-BUF. `capture_memory=dmabuf` requires it; `cpu` avoids it; `auto` permits CPU negotiation or one capture restart after a GPU error.

## Lifetime, scheduling and recovery

The CPU path still copies and requeues within the native PipeWire RT process callback. Intel DMA-BUF import is cached per buffer; import occurs on the control loop when metadata is ready, otherwise once on first use. The callback performs only GPU copy/conversion, never H.264 encoding. It explicitly waits at most 2 ms with `vaSyncSurface2` for that conversion. Driver calls themselves are not a hard real-time guarantee.

If conversion cannot complete within that bound, the stream fails and quarantines the producer buffer and owned destination. Shutdown waits off the RT thread for pending GPU reads before destroying the PipeWire stream. `auto` then retries CPU capture once. It never requeues a buffer still being read by Intel or holds producer buffers through asynchronous encoding. The imported surface is released on buffer removal; encoded/static-repeat frames retain owned NV12 surfaces independently. Pool reuse also checks FFmpeg buffer writability.

This follows the pinned KWin 5.27 producer synchronization model. It does not implement newer explicit-sync timeline protocols; future compositor/PipeWire changes need revalidation. Reference APIs: [PipeWire DMA-BUF handling](https://docs.pipewire.org/devel/page_dma_buf.html), [libva import descriptors](https://github.com/intel/libva/blob/2.20.0/va/va_drmcommon.h), [libva synchronization](https://intel.github.io/libva/group__api__core.html).

The raw queue holds at most two frames; dequeue selects the newest and counts stale raw drops. The fixed pool also bounds retained frames. Capture/queue drop and encoded-frame counters remain lightweight. QSTV framing and transport scheduling remain unchanged: two valid stream IDs, one TCP connection, complete Annex-B AUs, monotonic timestamps, startup/reconnect IDRs with SPS/PPS, bounded packet queue, and slow-client disconnect. Allocation/framing remains outside the shared transport mutex. No arbitrary encoded predictive-frame dropping was added.

The USB watcher, reconnect grace, ADB reverse creation, app launch with `--ei streams 2`, and stop-on-long-disconnect behavior remain unchanged. So do physical-output restore hooks, the connection delay before blanking physical outputs, the disconnected delay before restoring them, and layout recovery. `quest-displays` is not enabled at login by the installer instructions.

## Configuration and deployment

New defaults add `encoder`, `allow_nvenc_fallback`, `capture_memory`, `intel_render_node`, `gop_frames`, and `vbv_ms`. The installer preserves existing config files. Missing encoder fields retain legacy NVENC behavior, so existing users must explicitly migrate. For Intel-only operation use `encoder: intel-vaapi`, or `encoder: auto` with `allow_nvenc_fallback: false`. New-install defaults explicitly permit NVENC fallback for compatibility.

The minimum new build dependency is `libva-dev` 2.20 or newer. The existing bootstrap script extracts matching Noble headers into `.deps`; it does not install drivers or replace libraries. Intel runtime support uses the installed `iHD` media driver. No kernel, Mesa, Plasma, BIOS or CUDA changes were made. Built changes are not automatically installed into the currently running service.

## Validation scope

The synthetic Intel initialization smoke produced two forced IDR access units, each with SPS/PPS, through RGB upload, Intel VPP and VAAPI H.264. A brief live DMA-BUF attempt received CPU memory from the already-active screencast node; it did not establish DMA-BUF operation. The strict mode now rejects that condition clearly. No services or display layout were changed to investigate it. Intel DMA-BUF negotiation/conversion with an otherwise unconsumed node remains a manual check.

Focused tests cover configuration migration/validation, encoder selection/fallback permission, vendor-based device selection with shuffled numbering, two-stream bounds through existing protocol tests, Annex-B parameter-set recovery, and newest-raw-frame queue selection. No FPS, bitrate, thermal, resource or Quest performance comparisons were run.

Final CMake configuration and build passed. CTest passed all five groups: diagnostics, CLI help, protocol lifetime, encoder logic and transport backpressure. The retained NVENC backend also passed its short two-IDR smoke check after removing the redundant application copy. `git diff --check` passed.

## MANUAL VALIDATION REMAINING

1. Deploy the built host using the README install commands. Edit the preserved config to `encoder: intel-vaapi`, `capture_memory: auto`, `allow_nvenc_fallback: false`; retain the existing port, bitrate, and headset settings. Re-enable only `quest-headset` for login automation.
2. **MANUAL SESSION RESTART REQUIRED only if applying the GPU helper:** run `python3 tools/configure-kwin-gpu.py --apply`, save work, log out and select Plasma (Wayland). Run `python3 tools/verify-gpu.py` afterward. The inspected session was already Intel-rendered.
3. **MANUAL INTEL ENCODER / DMA-BUF VALIDATION REQUIRED:** with the new service as the only consumer, inspect `journalctl --user -u quest-streams -f` for Intel device/backend and actual `memory=DMA-BUF`. Use `capture_memory: cpu` for the supported fallback, or `dmabuf` to make unsupported negotiation/import fail visibly. Never infer DMA-BUF from the config setting alone.
4. **MANUAL QUEST TEST REQUIRED:** attach the headset; verify two panels, text/colors, IDR recovery, idle, USB grace/reconnect, and physical-display restoration on disconnect or encoder failure.
5. **MANUAL PERFORMANCE TEST REQUIRED:** assess smoothness, sustained FPS, latency, utilization/VRAM, thermals and simultaneous Isaac Sim/CUDA workloads. Compare VAAPI options manually if needed; QSV is not implemented.
