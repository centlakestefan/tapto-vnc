#pragma once

// Platform abstraction layer - implementation details
// NOTE: This file intentionally contains platform-specific #ifdef code
// as this is a cross-platform networking library that requires direct
// socket access. The #ifdef blocks are isolated here to minimize
// platform-specific code throughout the codebase.

#include "rfb_platform.hpp"
#include <cerrno>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    // Windows socket compatibility macros
    #define SHUT_RDWR SD_BOTH
    #define poll WSAPoll
    // Note: POLLIN is already defined in winsock2.h
    using socklen_t = int;

    // Windows socket error handling functions
    inline int get_socket_error() { return WSAGetLastError(); }
    inline bool is_eintr() { return WSAGetLastError() == WSAEINTR; }
    inline bool is_ewouldblock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
    inline const char* get_error_string() {
        static thread_local char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      nullptr, WSAGetLastError(), 0, buf, sizeof(buf), nullptr);
        return buf;
    }

    // Winsock initialization helper
    struct WinsockInit {
        WinsockInit() {
            WSADATA wsa_data;
            WSAStartup(MAKEWORD(2, 2), &wsa_data);
        }
        ~WinsockInit() {
            WSACleanup();
        }
    };
    static WinsockInit winsock_init;

    // Platform-specific socket operations
    inline int platform_setsockopt(socket_t fd, int level, int optname, const int* value) {
        return setsockopt(fd, level, optname, reinterpret_cast<const char*>(value), sizeof(int));
    }

    inline int platform_send(socket_t fd, const unsigned char* data, size_t size) {
        return ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
    }

    inline int platform_recv(socket_t fd, unsigned char* data, size_t size) {
        return ::recv(fd, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
    }

    inline int platform_ioctlsocket(socket_t fd, bool blocking) {
        u_long mode = blocking ? 0 : 1;
        return ioctlsocket(fd, FIONBIO, &mode);
    }

#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <poll.h>

    // Unix socket error handling functions
    inline int get_socket_error() { return errno; }
    inline bool is_eintr() { return errno == EINTR; }
    inline bool is_ewouldblock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
    inline const char* get_error_string() { return strerror(errno); }

    // Unix socket close wrapper
    inline void closesocket(int fd) { ::close(fd); }

    // Platform-specific socket operations
    inline int platform_setsockopt(socket_t fd, int level, int optname, const int* value) {
        return setsockopt(fd, level, optname, value, sizeof(int));
    }

    inline ssize_t platform_send(socket_t fd, const unsigned char* data, size_t size) {
        return ::send(fd, data, size, 0);
    }

    inline ssize_t platform_recv(socket_t fd, unsigned char* data, size_t size) {
        return ::recv(fd, data, size, 0);
    }

    inline int platform_fcntl_setfl(socket_t fd, bool blocking) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return -1;
        }
        if (blocking) {
            flags &= ~O_NONBLOCK;
        } else {
            flags |= O_NONBLOCK;
        }
        return fcntl(fd, F_SETFL, flags);
    }
#endif

