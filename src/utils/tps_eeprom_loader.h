/**
 * @file tps_eeprom_loader.h
 * @brief Utilities to flash the TPS26750 configuration patch to the attached I2C EEPROM.
 *
 * The EEPROM (CAT24C512) is connected to the TPS26750's I2Cc bus and also routed
 * to RP2040 GPIO 14 (SDA) / GPIO 15 (SCL), which maps to I2C1.
 *
 * This allows the RP2040 to program the EEPROM directly without an external flasher.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "pico/stdlib.h"

// =============================================================================
// EEPROM I2C Configuration (I2C1 on GPIO 14/15)
// =============================================================================

// I2C1 pins for EEPROM access (directly connected to CAT24C512)
#define EEPROM_I2C_INST      i2c1
#define EEPROM_I2C_SDA_PIN   14
#define EEPROM_I2C_SCL_PIN   15
#define EEPROM_I2C_SPEED_HZ  400000  // 400kHz Fast Mode

// The I2C address of the CAT24C512 EEPROM (0x50 is standard for 24Cxx series)
#define EEPROM_I2C_ADDR      0x50

// CAT24C512 EEPROM characteristics
#define EEPROM_PAGE_SIZE      128      // Bytes per page (writes must not cross boundaries)
#define EEPROM_WRITE_DELAY_MS 6        // Write cycle time (datasheet max is 5ms, add margin)
#define EEPROM_TOTAL_SIZE     65536    // 512Kbit = 64KB total capacity
#define EEPROM_I2C_TIMEOUT_US 50000    // 50ms timeout for I2C operations

// =============================================================================
// EEPROM Comparison Results
// =============================================================================

enum class EepromCompareResult {
    IDENTICAL,      // EEPROM matches the firmware binary exactly
    DIFFERENT,      // EEPROM has different configuration data
    EMPTY,          // EEPROM appears to be empty (all 0xFF or all 0x00)
    READ_ERROR,     // I2C communication failed
    NO_DEVICE       // EEPROM device not found on I2C bus
};

// =============================================================================
// EEPROM Flash Progress Callback
// =============================================================================

/**
 * @brief Callback function type for flash progress updates.
 * @param phase     Current phase: 0=write, 1=verify
 * @param progress  Progress percentage (0-100)
 * @param user_data User-provided context pointer
 */
typedef void (*EepromProgressCallback)(uint8_t phase, uint8_t progress, void* user_data);

enum class EepromFlashStatus : uint8_t {
    IN_PROGRESS,
    SUCCESS,
    ERROR
};

struct EepromFlashSession {
    const uint8_t* fw_data;
    size_t fw_size;
    size_t write_offset;
    size_t verify_offset;
    absolute_time_t next_action_time;
    bool waiting_for_write_cycle;
    bool verify_announced;
    const char* error_message;
};

// =============================================================================
// Public API
// =============================================================================

/**
 * @brief Initialize I2C1 for EEPROM access.
 * Call before using other EEPROM functions. 
 * @return true if initialization succeeded
 */
bool eepromInit();

/**
 * @brief Deinitialize I2C1 to release resources.
 * Call when done with EEPROM operations.
 */
void eepromDeinit();

/**
 * @brief Check if EEPROM device is present on the I2C bus.
 * @return true if device responds to probe
 */
bool eepromProbe();

/**
 * @brief Compare EEPROM contents against the firmware patch binary.
 * @return EepromCompareResult indicating match status
 */
EepromCompareResult eepromCompare();

/**
 * @brief Get the size of the firmware patch in bytes.
 * @return Firmware size in bytes
 */
size_t eepromGetFirmwareSize();

/**
 * @brief Begin a non-blocking EEPROM flash session.
 * @param session Session state storage owned by the caller
 * @return true if the session is ready to step, false on setup failure
 */
bool eepromFlashBegin(EepromFlashSession* session);

/**
 * @brief Advance a non-blocking EEPROM flash session by one write or verify chunk.
 * @param session Session state storage from eepromFlashBegin
 * @param callback Optional progress callback (can be nullptr)
 * @param user_data User context passed to callback
 * @return Current session status
 */
EepromFlashStatus eepromFlashStep(EepromFlashSession* session,
                                  EepromProgressCallback callback = nullptr,
                                  void* user_data = nullptr);