# QSTV v1 transport

The host listens on IPv4 `127.0.0.1:27183` by default, with one client at a time. It sends ordinary TCP bytes. There are no client-to-host messages, authentication, audio, input, or clipboard channels. TCP is a byte stream: receivers must assemble the complete header, then the indicated payload; one `recv()` is not one frame.

Every access unit has this 32-byte header. All multibyte integers are unsigned, big-endian. Serialize fields explicitly; never transmit a C/C++ struct with compiler padding.

| Offset | Bytes | Field | v1 value |
|---|---|---|---|
| 0 | 4 | magic | ASCII `QSTV` |
| 4 | 1 | version | 1 |
| 5 | 1 | message type | 1 = video access unit |
| 6 | 1 | stream ID | 0, 1, 2 = QUEST-1, QUEST-2, QUEST-3 |
| 7 | 1 | codec | 1 = H.264 |
| 8 | 2 | width | 2560 |
| 10 | 2 | height | 1440 |
| 12 | 2 | target FPS numerator | 60 |
| 14 | 2 | target FPS denominator | 1 |
| 16 | 8 | presentation timestamp | microseconds since the streaming host started, monotonic |
| 24 | 4 | payload bytes | 1…8 MiB |
| 28 | 4 | flags | bit 0 = IDR/keyframe; all other bits zero |

The payload immediately follows: one complete H.264 Annex-B access unit, including its NAL start codes. No AVCC length prefixes and no MP4 container are used. A fresh connection requests an IDR independently from each encoder. The server waits for that stream's keyframe, which includes in-band SPS/PPS. The Linux receiver checks for NAL types 5, 7 and 8 before accepting the first frame of each stream.

Timestamps are strictly increasing within each stream and share the host process's monotonic origin. They mark when the host consumes a captured sample, before CPU copy/upload/encode; they are not GPU presentation timestamps and cannot measure glass-to-glass latency. Original PipeWire timestamps are monitored separately (`source_timestamp_nonmonotonic`), because repeated/regressing source values were observed. `timestamp_adjustments` counts any final one-microsecond correction needed to enforce wire ordering.

The FPS fields describe the target, not measured delivery. Rendering damage, capture cost and load determine actual frames. PipeWire keepalives are disabled. On an idle output the host re-encodes its most recent frame roughly once a second, or when a new client requests startup; `static_repeat_frames` counts those separately. Capture FPS excludes these repeats. This is not a 60 FPS synthetic source. A source error ends that stream process instead of endlessly replaying a disconnected output.

The server has a bounded queue (12 packets and 16 MiB, plus at most one 8 MiB packet being sent) and a bounded socket send buffer. It disconnects a client if the queue overflows or a pending write makes no progress for two seconds. It does not silently discard predictive frames and continue an undecodable sequence. On reconnect, the client must reset its decoders and wait for each stream's new SPS/PPS + IDR. A second simultaneous TCP client is closed.

The Android client demultiplexes by stream ID, configures three MediaCodec H.264 decoders from the parameter sets, and submits each complete access unit with its timestamp to the corresponding decoder. This path was tested with three simultaneous Qualcomm hardware decoders on Quest 3 on September 12, 2026; see [quest-e2e-report.md](quest-e2e-report.md).

For USB, use `adb -s DEVICE_SERIAL reverse tcp:27183 tcp:27183`. The Android client connects to device `127.0.0.1:27183`; ADB carries it to the Linux loopback listener. Reapply the mapping after reconnecting the USB device. This is ordinary app networking over ADB, not Quest Link.
