// Streams frames delivered to the K3 USB virtual display by RenderService.
#include "screen_geometry.h"
#include "screenshot.h"
#include "display_manager.h"
#include "screen_manager.h"
#include "surface_utils.h"
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <native_buffer/buffer_common.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#ifdef __riscv_vector
#include <riscv_vector.h>
#endif
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace OHOS;
using namespace OHOS::Rosen;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using k3::screen::width;
using k3::screen::height;
constexpr int rowBytes = width * 4;
constexpr size_t frameBytes = size_t(rowBytes) * height;
using Pixels = std::vector<uint8_t>;

enum class Result : int { Complete, Failed, Fallback };
struct Reply { Result result; int framesWritten; };
struct Request {
    int frames;
    int fps;
};

int relaySocket(bool listening)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    // An upgrade may bind the next consumer while the old one is still alive.
    // A client and its daemon must be the same executable, including its inode.
    struct stat exe {};
    if (stat("/proc/self/exe", &exe) != 0) return -1;
    const int n = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
                           "k3.hongjing.frames.%u.%dx%d.%llx.%llx", unsigned(getuid()), width, height,
                           static_cast<unsigned long long>(exe.st_dev), static_cast<unsigned long long>(exe.st_ino));
    const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + n;
    const int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (listening ? (bind(s, reinterpret_cast<sockaddr*>(&address), len) == 0 && listen(s, 4) == 0)
                  : connect(s, reinterpret_cast<sockaddr*>(&address), len) == 0) return s;
    close(s);
    return -1;
}

bool sameUser(int socket)
{
    ucred peer {};
    socklen_t len = sizeof(peer);
    return getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &len) == 0 && peer.uid == getuid();
}

Reply client(const Request& request)
{
    int s = relaySocket(false);
    if (s < 0 || !sameUser(s)) {
        std::fprintf(stderr, "display surface relay unavailable\n");
        if (s >= 0) close(s);
        return {Result::Fallback, 0};
    }
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))] {};
    iovec io {const_cast<Request*>(&request), sizeof(request)};
    msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);
    auto *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    const int out = STDOUT_FILENO;
    std::memcpy(CMSG_DATA(c), &out, sizeof(out));
    Reply result {Result::Fallback, 0};
    if (sendmsg(s, &msg, MSG_NOSIGNAL) == sizeof(request)) {
        // An unexpected disconnect might interrupt a single raw frame.
        // Only the sender knows when it is safe to restart at a frame boundary.
        result = {Result::Failed, 0};
        Reply received {};
        ssize_t n;
        do { n = recv(s, &received, sizeof(received), 0); } while (n < 0 && errno == EINTR);
        if (n == sizeof(received)) result = received;
    }
    close(s);
    return result;
}

int receiveRequest(int s, Request &req)
{
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int))] {};
    iovec io {&req, sizeof(req)};
    msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = ancillary;
    msg.msg_controllen = sizeof(ancillary);
    if (!sameUser(s)) return -1;
    const auto rc = recvmsg(s, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (rc < 0) return -1;
    bool valid = rc == sizeof(req) && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) &&
                 req.frames >= 0 && req.fps >= 0 && req.fps <= 120;
    int out = -1;
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) { valid = false; continue; }
        const size_t bytes = c->cmsg_len - CMSG_LEN(0);
        if (bytes != sizeof(int)) valid = false;
        for (size_t pos = 0; pos + sizeof(int) <= bytes; pos += sizeof(int)) {
            int received;
            std::memcpy(&received, CMSG_DATA(c) + pos, sizeof(received));
            if (out < 0) out = received;
            else { close(received); valid = false; }
        }
    }
    if (valid) return out;
    if (out >= 0) close(out);
    return -1;
}

void copyBytes(uint8_t* dst, const uint8_t* src, size_t size)
{
#ifdef __riscv_vector
    while (size) {
        const size_t vl = vsetvl_e8m8(size);
        const auto bytes = vle8_v_u8m8(src, vl);
        vse8_v_u8m8(dst, bytes, vl);
        src += vl;
        dst += vl;
        size -= vl;
    }
#else
    std::memcpy(dst, src, size);
#endif
}

