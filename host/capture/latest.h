#pragma once
#include <deque>
#include <cstdint>
#include <utility>
namespace quest {
// Caller owns synchronization. Never apply this policy to encoded H.264 AUs.
template<class T, class Counter> T takeLatest(std::deque<T> &queue, Counter &drops) {
    if (queue.empty()) return {};
    T latest = std::move(queue.back());
    drops += queue.size() - 1;
    queue.clear();
    return latest;
}
}
