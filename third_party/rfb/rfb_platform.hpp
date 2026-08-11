#pragma once

// Platform abstraction layer for socket types and constants
// NOTE: This file intentionally contains platform-specific #ifdef code
// as this is a cross-platform networking library that requires direct
// socket access. The #ifdef blocks are isolated here to minimize
// platform-specific code throughout the codebase.

#ifdef _WIN32
    #include <winsock2.h>
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
#endif