void copyRgba(const uint8_t *src, int stride, int bpp, bool blueFirst, Pixels &pixels)
{
    if (!blueFirst && bpp == 4 && stride == rowBytes) {
        copyBytes(pixels.data(), src, frameBytes);
        return;
    }
    for (int y = 0; y < height; ++y) {
        auto *out = pixels.data() + y * rowBytes;
        const auto *in = src + y * stride;
        if (!blueFirst && bpp == 4) {
            copyBytes(out, in, rowBytes);
        } else {
            for (int x = 0; x < width; ++x, out += 4, in += bpp) {
                out[0] = in[blueFirst ? 2 : 0];
                out[1] = in[1];
                out[2] = in[blueFirst ? 0 : 2];
                out[3] = 255;
            }
        }
    }
}

class Frames {
public:
    // Single producer. At most a current, a being-written and a being-sent frame.
    Frames()
    {
        for (auto &p : slots) p = std::make_shared<Pixels>(frameBytes);
    }

    std::shared_ptr<Pixels> writable()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &p : slots) if (p.use_count() == 1) return p;
        return {};
    }

    void publish(const std::shared_ptr<Pixels>& frame)
    {
        std::lock_guard<std::mutex> lock(mutex);
        latest = frame;
        ++generation;
        changed.notify_all();
    }

    void beginSession(uint64_t display)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            fallback = false;
            latest.reset();
        }
        // Some RenderService builds need a first native frame after rebinding.
        // If the initial snapshot fails, the sending thread waits for that frame.
        seed(display);
    }

    void requireFallback()
    {
        std::lock_guard<std::mutex> lock(mutex);
        fallback = true;
        changed.notify_all();
    }

    bool seed(uint64_t display)
    {
        CaptureFrame snap {};
        auto dst = writable();
        if (!dst || !captureK3(display, snap)) return false;
        copyRgba(snap.data, snap.stride, snap.bytesPerPixel, snap.blueFirst, *dst);
        {
            std::lock_guard<std::mutex> lock(mutex);
            fallback = false;
        }
        publish(dst);
        return true;
    }

    // Unchanged desktops produce no surface events. Verify the retained image
    // periodically. If the screen changes without another event, let the session
    // use the system snapshot source instead of replaying a stale picture.
    void verifyStill(uint64_t display, bool first)
    {
        CaptureFrame snap {};
        auto scratch = writable();
        if (!scratch || !captureK3(display, snap)) { requireFallback(); return; }
        copyRgba(snap.data, snap.stride, snap.bytesPerPixel, snap.blueFirst, *scratch);
        std::lock_guard<std::mutex> lock(mutex);
        // Alpha on display buffers can differ from snapshot alpha; compare RGB.
        // Compare each pixel to detect small changes such as cursors.
        if (!latest) return;
        for (size_t i = 0; i < frameBytes; i += 4) {
            if (std::memcmp(scratch->data() + i, latest->data() + i, 3)) {
                if (first) { latest = scratch; ++generation; }
                else fallback = true;
                changed.notify_all();
                return;
            }
        }
    }

    void stream(int socket, int out, Request request)
    {
        uint64_t previous = 0;
        int writes = 0;
        int duplicates = 0;
        auto start = Clock::now();
        auto next = start;
        Result result = Result::Failed;
        for (;;) {
            if (request.frames && writes == request.frames) { result = Result::Complete; break; }
            std::this_thread::sleep_until(next);
            std::shared_ptr<Pixels> image;
            {
                std::unique_lock<std::mutex> lock(mutex);
                // Prompt codec startup on a still desktop; after startup send only a slow keepalive.
                changed.wait_for(lock, latest ? (writes < 4 ? 100ms : 1000ms) : 2000ms,
                                 [&] { return (latest && generation != previous) || fallback; });
                if (!latest) { result = Result::Fallback; break; }
                if (fallback) { result = Result::Fallback; break; }
                image = latest;
                if (generation == previous) ++duplicates;
                previous = generation;
            }
            pollfd watch {socket, POLLIN, 0};
            if (poll(&watch, 1, 0) != 0 || !image) break;
            next = request.fps ? Clock::now() + std::chrono::nanoseconds(1000000000 / request.fps) : Clock::now();
            size_t offset = 0;
            while (offset < image->size()) {
                ssize_t n = write(out, image->data() + offset, image->size() - offset);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                offset += n;
            }
            if (offset != image->size()) break;
            if (++writes % 120 == 0) {
                std::fprintf(stderr, "surface_output=%d static_repeats=%d elapsed_ms=%lld\n", writes, duplicates,
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()));
            }
        }
        std::fprintf(stderr, "surface_session_end output=%d static_repeats=%d\n", writes, duplicates);
        Reply reply {result, writes};
        close(out);
        send(socket, &reply, sizeof(reply), MSG_NOSIGNAL);
        close(socket);
        sending = false;
    }
    std::atomic<bool> sending {false};
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::shared_ptr<Pixels>, 3> slots;
    std::shared_ptr<Pixels> latest;
    uint64_t generation = 0;
    bool fallback = false;
};

