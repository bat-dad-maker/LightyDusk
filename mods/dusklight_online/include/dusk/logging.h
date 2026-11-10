#pragma once

// The in-tree actor uses Dusklight's internal Borealis logger for diagnostics.
// Mods intentionally do not link that private dependency, so keep those calls
// source-compatible and side-effect free inside the portable package.
struct DusklightOnlineActorLog {
    template <typename... Args>
    constexpr void info(const char*, Args&&...) const {}

    template <typename... Args>
    constexpr void warn(const char*, Args&&...) const {}
};

inline constexpr DusklightOnlineActorLog DuskLog{};
