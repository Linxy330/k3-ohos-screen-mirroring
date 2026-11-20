#pragma once
#include "focused_text.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace k3::control {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline void finishWorker(pid_t worker) {
    kill(worker, SIGTERM);
    for (int retries = 0; retries < 20; ++retries) {
        pid_t result = waitpid(worker, nullptr, WNOHANG);
        if (result == worker || (result < 0 && errno == ECHILD)) return;
        poll(nullptr, 0, 20);
    }
    kill(worker, SIGKILL);
    while (waitpid(worker, nullptr, 0) < 0 && errno == EINTR) {}
}

// On a headless boot the launcher can create its EntryView before a default
// screen exists. Bringing that 0x0 window forward would show only wallpaper.
// This fixed command has no client-supplied arguments.
constexpr char showLauncher[] = R"SH(
if hidumper -s WindowManagerService -a '-a' |
    grep -Eq '^EntryView[[:space:]]+0[[:space:]].*\[[[:space:]]*0[[:space:]]+0[[:space:]]+0[[:space:]]+0[[:space:]]*\]'; then
    /system/bin/aa stop-service -b com.ohos.launcher -m phone_launcher -a com.ohos.launcher.MainAbility
fi
exec /system/bin/aa start -b com.ohos.launcher -m phone_launcher -a com.ohos.launcher.MainAbility \
    -A action.system.home -e entity.system.home
)SH";

// Keep Home, Back and text submissions ordered without blocking video or heartbeats.
class InputCommands {
public:
    void push(const std::vector<uint8_t>& packet) {
        if (packet.empty()) return;
        const auto kind = packet.front();
        std::string text;
        if (kind == 0x15) {
            if (packet.size() == 1 || std::find(packet.begin()+1, packet.end(), 0) != packet.end()) return;
            text.assign(packet.begin()+1, packet.end());
        } else if (packet.size() != 1 || (kind != 0x12 && kind != 0x13)) {
            return;
        }
        if (pending.size() == 16) {
            std::fprintf(stderr, "input command queue full\n");
            return;
        }
        pending.push_back({kind, std::move(text)});
        update();
    }

    void update() {
        if (child > 0) {
            int status = 0;
            const auto done = waitpid(child, &status, WNOHANG);
            if (done == 0) {
                if (Clock::now() < deadline) return;
                std::fprintf(stderr, "input command timed out\n");
                finishWorker(child);
            } else if (done < 0 && errno == EINTR) {
                return;
            } else if (done < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                std::fprintf(stderr, "input command failed\n");
            }
            child = -1;
        }
        if (pending.empty()) return;

        auto& cmd = pending.front();
        const char* path = "/system/bin/uitest";
        const char* args[] = {"uitest", "uiInput", "keyEvent", "Back", nullptr};
        if (cmd.kind == 0x12) {
            path = "/bin/sh";
            args[0] = "sh";
            args[1] = "-c";
            args[2] = showLauncher;
            args[3] = nullptr;
        }
        const auto rc = cmd.kind == 0x15 ? spawnTextInput(&child, cmd.text) :
            posix_spawn(&child, path, nullptr, nullptr, const_cast<char* const*>(args), environ);
        if (rc != 0) {
            child = -1;
            std::fprintf(stderr, "input command start failed: %d\n", rc);
        }
        deadline = Clock::now() + (cmd.kind == 0x15 ? 12s : 5s);
        pending.pop_front();
    }

    ~InputCommands() {
        if (child > 0) finishWorker(child);
    }

private:
    struct Command {
        uint8_t kind;
        std::string text;
    };
    std::deque<Command> pending;
    pid_t child = -1;
    Clock::time_point deadline {};
};
}
