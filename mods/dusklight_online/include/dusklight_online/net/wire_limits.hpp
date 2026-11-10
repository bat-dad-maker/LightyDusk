#pragma once

#include <cstddef>

namespace dusklight_online::net {
// Keep encrypted ICE datagrams within libjuice's 1200-byte packet limit.
inline constexpr size_t kIceDatagramBytes = 1200;
inline constexpr size_t kSealedDatagramOverhead = 4 + 8 + 8 + 16;
inline constexpr size_t kReliableDatagramBytes =
    kIceDatagramBytes - kSealedDatagramOverhead;
}
