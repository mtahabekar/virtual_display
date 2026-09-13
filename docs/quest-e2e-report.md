# Quest 3 end-to-end validation — 2026-09-12

Historical development report: the three-display measurements below describe the older NVIDIA implementation, not the current two-display Intel path. Current stream IDs are only 0 and 1; Intel/Quest validation remains manual.

The Ubuntu host was tested over USB with the Quest client from
[`mtahabekar/Quest-Linux-Virtual-Desktop`](https://github.com/mtahabekar/Quest-Linux-Virtual-Desktop)
at commit `676589ca40ebf7ec80ed41e5faa396ebfe73be2a`.

## Test environment

- Headset: Meta Quest 3, Android 14 / API 34, build `UP1A.231005.007.A1`
- Client: `com.example.questlinuxvirtualdesktop`, version 1.0 (debug build)
- Decoder: Qualcomm `c2.qti.avc.decoder`, reported as hardware, 16 maximum instances
- Transport: USB debugging with `adb reverse tcp:27183 tcp:27183`
- Host: `quest-displays.service` and `quest-streams.service`, TCP listener on `127.0.0.1:27183`
- Video: three independent H.264 High 4:2:0 streams at 2560×1440

## Results

| Check | Result |
| --- | --- |
| QSTV v1 source compatibility audit | Passed |
| One hardware decoder | Passed after one automatic cold-start reconnect |
| Two simultaneous hardware decoders | Passed after one automatic cold-start reconnect |
| Three simultaneous hardware decoders | Passed after one automatic cold-start reconnect |
| Direct decoder Surface output | Passed for all enabled streams |
| Three panel Surface attachment | Passed |
| Labeled Linux content visible in headset capture | Passed |
| Host stream-service restart | All three decoders reset and reconnected with fresh SPS/PPS/IDR |
| App pause/resume | All three decoders closed and recreated successfully |
| ADB reverse absent, then restored | Client retried connection and recovered without relaunch |
| Static desktop for 60 seconds | Stable; no reconnects, errors, or queue growth |
| Host automated tests | 4/4 passed |

The headset selected `c2.qti.avc.decoder` independently for all three streams,
configured each for 2560×1440 direct-Surface output, and reported decoded and
render-submitted frame rates matching received frame rates. During moving-pattern
tests, observed rates were workload-dependent and uneven, ranging roughly from
11 to 43 FPS across short samples. Static desktops settled to the host's intended
approximately 1 FPS repeat rate with decoder queue depth zero.

The host remained connected through the loopback end of the ADB tunnel. A service
restart produced an expected EOF, followed by successful decoder teardown and
reinitialization. Removing the ADB reverse mapping produced expected connection
refusals; restoring it caused the already-running app to connect and initialize
all three streams.

## Evidence and remaining manual checks

- [`quest-view.png`](../artifacts/quest-e2e-2026-09-12/quest-view.png) captures the
  stereoscopic headset view with decoded Linux desktops on spatial panels.
- [`quest-three-patterns.png`](../artifacts/quest-e2e-2026-09-12/quest-three-patterns.png)
  captures labeled moving Linux test content on the panels in the current field of view.

The ADB framebuffer capture and logs verify rendering and stream identity for the
panels visible in the current headset pose. A person wearing the headset must still
confirm QUEST-1/2/3 left-to-right mapping across the full field of view, perceived
text clarity and latency, and hand/controller grab behavior. Physical USB unplug
and replug was approximated by removing and restoring the ADB reverse mapping; a
real cable disconnect remains untested.

## Observed limitations

The client initially reconnects once during a cold start because its three-access-unit
queue can fill while MediaCodec is being constructed. The warmed second attempt
stabilized in every test, but the client should allow a bounded startup allowance
rather than treating normal decoder construction time as backpressure.

The first service build serialized complete access-unit allocation and copying while
holding the transport queue mutex. In the headset this starved QUEST-1 and QUEST-3:
QUEST-2 could receive about 45 FPS while the other streams fell to roughly 8–14 FPS
and 1 FPS, and their host capture queues accumulated drops. Moving packet framing out
of the shared lock fixed that imbalance. A controlled 15-second Quest-connected run
after the change delivered 34.1 / 34.6 / 35.0 FPS, with zero host queue drops, zero
transport discards, zero slow-client disconnects, and matching decoder delivery.

The existing 3×60 FPS goal remains unmet. The remaining gap is the system-memory
capture and upload path rather than Quest decoder capacity. The Quest decoder queues
stayed at zero or one during sustained decoding and all three hardware decoders
remained active.
