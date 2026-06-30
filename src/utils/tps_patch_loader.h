/**
 * @file tps_patch_loader.h
 * @brief Push the TPS26750 configuration patch bundle directly from the RP2040
 *        over I2C0 at boot, instead of relying on the external EEPROM.
 *
 * The TPS26750 normally reads its patch bundle from the CAT24C512 EEPROM (0x50)
 * on its own I2Cc bus and self-transitions to 'APP ' mode. This module lets the
 * RP2040 push the SAME firmware straight into the controller using the TI
 * burst-mode patch tasks (PBMs -> burst write -> PBMc), so a future board can
 * drop the EEPROM entirely.
 *
 * Burst mode streams the "low region" (patch bundle + application config) that
 * lives inside the full-flash EEPROM image (`tps25750x_fullFlash_i2c_array`).
 * The low region is located and sized from the image header at runtime, so the
 * EEPROM and burst paths always deliver byte-identical firmware and config from
 * one embedded source - no separately generated bundle to drift out of sync.
 *
 * Control-plane registers (CMD1/DATA1/INT_EVENT1/MODE) are accessed through the
 * shared TPS26750 driver (`hw.pdController`). The raw patch burst is written to a
 * temporary patch target address on I2C0 with the Pico SDK directly.
 *
 * Reference: TPS26750 Technical Reference Manual (SLVUCR7) sections 5.4 and 6.2.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "pico/stdlib.h"

// =============================================================================
// Configuration
// =============================================================================

// Temporary 7-bit I2C target address used for the patch burst download. Set in
// the PBMs DATA1 input (byte 5) and used to address the raw burst bytes on I2C0.
// MUST NOT collide with any other device on I2C0: TPS26750 (0x21), INA228
// (0x40), or the reserved 0x00. The TPS responds on this address only during
// the burst download phase.
#define TPS_PATCH_TARGET_ADDR   0x0F

// Burst data bytes written per step() call. A smaller chunk keeps the boot
// loading bar smooth; the TPS auto-increments its patch pointer per byte.
#define TPS_PATCH_BURST_CHUNK   256

// =============================================================================
// Types
// =============================================================================

enum class TpsPatchStatus : uint8_t {
    IN_PROGRESS,
    SUCCESS,   // TPS is in 'APP ' mode (pushed by us, or it was already APP -> skipped)
    ERROR
};

enum class TpsPatchPhase : uint8_t {
    CHECK_MODE,  // Read MODE: APP -> skip, PTCH -> proceed, else wait
    WAIT_READY,  // Wait for INT_EVENT1.ReadyForPatch
    SEND_PBMS,   // Start burst download sequence
    BURST,       // Stream the bundle to the patch target address
    SEND_PBMC,   // Complete: CRC check + apply, TPS switches to APP
    WAIT_APP,    // Confirm MODE == 'APP '
    DONE,
    FAILED
};

struct TpsPatchSession {
    const uint8_t* data;
    size_t size;
    size_t offset;
    TpsPatchPhase phase;
    bool skipped;                 // true when the TPS was already in 'APP ' (no push needed)
    const char* error_message;
    absolute_time_t phase_deadline;
    uint32_t start_ms;
    uint32_t elapsed_ms;          // total push duration, valid once DONE/FAILED
};

// =============================================================================
// Public API (non-blocking session/step, mirrors tps_eeprom_loader)
// =============================================================================

/**
 * @brief Begin a patch-push session using the embedded firmware bundle.
 * @param session Caller-owned session storage.
 * @return true if the session is ready to step, false on setup failure.
 */
bool tpsPatchBegin(TpsPatchSession* session);

/**
 * @brief Advance the push by one unit of work (one mode poll or one burst chunk).
 * @param session Session from tpsPatchBegin.
 * @return Current status. Poll tpsPatchProgress() for the loading-bar percentage.
 */
TpsPatchStatus tpsPatchStep(TpsPatchSession* session);

/**
 * @brief Current progress (0-100) for the loading bar (burst-dominated).
 */
uint8_t tpsPatchProgress(const TpsPatchSession* session);
