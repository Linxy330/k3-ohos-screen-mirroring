#include "screen_geometry.h"
#include "hdc_peer.h"
#include "frame_rate.h"
#ifdef WITH_LOCAL_INPUT
#include <thread>
#include <deque>
#include <spawn.h>
#include "screenshot.h"
#include "touch_backend.h"
#include "input_commands.h"
#endif
#ifdef WITH_OH_CODEC
#include "screenshot.h"
#include "codec.h"
#include <atomic>
#include <thread>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr size_t maxFrame = 8u<<20;
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
uint32_t get32(const uint8_t *p) { uint32_t n; memcpy(&n,p,4); return ntohl(n); }
uint32_t get24(const uint8_t *p) { return uint32_t(p[0])<<16 | uint32_t(p[1])<<8 | p[2]; }
void put32(uint8_t *p,uint32_t n){ n=htonl(n); memcpy(p,&n,4); }
#ifdef WITH_LOCAL_INPUT
uint32_t read32(const uint8_t *p) { return get32(p); }
#include "input.hpp"
#endif
bool xfer(int fd, void *p, size_t n, bool writing) {
    auto *b=static_cast<uint8_t*>(p);
    const auto stopAt=Clock::now()+3s;
    while(n && !stopping && Clock::now()<stopAt) {
        pollfd f{fd,static_cast<short>(writing ? POLLOUT:POLLIN),0};
        int r=poll(&f,1,200);
        if(r<0 && errno==EINTR) continue;
        if(r<0 || (r && !(f.revents & f.events))) return false;
        if(!r) continue;
        ssize_t k=writing ? send(fd,b,n,MSG_DONTWAIT|MSG_NOSIGNAL) : recv(fd,b,n,MSG_DONTWAIT);
        if(k<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if(k<=0) return false;
        b+=k; n-=k;
    }
    return n==0;
}
bool packet(int fd,uint32_t t,Bytes &b) {
    uint8_t hdr[8]; put32(hdr,t); put32(hdr+4,b.size());
    return xfer(fd,hdr,8,true) && xfer(fd,b.data(),b.size(),true);
}
bool readPacket(int fd,uint32_t &t,Bytes &b,size_t limit) {
    uint8_t hdr[8];
    if(!xfer(fd,hdr,8,false) || get32(hdr+4)>limit) return false;
    t=get32(hdr); b.resize(get32(hdr+4));
    return xfer(fd,b.data(),b.size(),false);
}
Bytes config(bool avc,int fps,const Bytes &sps={},const Bytes &pps={}) {
    Bytes b(13); b[0]=avc ? 0:2;
    put32(b.data()+1,k3::screen::width); put32(b.data()+5,k3::screen::height); put32(b.data()+9,fps);
    if(avc) for(const auto *n:{&sps,&pps}) {
        b.push_back(n->size()>>8); b.push_back(n->size()&255);
        b.insert(b.end(),n->begin(),n->end());
    }
    return b;
}
bool display(int fd) { Bytes b(10); b[0]=1; put32(b.data()+1,k3::screen::width); put32(b.data()+5,k3::screen::height); return packet(fd,0x20,b); }
bool video(int fd, bool key, const Bytes &bytes) {
    Bytes msg(9); msg.reserve(bytes.size()+9); msg[0]=key;
    uint64_t us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    put32(msg.data()+1,us>>32); put32(msg.data()+5,us);
    msg.insert(msg.end(),bytes.begin(),bytes.end());
    return packet(fd,3,msg);
}
bool readBytes(FILE *f,void *p,size_t n) { return !n || fread(p,1,n,f)==n; }
struct Avc {
    int channel, fps;
    int lenSize=4;
    Bytes sps,pps;
    bool sent=false;
    void parameter(const uint8_t *p,size_t len) {
        if(!len || len>65535) return;
        size_t kind=p[0]&31;
        if(kind != 7 && kind != 8) return;
        Bytes &cur=kind==7 ? sps:pps;
        if(cur.size()!=len || memcmp(cur.data(),p,len)!=0) { cur.assign(p,p+len); sent=false; }
    }
    bool configure() {
        if(!sent && !sps.empty() && !pps.empty()) {
            auto b=config(true,fps,sps,pps); if(!packet(channel,2,b)) return false;
            sent=true;
        }
        return true;
    }
    bool parseConfig(const uint8_t *p,size_t size) {
        if(size<7 || *p!=1) return true; // Some vendor encoders send their SPS/PPS in band.
        lenSize=(p[4]&3)+1;
        size_t pos=6, count=p[5]&31;
        for(int list=0;list<2;++list) {
            if(list) { if(pos>=size) return false; count=p[pos++]; }
            for(size_t i=0;i<count;++i) {
                if(pos+2>size) return false;
                size_t n=(size_t(p[pos])<<8)|p[pos+1]; pos+=2;
                if(!n || pos+n>size) return false;
                parameter(p+pos,n); pos+=n;
            }
        }
        return configure();
    }
    bool sample(const uint8_t *p,size_t size,bool key) {
        Bytes annex;
        bool picture=false;
        auto append=[&](const uint8_t *n,size_t count){
            int type=n[0]&31;
            parameter(n,count);
            if(type==7 || type==8) return;
            if(type==5) key=true;
            if(type>=1 && type<=5) picture=true;
            annex.insert(annex.end(),{0,0,0,1}); annex.insert(annex.end(),n,n+count);
        };
        // FLV AVC samples are length-prefixed. Try that before any start-code
        // sniff, since a 0x000001xx length resembles a three-byte start code.
        size_t pos=0;
        auto length=[&](size_t off){size_t v=0; for(int j=0;j<lenSize;++j) v=(v<<8)|p[off+j]; return v;};
        while(pos+lenSize<=size) { size_t n=length(pos); if(!n||n>size-pos-lenSize) break; pos+=lenSize+n; }
        if(pos==size && pos) {
            for(pos=0;pos<size;) { size_t n=length(pos); pos+=lenSize; append(p+pos,n); pos+=n; }
        } else {
            auto prefix=[&](size_t q)->size_t{
                if(q+3<=size && !p[q]&&!p[q+1]&&p[q+2]==1) return 3;
                if(q+4<=size && !p[q]&&!p[q+1]&&!p[q+2]&&p[q+3]==1) return 4;
                return 0;
            };
            pos=0; if(!prefix(0)) { fprintf(stderr,"unexpected AVC sample framing\n"); return false; }
            while(pos<size) {
                size_t start=pos+prefix(pos),next=start;
                while(next<size && !prefix(next)) ++next;
                if(next>start) append(p+start,next-start);
                pos=next;
            }
        }
        return configure() && (!picture || !sent || video(channel,key,annex));
    }
};

bool flv(FILE *f,int fd,int fps) {
    uint8_t h[9];
    if(!readBytes(f,h,9) || memcmp(h,"FLV",3)!=0 || get32(h+5)!=9) {
        fprintf(stderr,"invalid or absent FLV header\n"); return false;
    }
    Bytes data;
    Avc a{fd,fps};
    for(;;) {
        uint8_t tag[15];
        if(!readBytes(f,tag,sizeof(tag))) return false;
        size_t n=get24(tag+5);
        if(n>maxFrame) return false;
        data.resize(n); if(!readBytes(f,data.data(),n)) return false;
        if((tag[4]&31)!=9 || n<5 || (data[0]&15)!=7) continue;
        if(data[1]==0) { if(!a.parseConfig(data.data()+5,n-5)) return false; }
        else if(data[1]==1 && !a.sample(data.data()+5,n-5,(data[0]>>4)==1)) return false;
    }
}
bool multipart(FILE *f,int fd) {
    char line[512]; Bytes img;
    for(;;) {
        long len=0; bool hadHeaders=false;
        while(fgets(line,sizeof(line),f)) {
            if(!strchr(line,'\n')) return false;
            if(strncasecmp(line,"Content-length:",15)==0) {
                char *end; len=strtol(line+15,&end,10);
                if(len<4 || len>static_cast<long>(maxFrame)) return false;
                hadHeaders=true;
            }
            if(hadHeaders && (strcmp(line,"\r\n")==0 || strcmp(line,"\n")==0)) break;
        }
        if(!hadHeaders || feof(f)) return false;
        img.resize(len);
        if(!readBytes(f,img.data(),len) || img[0]!=255 || img[1]!=216 || img[len-2]!=255 || img[len-1]!=217) return false;
        if(!video(fd,true,img)) return false;
    }
}

struct Options {
    int port;
    k3::FrameRate rate;
    bool avc, native;
    std::string capture, format;
};
#ifdef WITH_OH_CODEC
void nativeStream(int fd,k3::FrameRate rate) {
    uint64_t displayId;
    if(!getK3Desktop(displayId)) return;
    struct Sink { Avc avc; std::atomic_bool alive{true}; };
    Sink sink{{fd,rate.nominal()}};
    auto onOutput=[](const uint8_t *data,int size,uint32_t,int64_t,void *ctx) {
        auto &s=*static_cast<Sink*>(ctx);
        if(!s.avc.sample(data,size,false)) s.alive.store(false);
    };
    auto *codec=hw_open(k3::screen::width,k3::screen::height,rate.nominal(),0,onOutput,&sink);
    if(!codec) return;
    int count=0;
    double captureSec=0, submitSec=0;
    auto began=Clock::now();
    while(!stopping && sink.alive.load()) {
        auto start=Clock::now();
        CaptureFrame f{};
        if(!captureK3(displayId,f)) break;
        auto sampled=Clock::now();
        uint8_t *y,*uv;
        int stride;
        if(hw_input(codec,&y,&uv,&stride,1500)) break;
        convertNV12(f,y,uv,stride);
        auto pts=std::chrono::duration_cast<std::chrono::microseconds>(start.time_since_epoch()).count();
        if(hw_submit(codec,pts)) break;
        auto done=Clock::now();
        captureSec+=std::chrono::duration<double>(sampled-start).count();
        submitSec+=std::chrono::duration<double>(done-sampled).count();
        if(++count%120==0) fprintf(stderr,"native frames=%d elapsed=%.3f screenshot_ms=%.2f convert_submit_ms=%.2f\n",
            count,std::chrono::duration<double>(done-began).count(),1000*captureSec/count,1000*submitSec/count);
        if (rate.limit) std::this_thread::sleep_until(start+std::chrono::nanoseconds(1000000000/rate.limit));
    }
    hw_close(codec);
}
#endif
void captureChild(const Options &o,int stream) {
#ifdef WITH_OH_CODEC
    if(o.native) { nativeStream(stream,o.rate); _exit(0); }
#endif
    int raw[2],encoded[2];
    if(pipe2(raw,O_CLOEXEC)<0 || pipe2(encoded,O_CLOEXEC)<0) _exit(1);
    pid_t parent=getpid();
    auto spawn=[&](bool snapshot)->pid_t {
        pid_t child=fork(); if(child!=0) return child;
        prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()!=parent) _exit(1);
        signal(SIGPIPE,SIG_DFL);
        if(snapshot) {
            std::string limit=std::to_string(o.rate.limit);
            if(dup2(raw[1],1)<0) _exit(1);
            execl(o.capture.c_str(),o.capture.c_str(),"0",limit.c_str(),o.format.c_str(),nullptr);
        } else {
            std::string nominal=std::to_string(o.rate.nominal());
            if(dup2(raw[0],0)<0 || dup2(encoded[1],1)<0) _exit(1);
#ifdef K3_SOFTWARE_ONLY
            // Slice workers encode the current frame in parallel, even on a still
            // desktop where a frame-threaded encoder could wait for more input.
            const std::vector<const char*> args={
                "/vendor/bin/ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                "-f", "rawvideo", "-pixel_format", o.format.c_str(), "-video_size", k3::screen::ffmpegSize,
                "-framerate", nominal.c_str(), "-probesize", "32", "-fpsprobesize", "0", "-i", "pipe:0",
                "-an", "-fps_mode", "passthrough", "-c:v", "mjpeg", "-pix_fmt", "yuvj420p",
                "-thread_type", "slice", "-threads", "0", "-huffman", "default", "-q:v", "4",
                "-flush_packets", "1", "-f", "mpjpeg", "pipe:1", nullptr};
#else
            std::vector<const char*> args={"/vendor/bin/ffmpeg","-nostdin","-hide_banner","-loglevel","warning",
                "-f","rawvideo","-pixel_format",o.format.c_str(),"-video_size",k3::screen::ffmpegSize,"-framerate",nominal.c_str(),
                "-i","pipe:0","-an","-fps_mode","passthrough","-c:v",o.avc?"h264_stcodec":"mjpeg_stcodec","-pix_fmt",o.format.c_str(),
                "-b:v","6M","-g","30","-bf","0","-flush_packets","1"};
            if(o.avc) args.insert(args.end(),{"-flvflags","no_duration_filesize"});
            args.insert(args.end(),{"-f",o.avc?"flv":"mpjpeg","pipe:1",nullptr});
#endif
            execv(args.front(),const_cast<char *const*>(args.data()));
        }
        perror("encoder child exec"); _exit(127);
    };
    auto capt=spawn(true); auto enc=capt>0 ? spawn(false) : -1;
    close(raw[0]); close(raw[1]); close(encoded[1]);
    if(capt>0 && enc>0) {
        FILE *f=fdopen(encoded[0],"r"); if(!f) _exit(1);
        if(o.avc) flv(f,stream,o.rate.nominal()); else multipart(f,stream);
        fclose(f);
    }
    for(pid_t p:{capt,enc}) if(p>0) {kill(p,SIGKILL); while(waitpid(p,nullptr,0)<0 && errno==EINTR){}}
    _exit(0);
}

