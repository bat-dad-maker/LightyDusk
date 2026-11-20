// Guest window placement on Android (docs/las/ANDROID-PORT.md, "Dynamic memory window"): the game's memory goes in
// a free hole of the process's address space chosen at startup from /proc/self/maps, instead of the desktop's fixed
// 0x70_0000_0000. Guest addresses stay host addresses; only where the window starts changes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SIZE      0x320000000ull   // GUEST_HI - GUEST_LO (runtime/rt.h): 12.5 GB
#define GW_HEAP_OFF  0x240000000ull   // HEAP_BASE - GUEST_LO
#define GW_VA_LIMIT  (1ull << 39)     // the game was built for a 39-bit address space
#define GW_FLOOR     (1ull << 32)     // ART keeps its heap and boot image in the low 4 GB
#define GW_ALIGN     (1ull << 32)

typedef struct { uint64_t lo, hi; } GwRange;

// The process's mappings in address order, adjacent ones merged. Returns the count, -1 on error (or more than max).
int gw_read_maps(GwRange *out, int max);
// Free holes of at least min_size inside [floor, ceil), from mappings read by gw_read_maps. Returns the count.
int gw_holes(const GwRange *maps, int n, uint64_t floor, uint64_t ceil, uint64_t min_size, GwRange *out, int max);
// Reserves size bytes (read / write, not committed) at the lowest align-aligned address of a hole in [floor, ceil).
// Returns the base, 0 if there is no room. Retries when another thread maps into the chosen hole first.
uint64_t gw_reserve(uint64_t size, uint64_t align, uint64_t floor, uint64_t ceil);

#ifdef __cplusplus
}
#endif
