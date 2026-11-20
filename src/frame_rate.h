#pragma once

#include <cstdlib>
#include <cstring>

namespace k3 {
struct FrameRate {
    int limit = 0; // A zero capture limit means no clock-based wait.

    int nominal() const {
        // Rawvideo and the Hongjing configuration packet require a rate.
        // Encoded pictures are sent with their actual monotonic delivery times.
        return limit == 0 ? 60 : limit;
    }

    static bool parse(const char* argument, FrameRate& out) {
        if (std::strcmp(argument, "auto") == 0) {
            out.limit = 0;
            return true;
        }
        if (*argument == '\0' || argument[std::strspn(argument, "0123456789")] != '\0') {
            return false;
        }
        const long value = std::strtol(argument, nullptr, 10);
        if (value > 120) return false;
        out.limit = static_cast<int>(value);
        return true;
    }
};
}