void onFrame(void* context)
{
    uint64_t one = 1;
    ssize_t n;
    do { n = write(*static_cast<const int*>(context), &one, sizeof(one)); } while (n < 0 && errno == EINTR);
}

bool syncRead(int fd, uint64_t flags)
{
    dma_buf_sync sync {flags | DMA_BUF_SYNC_READ};
    int r;
    do { r = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); } while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r == 0;
}

// Hold a few live DMA-BUF mappings. The retained descriptor prevents inode
// reuse, even when gralloc recycles its numeric file descriptors.
class Mappings {
public:
    ~Mappings() { for (auto &entry : entries) clear(entry); }

    const uint8_t* get(int fd, int size)
    {
        struct stat id {};
        if (fstat(fd, &id) != 0) return nullptr;
        for (auto &entry : entries) {
            if (entry.fd >= 0 && entry.device == id.st_dev && entry.inode == id.st_ino && entry.size == size)
                return static_cast<const uint8_t*>(entry.address);
        }
        auto &entry = entries[next];
        next = (next + 1) % entries.size();
        clear(entry);
        int keep = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (keep < 0) return nullptr;
        void *address = mmap(nullptr, size, PROT_READ, MAP_SHARED, keep, 0);
        if (address == MAP_FAILED) { close(keep); return nullptr; }
        entry = {keep, size, id.st_dev, id.st_ino, address};
        return static_cast<const uint8_t*>(address);
    }
private:
    struct Entry { int fd = -1, size = 0; dev_t device = 0; ino_t inode = 0; void* address = nullptr; };
    std::array<Entry, 4> entries {};
    size_t next = 0;
    static void clear(Entry &entry)
    {
        if (entry.fd >= 0) { munmap(entry.address, entry.size); close(entry.fd); entry = {}; }
    }
};

class Reader {
public:
    Reader(uint64_t display, Frames& frames) : display(display), frames(frames) {}

