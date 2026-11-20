#include "screen_geometry.h"
#include "screenshot.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;
using k3::screen::width;
using k3::screen::height;
constexpr size_t rowBytes=width*4, frameBytes=rowBytes*height;

bool emitAll(const uint8_t *p,size_t n) {
    while(n) {
        ssize_t k=write(STDOUT_FILENO,p,n);
        if(k<0 && errno==EINTR) continue;
        if(k<=0) return false;
        p+=k; n-=k;
    }
    return true;
}

bool emitRgba(const CaptureFrame &f,std::vector<uint8_t> &packed) {
    if(f.bytesPerPixel==4 && !f.blueFirst && f.stride==rowBytes)
        return emitAll(f.data,frameBytes);
    if(packed.empty()) packed.resize(frameBytes);
    for(int row=0;row<height;++row) {
        const uint8_t *src=f.data+row*f.stride;
        uint8_t *dst=packed.data()+row*rowBytes;
        if(f.bytesPerPixel==4 && !f.blueFirst) {
            std::memcpy(dst,src,rowBytes);
            continue;
        }
        for(int col=0;col<width;++col,src+=f.bytesPerPixel,dst+=4) {
            dst[0]=src[f.blueFirst ? 2:0];
            dst[1]=src[1];
            dst[2]=src[f.blueFirst ? 0:2];
            dst[3]=255;
        }
    }
    return emitAll(packed.data(),packed.size());
}

class Writer {
public:
    bool publish(CaptureFrame next) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[&]{return !pending || failed;});
        if(failed) return false;
        pending=std::move(next);
        changed.notify_all();
        return true;
    }
    void finish() {
        std::lock_guard<std::mutex> lock(mutex);
        done=true;
        changed.notify_all();
    }
    void run() {
        std::vector<uint8_t> packed;
        for(;;) {
            CaptureFrame current{};
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock,[&]{return pending || done;});
                if(!pending) return;
                current=std::move(*pending);
                pending.reset();
                changed.notify_all();
            }
            auto start=Clock::now();
            if(!emitRgba(current,packed)) {
                std::lock_guard<std::mutex> lock(mutex);
                failed=true;
                changed.notify_all();
                return;
            }
            totalWriteSeconds+=Seconds(Clock::now()-start).count();
            ++frames;
        }
    }
    long frames=0;
    double totalWriteSeconds=0;
    bool failed=false;
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<CaptureFrame> pending;
    bool done=false;
};
bool nonnegative(const char *arg,long &out) {
    char *end=nullptr; errno=0;
    out=std::strtol(arg,&end,10);
    return !errno && *arg && !*end && out>=0;
}
}
int main(int argc,char **argv) {
    long frames=0,fps=0;
    if(argc<2 || argc>4 || !nonnegative(argv[1],frames) ||
       (argc>2 && (!nonnegative(argv[2],fps) || fps>120)) ||
       (argc==4 && std::strcmp(argv[3],"rgba"))) {
        std::fprintf(stderr,"Usage: k3-capture-pipe FRAMES [MAX_FPS [rgba]], 0 means unlimited\n");
        return 2;
    }
    uint64_t display;
    if(!getK3Desktop(display)) { std::fprintf(stderr,"K3 virtual desktop unavailable\n"); return 1; }
    Writer writer;
    std::thread consumer([&]{ writer.run(); });
    const auto begin=Clock::now();
    long n=0;
    double captureSec=0, waitSec=0;
    bool ok=true;
    while(!frames || n<frames) {
        auto start=Clock::now();
        CaptureFrame f{};
        if(!captureK3(display,f)) {ok=false;break;}
        auto captured=Clock::now();
        if(!n) std::fprintf(stderr,"overlapped source=%s stride=%d fifo_slots=1\n",
            f.blueFirst?"bgra":(f.bytesPerPixel==3?"rgb24":"rgba"),f.stride);
        if(!writer.publish(std::move(f))) {ok=false;break;}
        auto queued=Clock::now();
        captureSec+=Seconds(captured-start).count();
        waitSec+=Seconds(queued-captured).count();
        if(++n%120==0) std::fprintf(stderr,"capture_frames=%ld elapsed=%.3f screenshot_mean_ms=%.3f wait_queue_mean_ms=%.3f\n",
            n,Seconds(queued-begin).count(),captureSec*1000/n,waitSec*1000/n);
        if(fps && n!=frames) std::this_thread::sleep_until(start+std::chrono::nanoseconds(1000000000/fps));
    }
    writer.finish();
    consumer.join();
    std::fprintf(stderr,"writer_frames=%ld elapsed=%.3f copy_write_ms=%.3f\n",writer.frames,
        Seconds(Clock::now()-begin).count(),writer.frames?writer.totalWriteSeconds*1000/writer.frames:0);
    return !ok || writer.failed || writer.frames!=n;
}
