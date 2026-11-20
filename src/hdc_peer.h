#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hdc_peer_detail {
using Clock = std::chrono::steady_clock;
using Directory = std::unique_ptr<DIR, decltype(&closedir)>;

inline bool IsNumber(const char* text)
{
    return *text && text[std::strspn(text, "0123456789")] == '\0';
}

// /proc/net/tcp{,6} renders each native 32-bit address word as hex.
// The preview listener is IPv4, but its peer may use an IPv4-mapped socket.
inline void FormatEndpoint(const sockaddr_in& endpoint, bool mapped, char (&out)[40])
{
    if (mapped) {
        std::snprintf(out, sizeof(out), "%08X%08X%08X%08X:%04X",
            0U, 0U, unsigned(htonl(0xffff)), unsigned(endpoint.sin_addr.s_addr),
            unsigned(ntohs(endpoint.sin_port)));
    } else {
        std::snprintf(out, sizeof(out), "%08X:%04X", unsigned(endpoint.sin_addr.s_addr),
            unsigned(ntohs(endpoint.sin_port)));
    }
}

inline unsigned long long FindPeerInode(const sockaddr_in& peer, const sockaddr_in& local,
    bool mapped, Clock::time_point deadline)
{
    char remoteEnd[40], localEnd[40];
    FormatEndpoint(peer, mapped, remoteEnd);
    FormatEndpoint(local, mapped, localEnd);
    std::unique_ptr<FILE, decltype(&std::fclose)> table(
        std::fopen(mapped ? "/proc/net/tcp6" : "/proc/net/tcp", "r"), &std::fclose);
    if (!table) return 0;

    char row[768];
    for (int count = 0; count < 8192 && Clock::now() < deadline &&
            std::fgets(row, sizeof(row), table.get()); ++count) {
        char source[40], destination[40], state[4];
        unsigned long long inode = 0;
        if (std::sscanf(row, "%*s %39s %39s %3s %*s %*s %*s %*s %*s %llu",
                source, destination, state, &inode) == 4 &&
            std::strcmp(state, "01") == 0 && std::strcmp(source, remoteEnd) == 0 &&
            std::strcmp(destination, localEnd) == 0) return inode;
    }
    return 0;
}

inline bool IsTrustedExecutable(int process, const struct stat& expected)
{
    struct stat actual{};
    return fstatat(process, "exe", &actual, 0) == 0 &&
        actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino;
}

// On this K3 image init launches hdcd as root. An app executing that same
// binary with an inherited TCP socket must not acquire control.
inline bool IsSystemDaemon(int process)
{
    int fd = openat(process, "status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::unique_ptr<FILE, decltype(&std::fclose)> status(fdopen(fd, "r"), &std::fclose);
    if (!status) {
        close(fd);
        return false;
    }
    bool fromInit = false, root = false;
    char line[256];
    while (std::fgets(line, sizeof(line), status.get())) {
        unsigned int parent, real, effective, saved, fs;
        if (std::sscanf(line, "PPid: %u", &parent) == 1) fromInit = parent == 1;
        if (std::sscanf(line, "Uid: %u %u %u %u", &real, &effective, &saved, &fs) == 4)
            root = real == 0 && effective == 0 && saved == 0 && fs == 0;
        if (root && fromInit) return true;
    }
    return false;
}

inline bool DaemonOwnsSocket(int process, const struct stat& trusted,
    unsigned long long socketInode, Clock::time_point deadline)
{
    if (!IsTrustedExecutable(process, trusted) || !IsSystemDaemon(process)) return false;
    int fd = openat(process, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    Directory entries(fdopendir(fd), &closedir);
    if (!entries) {
        close(fd);
        return false;
    }
    for (int count = 0; count < 8192 && Clock::now() < deadline; ++count) {
        const auto* entry = readdir(entries.get());
        if (!entry) break;
        if (!IsNumber(entry->d_name)) continue;
        struct stat st{};
        if (fstatat(fd, entry->d_name, &st, 0) == 0 && S_ISSOCK(st.st_mode) &&
            static_cast<unsigned long long>(st.st_ino) == socketInode)
            return IsTrustedExecutable(process, trusted);
    }
    return false;
}
} // namespace hdc_peer_detail

// Call once for an accepted connection. False allows preview, never control.
inline bool IsHdcdForwardedPeer(int acceptedFd)
{
    using namespace hdc_peer_detail;
    sockaddr_in peer{}, local{};
    socklen_t peerSize = sizeof(peer), localSize = sizeof(local);
    int type;
    socklen_t typeSize = sizeof(type);
    if (getsockopt(acceptedFd, SOL_SOCKET, SO_TYPE, &type, &typeSize) != 0 || type != SOCK_STREAM ||
        getpeername(acceptedFd, reinterpret_cast<sockaddr*>(&peer), &peerSize) != 0 ||
        getsockname(acceptedFd, reinterpret_cast<sockaddr*>(&local), &localSize) != 0 ||
        peerSize != sizeof(peer) || localSize != sizeof(local) ||
        peer.sin_family != AF_INET || local.sin_family != AF_INET ||
        (ntohl(peer.sin_addr.s_addr) >> 24) != 127 || (ntohl(local.sin_addr.s_addr) >> 24) != 127)
        return false;

    const auto deadline = Clock::now() + std::chrono::milliseconds(250);
    auto inode = FindPeerInode(peer, local, false, deadline);
    if (inode == 0) inode = FindPeerInode(peer, local, true, deadline);
    if (inode == 0) return false;

    struct stat trusted{};
    if (stat("/system/bin/hdcd", &trusted) != 0 || !S_ISREG(trusted.st_mode) ||
        trusted.st_uid != 0 || (trusted.st_mode & (S_IWGRP | S_IWOTH))) return false;
    Directory proc(opendir("/proc"), &closedir);
    if (!proc) return false;
    for (int count = 0; count < 8192 && Clock::now() < deadline; ++count) {
        const auto* entry = readdir(proc.get());
        if (!entry) break;
        if (!IsNumber(entry->d_name)) continue;
        int process = openat(dirfd(proc.get()), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (process < 0) continue;
        bool ownsSocket = DaemonOwnsSocket(process, trusted, inode, deadline);
        close(process);
        if (ownsSocket) return true;
    }
    return false;
}
