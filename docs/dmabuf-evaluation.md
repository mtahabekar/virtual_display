# DMA-BUF capture evaluation

September 12, 2026; baseline `c4714b3`, two Quest outputs.

**Decision: keep the existing CPU capture path.** A working GPU capture prototype reduced CPU consumption but did not demonstrate a material frame-rate or processing-time improvement sufficient to replace the stable implementation. This does not rule out a better GPU implementation; removal of the CPU copy is not a demonstrated fix for 60 FPS here.

## Corrected experiment

Initial comparisons are excluded: stopping `quest-streams` runs an `ExecStopPost` helper that re-enables the laptop panel. The user identified that this did not match the current layout. The corrected harness disabled physical outputs after stopping the service, saved KScreen inventories, and required the enabled-output set to be exactly `Virtual-QUEST-1` and `Virtual-QUEST-2`. Both were 2560×1440 at 60 Hz, desktop scale 1.25. Test windows reported their actual QScreen names; decoded frames contained the corresponding QUEST-1/QUEST-2 labels. The physical panel was off for both measured backends.

The test pattern now scales its entire drawing to the window, keeping its moving bar visible with fractional desktop scaling. Both backends used this corrected pattern.

Two 20-second-per-backend comparisons used opposite orders. Both captured and encoded both outputs with H.264 NVENC, p1/ultra-low-latency tuning and a 60 Mbps bitrate setting, recording locally. They did not measure Quest rendering, USB transport, mouse latency or glass-to-glass latency. Workload generation and compositor scheduling can limit these frame rates; this test does not establish their exact bottleneck.

| Order | Backend | QUEST-1 FPS | QUEST-2 FPS | Host CPU* | KWin CPU* |
| --- | --- | ---: | ---: | ---: | ---: |
| CPU → GPU | Current CPU | 28.16 | 35.19 | 41.72% | 81.22% |
| CPU → GPU | DMA-BUF | 27.89 | 36.64 | 34.45% | 70.41% |
| GPU → CPU | DMA-BUF | 28.75 | 36.57 | 35.06% | 70.23% |
| GPU → CPU | Current CPU | 28.09 | 35.24 | 42.43% | 79.92% |

*Percent of one CPU core; means exclude the first and final two samples to reduce startup/teardown effects. Streaming-process CPU consumption decreased about 17% relatively, or approximately 0.073 of one core. KWin CPU consumption also decreased. Power was not measured.

Mean capture-copy time was 1.98–2.06 ms with CPU capture and 6.40–6.76 ms with GPU capture, including mapping/synchronization. Mean encoder time was 6.63–7.46 ms with CPU input and 2.71–3.09 ms with CUDA input. Adding these stage means gives roughly 8.6–9.5 ms versus 9.1–9.8 ms; these diagnostic sums are **not end-to-end latency**. The encoder-only improvement moves much of the cost into capture.

All eight corrected H.264 recordings decoded completely with FFmpeg `-xerror` and no errors. One CPU stream dropped one frame in its host queue in the first run; the other seven stream-runs reported zero queue drops. Both backends had source timestamp anomalies, so these measurements do not establish perfectly paced or unique presentation frames.

## Implementation tested

NVIDIA RTX 5070 Laptop GPU, driver 580.173.02, KWin 5.27.11, PipeWire 1.0.5, FFmpeg 6.1.1, Ubuntu 24.04.4.

The prototype negotiated tiled BGRA DMA-BUF buffers, imported them as EGL images/GL textures, registered them with CUDA, and copied into owned CUDA AVFrames for NVENC. Imports were cached per PipeWire buffer and removed with the buffer. Explicit non-mipmapped GL texture filtering was necessary for CUDA registration on this driver.

This eliminates raw-image CPU readback/upload but is **not literally zero-copy**: it makes one GPU-to-GPU copy. That permits prompt return of KWin's buffer while encoding or static-frame repetition retains the owned frame. Direct encoder consumption of the original compositor buffer was not implemented or benchmarked; that would require a different lifetime/synchronization design to avoid reintroducing producer-buffer starvation.

GPU reads are synchronized before returning producer buffers. Optimizing that synchronization might help, but the tested prototype does not demonstrate it. Production adoption would also need prolonged idle/reconnect and failure-path validation.

Primary references: [PipeWire DMA-BUF handling](https://docs.pipewire.org/devel/page_dma_buf.html), [NVIDIA encoder input APIs](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/), [FFmpeg CUDA frame handling](https://github.com/FFmpeg/FFmpeg/blob/n6.1.1/libavutil/hwcontext_cuda.c).

## Evidence and disposition

Local artifacts, ignored by Git:

- `artifacts/cpu-vs-dmabuf-quest-only/{cpu,gpu}/`: stats, resource samples, display inventories, recordings and decoded first frames.
- `artifacts/cpu-vs-dmabuf-quest-only-reverse/{cpu,gpu}/`: reverse-order stats, resource samples, inventories and recordings.
- `artifacts/dmabuf-evaluation/prototype.patch`: experimental capture/encoder changes and comparison harness. CUDA declarations come from MIT-licensed FFmpeg `nv-codec-headers` tag `n12.1.14.0`, with license retained.

The patch is preserved for future profiling, not applied to production sources. It requires the retained test-pattern improvements plus EGL/OpenGL development headers. No new CUDA toolkit, driver, compositor, service configuration or Quest app was installed. The existing streaming service was restarted after each comparison. The repository retains the test-pattern correction and this evaluation, not the GPU capture rewrite.