    bool read(OHNativeWindowBuffer* window)
    {
        auto *buffer = OH_NativeWindow_GetBufferHandleFromNative(window);
        if (!buffer) return false;
        const int format = buffer->format;
        const int stride = buffer->stride;
        if (!seen++) {
            std::fprintf(stderr, "surface_buffer=%dx%d stride=%d format=%d usage=%llx bytes=%u\n",
                        buffer->width, buffer->height, stride, format,
                        static_cast<unsigned long long>(buffer->usage), buffer->size);
        }
        // Check both the compositor-provided allocation and WMS privacy state, fail closed.
        auto dst = frames.writable();
        if (!dst) return true;
        if ((buffer->usage & BUFFER_USAGE_PROTECTED) ||
            !k3DisplayReadable(display)) {
            copyBytes(dst->data(), k3BlackRgba(), frameBytes);
            frames.publish(dst);
            if (!hidden++) std::fprintf(stderr, "protected or private surface suppressed\n");
            return true;
        }
        const bool bgra = format == NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
        if ((!bgra && format != NATIVEBUFFER_PIXEL_FMT_RGBA_8888 && format != NATIVEBUFFER_PIXEL_FMT_RGBX_8888) ||
            buffer->width != width || buffer->height != height || stride < rowBytes ||
            buffer->size <= 0 || size_t(buffer->size) < size_t(height - 1) * stride + rowBytes) return false;
        if (buffer->fd < 0) return false;
        const uint8_t* mapped = maps.get(buffer->fd, buffer->size);
        if (!mapped) { perror("map display buffer"); return false; }
        bool synced = syncRead(buffer->fd, DMA_BUF_SYNC_START);
        if (synced) {
            copyRgba(static_cast<const uint8_t*>(mapped), stride, 4, bgra, *dst);
            synced = syncRead(buffer->fd, DMA_BUF_SYNC_END);
        }
        if (!synced) { perror("sync display buffer"); return false; }
        // A window can enter privacy mode while the hardware fence or CPU copy is in flight.
        if (!k3DisplayReadable(display)) copyBytes(dst->data(), k3BlackRgba(), frameBytes);
        frames.publish(dst);
        return true;
    }
    uint64_t seen = 0, hidden = 0;
private:
    uint64_t display;
    Frames &frames;
    Mappings maps;
};

