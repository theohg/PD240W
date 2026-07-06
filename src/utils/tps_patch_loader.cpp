/**
 * @file tps_patch_loader.cpp
 * @brief Implementation of the RP2040 -> TPS26750 I2C patch-push.
 *
 * Uses I2C0 (shared with the TPS26750 @0x21 and INA228 @0x40). Control registers
 * are accessed via the shared driver (hw.pdController); the raw burst is written
 * to TPS_PATCH_TARGET_ADDR with i2c_write_blocking().
 */

#include "tps_patch_loader.h"
#include "hardware.h"
#include "hardware/i2c.h"
#include "tps26750.h"
#include "utils/logging.h"
#include <string.h>

// =============================================================================
// Embedded patch bundle (the same image the EEPROM is programmed with)
// =============================================================================

// PBMs burst mode streams the "low region" - the patch bundle plus application
// config - which lives inside the full-flash EEPROM image. Rather than embed a
// second, separately generated copy (which can drift and silently ship a wrong
// config, e.g. stripped PPS APDOs), the low region is located and sized from the
// image header at runtime. EEPROM and burst paths therefore deliver identical
// firmware + config from one source. TRM SLVUC05A: pbmsInData.lrBinSize = the
// low-region byte count.
extern "C" {
    extern const char tps25750x_fullFlash_i2c_array[];
    extern int gSizeFullFlashArray;
}

// =============================================================================
// TPS26750 host-interface details (TRM SLVUCR7)
// =============================================================================

