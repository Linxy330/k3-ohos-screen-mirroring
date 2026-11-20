#pragma once
#include "touch_backend.h"
#include <spawn.h>
#include <sys/wait.h>

// The desktop client has already converted window coordinates to video pixels.
// Each connection owns one local finger, regardless of the Flutter pointer ID.
class TouchGesture {
public:
    void handle(const std::vector<uint8_t>& packet) {
        if (packet.size() != 11) return;
        const int id = (int(packet[9]) << 8) | packet[10];
        const uint32_t x = read32(packet.data() + 1);
        const uint32_t y = read32(packet.data() + 5);
        const bool inBounds = x < k3::screen::width && y < k3::screen::height;
        if (packet[0] == 1) {
            uint64_t displayId;
            if (finger != -1 || !inBounds || !getK3Desktop(displayId) || displayId != 0) return;
            started = Clock::now();
            downTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(
                started.time_since_epoch()).count();
            if (k3::injectTouch(k3::TouchAction::Down, x, y, downTimeUs)) {
                finger = id;
                lastX = x;
                lastY = y;
            }
        } else if (finger == id && packet[0] == 2) {
            if (inBounds && (x != lastX || y != lastY)) {
                k3::injectTouch(k3::TouchAction::Move, x, y, downTimeUs);
                lastX = x;
                lastY = y;
            }
        } else if (finger == id && packet[0] == 3) {
            // A fast Mac click can deliver DOWN and UP in the same scheduler tick.
            std::this_thread::sleep_until(started + 35ms);
            k3::injectTouch(k3::TouchAction::Up, inBounds ? x : lastX, inBounds ? y : lastY, downTimeUs);
            finger = -1;
        }
    }

    ~TouchGesture() {
        if (finger != -1) k3::injectTouch(k3::TouchAction::Cancel, lastX, lastY, downTimeUs);
    }

private:
    int finger = -1;
    uint32_t lastX = 0, lastY = 0;
    int64_t downTimeUs = 0;
    Clock::time_point started {};
};
