#pragma once

#include <cstdint>

// ============================================================================
// Settings value types (pure, hardware-free)
// ============================================================================
// Flash-facing enums and their small helpers, split out of settings.h so that
// pure logic (e.g. logic/pd_diagnostics.h) can depend on these types without
// pulling in the Pico-SDK-coupled Settings class. This header includes only
// <cstdint> and is therefore includable from a host unit-test build.
// ============================================================================

// Startup contract type persisted to flash (which kind of PDO was last used).
enum class SavedStartupContractType : uint8_t {
    NONE = 0,
    UNKNOWN = 1,
    FIXED = 2,
    PPS = 3,
    AVS = 4,
};

// Startup contract negotiation modes
enum class StartupContractMode : uint8_t {
    LOWEST_VOLTAGE = 0,   // Negotiate lowest voltage available
    HIGHEST_VOLTAGE = 1,  // Negotiate highest voltage available
    LAST_USED = 2         // Restore last used contract (closest if unavailable)
};

// Current limit operating modes
enum class CurrentLimitMode : uint8_t {
    OFF = 0,
    OCP = 1,
    CC = 2,
};

// Shared CurrentLimitMode helpers, defined once next to the enum. These were
// previously copy-pasted into state_machine.cpp, cc_controller.cpp, and
// settings.cpp; keeping a single inline definition ends that drift.
inline const char* currentLimitModeName(CurrentLimitMode mode) {
    switch (mode) {
        case CurrentLimitMode::OFF: return "OFF";
        case CurrentLimitMode::OCP: return "OCP";
        case CurrentLimitMode::CC:  return "CC";
    }
    return "OCP";
}

// Clamp a raw persisted byte to a valid mode (falls back to OCP for garbage).
inline CurrentLimitMode normalizeCurrentLimitMode(uint8_t raw) {
    switch (static_cast<CurrentLimitMode>(raw)) {
        case CurrentLimitMode::OFF:
        case CurrentLimitMode::OCP:
        case CurrentLimitMode::CC:
            return static_cast<CurrentLimitMode>(raw);
    }
    return CurrentLimitMode::OCP;
}

inline CurrentLimitMode normalizeCurrentLimitMode(CurrentLimitMode mode) {
    return normalizeCurrentLimitMode(static_cast<uint8_t>(mode));
}

// Cycle order for the BTN2 mode toggle on the Current Limit screen.
inline CurrentLimitMode nextCurrentLimitMode(CurrentLimitMode mode) {
    switch (mode) {
        case CurrentLimitMode::OCP: return CurrentLimitMode::CC;
        case CurrentLimitMode::CC:  return CurrentLimitMode::OFF;
        case CurrentLimitMode::OFF: return CurrentLimitMode::OCP;
    }
    return CurrentLimitMode::OCP;
}
