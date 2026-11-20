#pragma once
#include <cstdint>

namespace k3 {
enum class TouchAction { Down, Move, Up, Cancel };
// downTimeUs is CLOCK_MONOTONIC time in microseconds, constant for one gesture.
// A true result means the request was submitted. The platform API returns void.
bool injectTouch(TouchAction action, int x, int y, int64_t downTimeUs);
}
