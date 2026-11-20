#include "screen_geometry.h"
// K3-Pico / OpenHarmony 6.1. Manually started; no boot configuration is used.
#include "display_manager.h"
#include "screen_manager.h"
#include "surface_utils.h"
#include <native_image/native_image.h>

#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {
using namespace OHOS;
using namespace OHOS::Rosen;

constexpr uint32_t width = k3::screen::width;
constexpr uint32_t height = k3::screen::height;
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }

int takeInstanceLock()
{
    // The kernel releases this abstract socket when the process exits.
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    constexpr char name[] = "k3.virtual-primary";
    std::memcpy(addr.sun_path + 1, name, sizeof(name) - 1);
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&addr),
                        offsetof(sockaddr_un, sun_path) + sizeof(name)) == 0) {
        return fd;
    }
    std::perror("acquire K3 virtual-primary instance lock");
    if (fd >= 0) close(fd);
    return -1;
}

struct ImageDeleter {
    void operator()(OH_NativeImage* image) const { OH_NativeImage_Destroy(&image); }
};

void onFrame(void* context)
{
    const int fd = *static_cast<const int*>(context);
    const uint64_t one = 1;
    ssize_t result;
    do { result = write(fd, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
}

int consume(OH_NativeImage* image, int fd)
{
    uint64_t frames = 0;
    unsigned long lastReport = 0;
    while (!stopping) {
        pollfd event {fd, POLLIN, 0};
        int ready = poll(&event, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) return 1;
        uint64_t available = 0;
        if (ready > 0 && read(fd, &available, sizeof(available)) == sizeof(available)) {
            for (uint64_t i = 0; i < available; ++i) {
                OHNativeWindowBuffer* buffer = nullptr;
                int fence = -1;
                if (OH_NativeImage_AcquireNativeWindowBuffer(image, &buffer, &fence) != 0) break;
                // No CPU reads or writes. Return the producer's completion fence to the queue.
                int error = OH_NativeImage_ReleaseNativeWindowBuffer(image, buffer, fence);
                if (error != 0) {
                    std::fprintf(stderr, "release buffer error=%d\n", error);
                    return 1;
                }
                ++frames;
            }
        }
        timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (static_cast<unsigned long>(now.tv_sec) - lastReport >= 5) {
            std::printf("virtual surface buffers=%" PRIu64 "\n", frames);
            lastReport = now.tv_sec;
        }
    }
    return 0;
}

int run(bool probe, int& eventFd)
{
    auto& displays = DisplayManager::GetInstance();
    auto& screens = ScreenManager::GetInstance();
    auto before = displays.GetDefaultDisplaySync();
    if (before != nullptr) {
        std::fprintf(stderr, "already has default display=%" PRIu64 " screen=%" PRIu64 "; leaving it alone\n",
                     before->GetId(), before->GetScreenId());
        return 1;
    }
    std::unique_ptr<OH_NativeImage, ImageDeleter> image(OH_ConsumerSurface_Create());
    if (!image) {
        std::fprintf(stderr, "cannot allocate consumer surface\n");
        return 1;
    }
    uint64_t surfaceId = 0;
    if (OH_ConsumerSurface_SetDefaultSize(image.get(), width, height) != 0 ||
        OH_NativeImage_GetSurfaceId(image.get(), &surfaceId) != 0) {
        std::fprintf(stderr, "cannot configure consumer surface\n");
        return 1;
    }
    auto* registry = SurfaceUtils::GetInstance();
    VirtualScreenOption option {};
    option.name_ = "K3 USB desktop";
    option.width_ = width;
    option.height_ = height;
    option.density_ = 1.0f;
    option.isForShot_ = false;
    if (registry != nullptr) option.surface_ = registry->GetSurface(surfaceId);
    if (option.surface_ == nullptr) {
        std::fprintf(stderr, "no registered producer for surface=%" PRIu64 "\n", surfaceId);
        return 1;
    }
    OH_OnFrameAvailableListener listener {&eventFd, onFrame};
    if (OH_NativeImage_SetOnFrameAvailableListener(image.get(), listener) != 0) {
        std::fprintf(stderr, "cannot register frame listener\n");
        return 1;
    }
    ScreenId id = screens.CreateVirtualScreen(option);
    std::printf("created screen=%" PRIu64 " surface=%" PRIu64 "\n", id, surfaceId);
    if (id == SCREEN_ID_INVALID || id == ERROR_ID_NOT_SYSTEM_APP) {
        std::fprintf(stderr, "virtual screen creation rejected\n");
        return 1;
    }
    if (probe || stopping) {
        auto result = screens.DestroyVirtualScreen(id);
        std::printf("probe: destroy result=%d\n", static_cast<int>(result));
        return result == DMError::DM_OK ? 0 : 1;
    }
    if (!displays.SetVirtualScreenAsDefault(id)) {
        auto result = screens.DestroyVirtualScreen(id);
        std::fprintf(stderr, "promotion failed; destroy result=%d\n", static_cast<int>(result));
        return 1;
    }
    std::printf("SetVirtualScreenAsDefault succeeded\n");
    auto display = displays.GetDefaultDisplaySync();
    if (display != nullptr) {
        std::printf("default display=%" PRIu64 " screen=%" PRIu64 " size=%dx%d\n",
                    display->GetId(), display->GetScreenId(), display->GetWidth(), display->GetHeight());
    } else {
        std::printf("default display not published at time of query\n");
    }
    int result = consume(image.get(), eventFd);
    OH_NativeImage_UnsetOnFrameAvailableListener(image.get());
    auto current = displays.GetDefaultDisplaySync();
    if (current != nullptr && current->GetScreenId() != id) {
        std::printf("destroy result=%d\n", static_cast<int>(screens.DestroyVirtualScreen(id)));
    } else {
        std::printf("consumer stopped; OpenHarmony retains the default screen until reset\n");
    }
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 2 || (std::strcmp(argv[1], "--probe") != 0 && std::strcmp(argv[1], "--run") != 0)) {
        std::fprintf(stderr, "Usage: k3-virtual-primary --probe | --run\n");
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    int guard = takeInstanceLock();
    if (guard < 0) return 1;
    int events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (events < 0) { std::perror("eventfd"); close(guard); return 1; }
    int result = run(std::strcmp(argv[1], "--probe") == 0, events);
    close(events);
    close(guard);
    return result;
}
