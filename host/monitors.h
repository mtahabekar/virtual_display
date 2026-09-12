#pragma once

namespace quest {
// Virtual monitors QUEST-1..QUEST-N use stream IDs 0..N-1 everywhere: KWin outputs,
// PipeWire capture, NVENC workers and QSTV transport. The Quest client must be
// launched with the same count (adb shell am start ... --ei streams N).
constexpr int MonitorCount = 2;
}