int serve()
{
    auto &screens = ScreenManager::GetInstance();
    uint64_t displayId;
    if (!getK3Desktop(displayId)) return 1;
    auto display = DisplayManager::GetInstance().GetDefaultDisplaySync();
    if (!display || display->GetId() != displayId) return 1;
    const auto id = display->GetScreenId();
    int listener = relaySocket(true);
    int events = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (listener < 0 || events < 0) { std::perror("surface relay startup"); return 1; }
    Frames frames;
    auto *surface = OH_ConsumerSurface_Create();
    if (!surface) return 1;
    OH_OnFrameAvailableListener notify {&events, onFrame};
    auto sizeResult = OH_ConsumerSurface_SetDefaultSize(surface, width, height);
    auto usageResult = OH_ConsumerSurface_SetDefaultUsage(surface, BUFFER_USAGE_CPU_READ | BUFFER_USAGE_CPU_READ_OFTEN);
    auto listenerResult = OH_NativeImage_SetOnFrameAvailableListener(surface, notify);
    uint64_t surfaceId = 0;
    auto idResult = OH_NativeImage_GetSurfaceId(surface, &surfaceId);
    std::fprintf(stderr, "surface setup size=%d usage=%d listener=%d id=%d\n",
                 sizeResult, usageResult, listenerResult, idResult);
    if (sizeResult || usageResult || listenerResult || idResult) return 1;
    auto *registry = SurfaceUtils::GetInstance();
    if (!registry) return 1;
    auto endpoint = registry->GetSurface(surfaceId);
    if (!endpoint) return 1;
    auto skip = screens.SetScreenSkipProtectedWindow({id}, true);
    std::fprintf(stderr, "privacy skip result=%d\n", int(skip));
    if (skip != DMError::DM_OK) return 1;
    auto attached = screens.SetVirtualScreenSurface(id, endpoint);
    std::fprintf(stderr, "SetVirtualScreenSurface result=%d screen=%llu\n", int(attached), static_cast<unsigned long long>(id));
    if (attached != DMError::DM_OK) return 1;
    Reader reader(displayId, frames);
    uint64_t failures = 0;
    bool receivedSurface = false;
    auto verifyAt = Clock::now() + 3s;
    bool firstIdleCheck = true;
    while (true) {
        pollfd fds[] = {{events, POLLIN, 0}, {listener, POLLIN, 0}};
        if (poll(fds, 2, frames.sending ? 1000 : -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (fds[1].revents & POLLIN) {
            int s = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (s >= 0) {
                pollfd ready {s, POLLIN, 0};
                Request request {};
                int out = poll(&ready, 1, 100) > 0 ? receiveRequest(s, request) : -1;
                if (out >= 0 && !frames.sending.exchange(true)) {
                    frames.beginSession(displayId);
                    firstIdleCheck = true;
                    verifyAt = Clock::now() + 3s;
                    std::thread([&frames, s, out, request] { frames.stream(s, out, request); }).detach();
                } else {
                    if (out >= 0) close(out);
                    Reply decline {Result::Fallback, 0};
                    send(s, &decline, sizeof(decline), MSG_NOSIGNAL);
                    close(s);
                }
            }
        }
        if (fds[0].revents & POLLIN) {
            firstIdleCheck = true;
            verifyAt = Clock::now() + 3s;
            uint64_t available = 0;
            if (read(events, &available, sizeof(available)) != sizeof(available)) continue;
            for (uint64_t n = 0; n < available; ++n) {
                OHNativeWindowBuffer *buffer = nullptr;
                int fence = -1;
                if (OH_NativeImage_AcquireNativeWindowBuffer(surface, &buffer, &fence)) break;
                if (!receivedSurface) {
                    std::fprintf(stderr, "first native display buffer acquired\n");
                    receivedSurface = true;
                }
                if (!frames.sending) {
                    if (OH_NativeImage_ReleaseNativeWindowBuffer(surface, buffer, fence)) return 1;
                    continue;
                }
                bool ready = true;
                if (fence >= 0) {
                    pollfd done {fence, POLLIN, 0};
                    int r;
                    do { r = poll(&done, 1, 250); } while (r < 0 && errno == EINTR);
                    ready = r > 0 && (done.revents & POLLIN);
                    if (ready) { close(fence); fence = -1; }
                }
                if (!ready || !buffer || !reader.read(buffer)) {
                    frames.requireFallback();
                    if (!(failures++ % 120)) std::fprintf(stderr, "surface read failed count=%llu\n", static_cast<unsigned long long>(failures));
                }
                if (OH_NativeImage_ReleaseNativeWindowBuffer(surface, buffer, fence) != 0) return 1;
                if (reader.seen && reader.seen % 120 == 0) std::fprintf(stderr, "surface_acquired=%llu failures=%llu\n",
                    static_cast<unsigned long long>(reader.seen), static_cast<unsigned long long>(failures));
            }
        } else if (frames.sending && Clock::now() >= verifyAt) {
            frames.verifyStill(displayId, firstIdleCheck);
            firstIdleCheck = false;
            verifyAt = Clock::now() + 3s;
        }
    }
}
bool number(const char *arg, int &out)
{
    char *end = nullptr;
    errno = 0;
    long value = strtol(arg, &end, 10);
    if (errno || end == arg || *end || value < 0 || value > INT32_MAX) return false;
    out = static_cast<int>(value);
    return true;
}
}
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--serve") == 0) return serve();
    Request req {};
    if (argc < 2 || argc > 4 || !number(argv[1], req.frames) || (argc > 2 && !number(argv[2], req.fps)) ||
        req.fps > 120 || (argc == 4 && strcmp(argv[3], "rgba"))) {
        std::fprintf(stderr, "Usage: k3-surface-capture --serve | FRAMES [MAX_FPS [rgba]]\n");
        return 2;
    }
    Reply reply = client(req);
    if (reply.result == Result::Complete) return 0;
    if (reply.result != Result::Fallback || reply.framesWritten < 0 ||
        (req.frames && reply.framesWritten > req.frames)) return 1;
    if (req.frames && req.frames == reply.framesWritten) return 0;
    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
    if (n <= 0 || n >= sizeof(self)-1) return 1;
    self[n] = 0;
    char *last = strrchr(self, '/');
    if (!last) return 1;
    last[1] = 0;
    std::string backup = std::string(self) + "k3-capture-pipe";
    std::string count = std::to_string(req.frames ? req.frames - reply.framesWritten : 0);
    std::string rate = std::to_string(req.fps);
    std::fprintf(stderr, "using DMS capture fallback after %d whole frames\n", reply.framesWritten);
    execl(backup.c_str(), backup.c_str(), count.c_str(), rate.c_str(), "rgba", nullptr);
    std::perror("start DMS capture fallback");
    return 1;
}