void session(int cli,const Options &o) {
    if(!IsHdcdForwardedPeer(cli)) { fprintf(stderr,"rejected non-HDC peer\n"); return; }
    int channel[2];
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,channel)<0) return;
    pid_t parent=getpid();
    pid_t child=fork();
    if(child==0) {
        signal(SIGTERM,SIG_DFL); prctl(PR_SET_PDEATHSIG,SIGKILL); if(getppid()!=parent) _exit(1);
        close(cli); close(channel[0]); captureChild(o,channel[1]);
    }
    close(channel[1]);
    if(child<0) {close(channel[0]);return;}
    Bytes cfg, data, emptyList{0x10,0,0};
    bool running=display(cli);
    if(!o.avc) { cfg=config(false,o.rate.nominal()); running=running && packet(cli,2,cfg); }
    auto heartbeat=Clock::now();
#ifdef WITH_LOCAL_INPUT
    TouchGesture touch;
    k3::control::InputCommands commands;
#endif
    while(running && !stopping && Clock::now()-heartbeat<30s) {
#ifdef WITH_LOCAL_INPUT
        commands.update();
#endif
        pollfd f[2]{{cli,POLLIN,0},{channel[0],POLLIN,0}};
        int r=poll(f,2,500);
        if(r<0 && errno==EINTR) continue;
        if(r<0) break;
        uint32_t t=0;
        if(f[0].revents & POLLIN) {
            if(!readPacket(cli,t,data,65536)) break;
            if(t==1) { if(data.size()!=8||!packet(cli,1,data)) break; heartbeat=Clock::now(); }
#ifdef WITH_LOCAL_INPUT
            if(t==0x10 && data.size()==11 && data[0]>=1 && data[0]<=3) touch.handle(data);
            if(t==0x10) commands.push(data);
#endif
            if(t==0x10 && data.size()==1 && data[0]==0x31 && !display(cli)) break;
            if(t==0x10 && data.size()==1 && data[0]==0x30 && !packet(cli,0x20,emptyList)) break;
            if(t==0x10 && data.size()==13 && data[0]==0x42 && !cfg.empty() && !packet(cli,2,cfg)) break;
        }
        if(f[0].revents & (POLLHUP|POLLERR|POLLNVAL)) break;
        if(f[1].revents & POLLIN) {
            if(!readPacket(channel[0],t,data,maxFrame+9) || (t!=2&&t!=3) || !packet(cli,t,data)) break;
            if(t==2) cfg=data;
        } else if(f[1].revents & (POLLHUP|POLLERR|POLLNVAL)) break;
    }
    close(channel[0]); kill(child,SIGTERM);
    while(waitpid(child,nullptr,0)<0 && errno==EINTR) {}
}
}
int main(int argc,char **argv) {
#ifdef WITH_LOCAL_INPUT
    if (argc == 2 && !std::strcmp(argv[1], "--insert-focused-text")) return k3::control::insertTextFromStdin();
#endif
#ifdef K3_SOFTWARE_ONLY
    if (argc != 6 || strcmp(argv[3], "mjpeg") || strcmp(argv[5], "rgba")) {
        fprintf(stderr, "Usage: k3-sw-wire PORT auto|MAX_FPS mjpeg CAPTURE rgba\n");
        return 2;
    }
#else
    if(argc<4 || argc>6) {fprintf(stderr,"hw-wire PORT auto|MAX_FPS h264|oh-h264|mjpeg [CAPTURE [rgba|nv12]]\n");return 2;}
#endif
    bool native=false;
#ifdef WITH_OH_CODEC
    native=!strcmp(argv[3],"oh-h264");
#endif
    k3::FrameRate rate;
    if(!k3::FrameRate::parse(argv[2],rate)) {
        fprintf(stderr,"capture rate must be auto, 0 (unlimited), or an integer 1..120\n");
        return 2;
    }
    Options o{atoi(argv[1]),rate,native||!strcmp(argv[3],"h264"),native,argc>4?argv[4]:"./k3-capture-rgba",argc>5?argv[5]:"rgba"};
    if(o.port<1024 || o.port>65535 || (!o.avc&&strcmp(argv[3],"mjpeg")) ||
       (o.format!="rgba" && o.format!="nv12")) return 2;
    signal(SIGPIPE,SIG_IGN);
    struct sigaction sa{}; sa.sa_handler=stop; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM,&sa,nullptr); sigaction(SIGINT,&sa,nullptr);
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(o.port);
    int one=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0 || listen(listener,2)<0) {perror("listen");return 1;}
    fprintf(stderr,"USB listener on %d: %s, capture=%s, nominal encoder rate=%d fps\n",
            o.port,argv[3],o.rate.limit ? argv[2] : "unlimited",o.rate.nominal());
    while(!stopping) {
        pollfd f{listener,POLLIN,0}; if(poll(&f,1,500)<=0) continue;
        int client=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);
        if(client<0) continue;
        setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
#ifdef WITH_LOCAL_INPUT
        // Keep the acceptor free of platform client threads so reconnects fork safely.
        pid_t original=getpid(), child=fork();
        if(!child) {
            close(listener); prctl(PR_SET_PDEATHSIG,SIGTERM); if(getppid()!=original) _exit(1);
            session(client,o); close(client); _exit(0);
        }
        close(client);
        if(child>0) {
            while(!stopping && waitpid(child,nullptr,WNOHANG)==0) poll(nullptr,0,50);
            if(stopping) { kill(child,SIGTERM); while(waitpid(child,nullptr,0)<0 && errno==EINTR){} }
        }
#else
        session(client,o); close(client);
#endif
    }
    close(listener);
}