namespace {

constexpr uint8_t  REG_CMD1  = 0x08;   // Command register for I2Ct
constexpr uint8_t  REG_DATA1 = 0x09;   // Data register for CMD1

// INT_EVENT1 bit 81 = "Ready for Patch" (byte 10, bit 1). Enabled by default.
constexpr uint8_t  INT_BIT_READY_FOR_PATCH = 81;

// 4CC tasks (written to CMD1).
const char* const CMD_PBMS = "PBMs";   // Start patch burst download sequence
const char* const CMD_PBMC = "PBMc";   // Patch burst download complete

// PBMs DATA1 input: byte 5 = burst-mode timeout (0x32 -> 5 s, LSB 100 ms).
constexpr uint8_t  PBMS_BURST_TIMEOUT = 0x32;

// Timeouts (ms).
constexpr uint32_t MODE_WAIT_TIMEOUT_MS  = 1500;  // wait for a readable PTCH/APP mode
constexpr uint32_t READY_WAIT_TIMEOUT_MS = 1000;  // wait for ReadyForPatch
constexpr uint32_t CMD_CLEAR_TIMEOUT_MS  = 3000;  // wait for a 4CC task to clear CMD1
constexpr uint32_t APP_WAIT_TIMEOUT_MS   = 2000;  // wait for MODE -> 'APP ' after PBMc

inline uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline bool modeIs(const char* mode, const char* want) {
    return mode[0] == want[0] && mode[1] == want[1] &&
           mode[2] == want[2] && mode[3] == want[3];
}

// Confirm a freshly-written 4CC actually latched into CMD1. The device echoes
// the command in CMD1 while it executes; if the write was dropped (e.g. lost to
// clock stretching after the DATA1 write) CMD1 stays 0 and we'd otherwise mistake
// that for instant completion. Returns true once CMD1 shows the command OR has
// already moved on to a completion state (0x00 / "!CMD"); false only if CMD1
// stays 0 for the whole window with no sign the task ran.
bool waitCommandLatched(const char* cmd4) {
    uint32_t start = nowMs();
    uint8_t cmd[4];
    while (nowMs() - start < 50) {
        if (hw.pdController.readRegister(REG_CMD1, cmd, 4)) {
            if (cmd[0] == (uint8_t)cmd4[0] && cmd[1] == (uint8_t)cmd4[1] &&
                cmd[2] == (uint8_t)cmd4[2] && cmd[3] == (uint8_t)cmd4[3]) {
                return true;  // command is executing
            }
            if (cmd[0] == '!') {
                return true;  // already reported a result; let the clear-poll decode it
            }
        }
        sleep_ms(1);  // don't hammer the I2C bus between polls (matches waitCommandClear)
    }
    return false;  // CMD1 never showed the command -> write was dropped
}

// Poll CMD1 until the task engine clears it (success) or replaces it with "!CMD"
// (unrecognized) / times out. Brief blocking wait used only at task boundaries.
bool waitCommandClear(uint32_t timeout_ms) {
    uint32_t start = nowMs();
    uint8_t cmd[4];
    while (true) {
        if (hw.pdController.readRegister(REG_CMD1, cmd, 4)) {
            if (cmd[0] == 0 && cmd[1] == 0 && cmd[2] == 0 && cmd[3] == 0) {
                return true;
            }
            if (cmd[0] == '!' && cmd[1] == 'C' && cmd[2] == 'M' && cmd[3] == 'D') {
                LOG_ERROR("[PATCH] Command rejected (!CMD)");
                return false;
            }
        }
        if (nowMs() - start >= timeout_ms) {
            return false;
        }
        sleep_ms(2);
    }
}

TpsPatchStatus fail(TpsPatchSession* s, const char* msg) {
    s->phase = TpsPatchPhase::FAILED;
    s->error_message = msg;
    s->elapsed_ms = nowMs() - s->start_ms;
    LOG_ERROR("[PATCH] %s", msg);
    return TpsPatchStatus::ERROR;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

bool tpsPatchBegin(TpsPatchSession* session) {
    if (!session) {
        return false;
    }

    *session = {};
    session->offset = 0;
    session->phase = TpsPatchPhase::CHECK_MODE;
    session->skipped = false;
    session->error_message = nullptr;
    session->start_ms = nowMs();
    session->phase_deadline = make_timeout_time_ms(MODE_WAIT_TIMEOUT_MS);

    // Locate the low region inside the full-flash image (TI image format):
    //   image[0..3]            : LE32 offset of the low region
    //   lowRegion[8..11], [12..15] : LE32 section sizes; bundle = their sum.
    const uint8_t* image = reinterpret_cast<const uint8_t*>(tps25750x_fullFlash_i2c_array);
    size_t image_size = (gSizeFullFlashArray > 0) ? (size_t)gSizeFullFlashArray : 0;
    uint32_t low_off = (image_size >= 4) ? le32(image) : 0;

    if (image_size < 16 || low_off + 16 > image_size) {
        session->phase = TpsPatchPhase::FAILED;
        session->error_message = "Bad patch image header";
        LOG_ERROR("[PATCH] Bad image: size=%u low_off=%u",
                  (unsigned)image_size, (unsigned)low_off);
        return false;
    }

    uint32_t low_size = le32(image + low_off + 8) + le32(image + low_off + 12);
    if (low_size == 0 || low_off + low_size > image_size) {
        session->phase = TpsPatchPhase::FAILED;
        session->error_message = "Bad low-region size";
        LOG_ERROR("[PATCH] Bad low-region: off=%u size=%u image=%u",
                  (unsigned)low_off, (unsigned)low_size, (unsigned)image_size);
        return false;
    }

    session->data = image + low_off;
    session->size = low_size;

    LOG_DEBUG("[PATCH] Session ready (%u bytes, target 0x%02X)",
              (unsigned)session->size, TPS_PATCH_TARGET_ADDR);
    return true;
}

TpsPatchStatus tpsPatchStep(TpsPatchSession* session) {
    if (!session) {
        return TpsPatchStatus::ERROR;
    }
    if (!session->data || session->size == 0) {
        return fail(session, "Invalid session");
    }

    switch (session->phase) {
        // ---------------------------------------------------------------------
        case TpsPatchPhase::CHECK_MODE: {
            char mode[5] = {0};
            if (hw.pdController.getMode(mode)) {
                if (modeIs(mode, "APP ")) {
                    session->skipped = true;
                    session->phase = TpsPatchPhase::DONE;
                    session->elapsed_ms = nowMs() - session->start_ms;
                    LOG_INFO("[PATCH] TPS already in APP mode - skipping push");
                    return TpsPatchStatus::SUCCESS;
                }
                if (modeIs(mode, "PTCH")) {
                    LOG_INFO("[PATCH] TPS in PTCH mode - pushing PD config (%u bytes)",
                             (unsigned)session->size);
                    session->phase = TpsPatchPhase::WAIT_READY;
                    session->phase_deadline = make_timeout_time_ms(READY_WAIT_TIMEOUT_MS);
                    return TpsPatchStatus::IN_PROGRESS;
                }
                // 'BOOT' or transient state: keep polling until deadline.
            }
            if (absolute_time_diff_us(session->phase_deadline, get_absolute_time()) >= 0) {
                return fail(session, "TPS not in PTCH mode");
            }
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::WAIT_READY: {
            uint8_t events[11] = {0};
            if (hw.pdController.readInterrupts(events) &&
                hw.pdController.isInterruptSet(events, INT_BIT_READY_FOR_PATCH)) {
                session->phase = TpsPatchPhase::SEND_PBMS;
                return TpsPatchStatus::IN_PROGRESS;
            }
            if (absolute_time_diff_us(session->phase_deadline, get_absolute_time()) >= 0) {
                // Best effort: proceed anyway if the mode is still PTCH.
                LOG_WARN("[PATCH] ReadyForPatch not seen - attempting PBMs anyway");
                session->phase = TpsPatchPhase::SEND_PBMS;
            }
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::SEND_PBMS: {
            // DATA1 input (6 bytes, TRM SLVUC05A): [0..3]=bundle size (LE),
            // [4]=patch target I2C address, [5]=burst timeout. Matches the TI
            // packed struct { u32 fw_size; u8 addr; u8 timeout; }.
            uint8_t d[6] = {0};
            d[0] = (uint8_t)(session->size & 0xFF);
            d[1] = (uint8_t)((session->size >> 8) & 0xFF);
            d[2] = (uint8_t)((session->size >> 16) & 0xFF);
            d[3] = (uint8_t)((session->size >> 24) & 0xFF);
            d[4] = TPS_PATCH_TARGET_ADDR;
            d[5] = PBMS_BURST_TIMEOUT;

            if (!hw.pdController.writeRegister(REG_DATA1, d, sizeof(d))) {
                return fail(session, "PBMs DATA1 write failed");
            }

            // The device clock-stretches/stays busy right after accepting the
            // DATA1 write; a back-to-back CMD1 write gets ACKed but dropped, so
            // PBMs never runs and CMD1 stays 0 (looks "clear"). Settle, then read
            // DATA1 back to confirm it latched before issuing the command.
            // (App note SLVAFV8 steps 4-6.)
            sleep_ms(2);
            uint8_t echo[8] = {0};
            if (hw.pdController.readRegister(REG_DATA1, echo, sizeof(echo))) {
                LOG_DEBUG("[PATCH] PBMs DATA1 set: %02X %02X %02X %02X %02X %02X",
                          echo[0], echo[1], echo[2], echo[3], echo[4], echo[5]);
            }

            if (!hw.pdController.sendCommand(CMD_PBMS)) {
                return fail(session, "PBMs command write failed");
            }
            // Confirm the command actually latched (CMD1 reads back the 4CC)
            // before waiting for it to clear, so a dropped CMD1 write is caught
            // here instead of being mistaken for instant completion.
            if (!waitCommandLatched(CMD_PBMS)) {
                return fail(session, "PBMs command did not latch");
            }
            if (!waitCommandClear(CMD_CLEAR_TIMEOUT_MS)) {
                return fail(session, "PBMs did not complete");
            }

            // The device clock-stretches while finishing PBMs and switching its
            // second slave address; give it a moment before reading the output,
            // and read the wide DATA1 register (64 bytes) rather than a 2-byte
            // snippet so the leading byte-count frames correctly. PatchStartStatus
            // is the first data byte. (App note SLVAFV8 step 9.)
            sleep_ms(5);
            uint8_t out[8] = {0};
            if (!hw.pdController.readRegister(REG_DATA1, out, sizeof(out))) {
                return fail(session, "PBMs status read failed");
            }
            LOG_DEBUG("[PATCH] PBMs DATA1: %02X %02X %02X %02X %02X %02X",
                      out[0], out[1], out[2], out[3], out[4], out[5]);
            uint8_t status = out[0];  // PatchStartStatus
            if (status != 0x00) {
                // TRM SLVUC05A: 0x04=invalid bundle size, 0x05=invalid slave
                // address, 0x06=invalid timeout.
                LOG_ERROR("[PATCH] PBMs PatchStartStatus=0x%02X", status);
                return fail(session, "PBMs rejected");
            }

            session->offset = 0;
            session->phase = TpsPatchPhase::BURST;
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::BURST: {
            size_t remaining = session->size - session->offset;
            size_t chunk = (remaining < TPS_PATCH_BURST_CHUNK) ? remaining
                                                               : TPS_PATCH_BURST_CHUNK;
            int written = i2c_write_blocking(i2c0, TPS_PATCH_TARGET_ADDR,
                                             &session->data[session->offset], chunk, false);
            if (written != (int)chunk) {
                return fail(session, "Burst write failed");
            }
            session->offset += chunk;

            if ((session->offset % 4096) == 0 || session->offset == session->size) {
                LOG_INFO("[PATCH] Streaming... %u / %u bytes",
                         (unsigned)session->offset, (unsigned)session->size);
            }

            if (session->offset >= session->size) {
                session->phase = TpsPatchPhase::SEND_PBMC;
            }
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::SEND_PBMC: {
            if (!hw.pdController.sendCommand(CMD_PBMC)) {
                return fail(session, "PBMc command write failed");
            }
            if (!waitCommandClear(CMD_CLEAR_TIMEOUT_MS)) {
                return fail(session, "PBMc did not complete");
            }

            // Output status (TRM Table 5-16): DATA1[0]=return-code nibbles,
            // [2]=DevicePatchCompleteStatus, [3]=AppConfigPatchCompleteStatus.
            uint8_t out[16] = {0};
            if (hw.pdController.readRegister(REG_DATA1, out, sizeof(out))) {
                LOG_DEBUG("[PATCH] PBMc ret=0x%02X devStatus=0x%02X acStatus=0x%02X",
                          out[0], out[2], out[3]);
            }

            session->phase = TpsPatchPhase::WAIT_APP;
            session->phase_deadline = make_timeout_time_ms(APP_WAIT_TIMEOUT_MS);
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::WAIT_APP: {
            char mode[5] = {0};
            if (hw.pdController.getMode(mode) && modeIs(mode, "APP ")) {
                session->phase = TpsPatchPhase::DONE;
                session->elapsed_ms = nowMs() - session->start_ms;
                LOG_INFO("[PATCH] Push complete - TPS in APP mode (%u ms)",
                         (unsigned)session->elapsed_ms);
                return TpsPatchStatus::SUCCESS;
            }
            if (absolute_time_diff_us(session->phase_deadline, get_absolute_time()) >= 0) {
                return fail(session, "TPS did not reach APP mode");
            }
            return TpsPatchStatus::IN_PROGRESS;
        }

        // ---------------------------------------------------------------------
        case TpsPatchPhase::DONE:
            return TpsPatchStatus::SUCCESS;

        case TpsPatchPhase::FAILED:
        default:
            return TpsPatchStatus::ERROR;
    }
}

uint8_t tpsPatchProgress(const TpsPatchSession* session) {
    if (!session) {
        return 0;
    }
    switch (session->phase) {
        case TpsPatchPhase::CHECK_MODE:
        case TpsPatchPhase::WAIT_READY:
        case TpsPatchPhase::SEND_PBMS:
            return 5;
        case TpsPatchPhase::BURST:
            // Map streamed bytes into 5..95%.
            return (uint8_t)(5 + (session->offset * 90ULL) / session->size);
        case TpsPatchPhase::SEND_PBMC:
        case TpsPatchPhase::WAIT_APP:
            return 97;
        case TpsPatchPhase::DONE:
            return 100;
        case TpsPatchPhase::FAILED:
        default:
            return 0;
    }
}
