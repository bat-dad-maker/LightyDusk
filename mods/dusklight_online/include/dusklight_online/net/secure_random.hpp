#pragma once

#include <cstdint>
#include <limits>
#include <span>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dusklight_online::net {

inline bool secure_random_bytes(std::span<uint8_t> bytes) {
#if defined(_WIN32)
    return bytes.size() <= std::numeric_limits<ULONG>::max() &&
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t n = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (n > 0) offset += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    ::close(fd);
    return offset == bytes.size();
#endif
}

} // namespace dusklight_online::net
