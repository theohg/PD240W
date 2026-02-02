/**
 * @file tps_eeprom_loader.cpp
 * @brief Implementation of the EEPROM flashing logic for TPS26750.
 *
 * Uses I2C1 on GPIO 14 (SDA) / GPIO 15 (SCL) to communicate with the
 * CAT24C512 EEPROM that stores the TPS26750 configuration patch.
 */

#include "tps_eeprom_loader.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "utils/logging.h"
#include <string.h>
#include <algorithm>

// =============================================================================
// External Data References (from full_flash_c_config.c)
// =============================================================================

extern "C" {
    extern const char tps25750x_fullFlash_i2c_array[];
    extern int gSizeFullFlashArray;
}

// =============================================================================
// Private Helper Functions
// =============================================================================

/**
 * @brief Initialize I2C1 on GPIO 14/15 for EEPROM access.
 */
static void eeprom_i2c_init() {
    // Initialize I2C1 peripheral
    i2c_init(EEPROM_I2C_INST, EEPROM_I2C_SPEED_HZ);

    // Configure GPIO pins for I2C function
    gpio_set_function(EEPROM_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(EEPROM_I2C_SCL_PIN, GPIO_FUNC_I2C);

    // Enable internal pull-ups (external pull-ups should also be present)
    gpio_pull_up(EEPROM_I2C_SDA_PIN);
    gpio_pull_up(EEPROM_I2C_SCL_PIN);

    LOG_INFO("[EEPROM] I2C1 initialized on GPIO %d (SDA) / GPIO %d (SCL) @ %d Hz",
             EEPROM_I2C_SDA_PIN, EEPROM_I2C_SCL_PIN, EEPROM_I2C_SPEED_HZ);
}

/**
 * @brief Deinitialize I2C1 to release resources.
 */
static void eeprom_i2c_deinit() {
    i2c_deinit(EEPROM_I2C_INST);

    // Reset GPIO pins to default state (input, no pulls)
    gpio_set_function(EEPROM_I2C_SDA_PIN, GPIO_FUNC_NULL);
    gpio_set_function(EEPROM_I2C_SCL_PIN, GPIO_FUNC_NULL);
    gpio_disable_pulls(EEPROM_I2C_SDA_PIN);
    gpio_disable_pulls(EEPROM_I2C_SCL_PIN);

    LOG_INFO("[EEPROM] I2C1 deinitialized");
}

/**
 * @brief Scan I2C bus and print found devices (for debugging)
 */
static void eeprom_i2c_scan() {
    LOG_INFO("[EEPROM] Scanning I2C1 bus...");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        // Try to read 1 byte - if device ACKs, it's present
        int ret = i2c_read_blocking(EEPROM_I2C_INST, addr, &dummy, 1, false);
        if (ret >= 0) {
            LOG_INFO("[EEPROM]   Found device at 0x%02X", addr);
            found++;
        }
    }
    LOG_INFO("[EEPROM] Scan complete: %d device(s) found", found);
}

/**
 * @brief Probe for EEPROM device presence on I2C bus.
 * @return true if device ACKs, false if NACK or timeout
 */
static bool eeprom_probe_device() {
    // Simple probe: write address bytes and check for ACK
    // This mimics what i2cdetect does
    uint8_t addr_buf[2] = {0x00, 0x00};  // EEPROM memory address 0x0000

    // Use blocking version for probe - more reliable
    int ret = i2c_write_blocking(EEPROM_I2C_INST, EEPROM_I2C_ADDR, addr_buf, 2, false);

    LOG_DEBUG("[EEPROM] Probe result: ret=%d", ret);

    // ret == 2 means 2 bytes written successfully (device ACKed)
    // ret < 0 means error (NACK or bus issue)
    return (ret == 2);
}

/**
 * @brief Writes a block of data to the EEPROM handling page boundaries.
 * @param mem_addr  Starting EEPROM memory address
 * @param data      Pointer to data buffer
 * @param len       Number of bytes to write
 * @return true on success, false on I2C error
 */
static bool eeprom_write_block(uint16_t mem_addr, const uint8_t* data, size_t len) {
    // Buffer: 2 bytes address + up to EEPROM_PAGE_SIZE data bytes
    uint8_t buffer[EEPROM_PAGE_SIZE + 2];

    size_t written = 0;
    while (written < len) {
        // Calculate space remaining in current EEPROM page
        uint16_t page_offset = mem_addr % EEPROM_PAGE_SIZE;
        size_t chunk_size = std::min((size_t)(EEPROM_PAGE_SIZE - page_offset), len - written);

        // Prepare I2C packet: [Addr High] [Addr Low] [Data...]
        buffer[0] = (mem_addr >> 8) & 0xFF;
        buffer[1] = mem_addr & 0xFF;
        memcpy(&buffer[2], &data[written], chunk_size);

        // Perform write with timeout
        int ret = i2c_write_timeout_us(EEPROM_I2C_INST, EEPROM_I2C_ADDR, buffer, chunk_size + 2, false, EEPROM_I2C_TIMEOUT_US);

        if (ret == PICO_ERROR_GENERIC) {
            LOG_ERROR("[EEPROM] I2C write NACK at 0x%04X", mem_addr);
            return false;
        } else if (ret == PICO_ERROR_TIMEOUT) {
            LOG_ERROR("[EEPROM] I2C write TIMEOUT at 0x%04X", mem_addr);
            return false;
        } else if (ret < 0) {
            LOG_ERROR("[EEPROM] I2C write error at 0x%04X (ret=%d)", mem_addr, ret);
            return false;
        }

        // Wait for EEPROM internal write cycle to complete
        sleep_ms(EEPROM_WRITE_DELAY_MS);

        // Progress indicator (every 1KB)
        if ((written % 1024) == 0) {
            LOG_INFO("[EEPROM] Writing... %u / %u bytes", written, len);
        }

        // Advance pointers
        mem_addr += chunk_size;
        written += chunk_size;
    }

    return true;
}

/**
 * @brief Reads a block of data from the EEPROM.
 * @param mem_addr  Starting EEPROM memory address
 * @param dest      Destination buffer
 * @param len       Number of bytes to read
 * @return true on success, false on I2C error
 */
static bool eeprom_read_block(uint16_t mem_addr, uint8_t* dest, size_t len) {
    uint8_t addr_buf[2];
    addr_buf[0] = (mem_addr >> 8) & 0xFF;
    addr_buf[1] = mem_addr & 0xFF;

    // Write the address we want to read from (with repeated start)
    int ret = i2c_write_timeout_us(EEPROM_I2C_INST, EEPROM_I2C_ADDR, addr_buf, 2, true, EEPROM_I2C_TIMEOUT_US);
    if (ret == PICO_ERROR_TIMEOUT) {
        LOG_ERROR("[EEPROM] I2C address write TIMEOUT at 0x%04X", mem_addr);
        return false;
    } else if (ret < 0) {
        LOG_ERROR("[EEPROM] I2C address write error at 0x%04X (ret=%d)", mem_addr, ret);
        return false;
    }

    // Read the data with timeout
    ret = i2c_read_timeout_us(EEPROM_I2C_INST, EEPROM_I2C_ADDR, dest, len, false, EEPROM_I2C_TIMEOUT_US);
    if (ret == PICO_ERROR_TIMEOUT) {
        LOG_ERROR("[EEPROM] I2C read TIMEOUT at 0x%04X", mem_addr);
        return false;
    } else if (ret < 0) {
        LOG_ERROR("[EEPROM] I2C read error at 0x%04X (ret=%d)", mem_addr, ret);
        return false;
    }

    return true;
}

/**
 * @brief Check if EEPROM already contains the correct data.
 * @param fw_data   Pointer to firmware data
 * @param fw_size   Size of firmware data
 * @return true if EEPROM matches firmware exactly
 */
static bool eeprom_already_programmed(const uint8_t* fw_data, size_t fw_size) {
    uint8_t read_buffer[128];
    size_t checked = 0;

    // Quick check: Read first 128 bytes to detect obvious mismatch early
    if (!eeprom_read_block(0, read_buffer, sizeof(read_buffer))) {
        LOG_WARN("[EEPROM] Quick check read failed");
        return false;
    }
    if (memcmp(fw_data, read_buffer, std::min(sizeof(read_buffer), fw_size)) != 0) {
        LOG_INFO("[EEPROM] Header mismatch - needs programming");
        return false;
    }
    LOG_INFO("[EEPROM] Header matches, checking full image...");
    checked = sizeof(read_buffer);

    // Full verification with progress
    while (checked < fw_size) {
        size_t chunk = std::min(sizeof(read_buffer), fw_size - checked);

        if (!eeprom_read_block(checked, read_buffer, chunk)) {
            LOG_WARN("[EEPROM] Read failed at 0x%04X during check", checked);
            return false;  // Read failed, assume not programmed
        }

        if (memcmp(&fw_data[checked], read_buffer, chunk) != 0) {
            LOG_INFO("[EEPROM] Mismatch at 0x%04X - needs programming", checked);
            return false;  // Mismatch found
        }

        checked += chunk;

        // Progress every 4KB
        if ((checked % 4096) == 0) {
            LOG_INFO("[EEPROM] Checking... %u / %u bytes", checked, fw_size);
        }
    }

    return true;  // All bytes match
}

/**
 * @brief Check if EEPROM appears to be empty (all 0xFF or all 0x00).
 * @return true if first 128 bytes are all 0xFF or all 0x00
 */
static bool eeprom_is_empty() {
    uint8_t read_buffer[128];
    if (!eeprom_read_block(0, read_buffer, sizeof(read_buffer))) {
        return false;  // Can't read, assume not empty
    }

    bool all_ff = true;
    bool all_00 = true;
    for (size_t i = 0; i < sizeof(read_buffer); i++) {
        if (read_buffer[i] != 0xFF) all_ff = false;
        if (read_buffer[i] != 0x00) all_00 = false;
    }

    return all_ff || all_00;
}

// =============================================================================
// Public Runtime API
// =============================================================================

bool eepromInit() {
    eeprom_i2c_init();
    sleep_ms(50);  // Allow bus to stabilize
    return true;
}

void eepromDeinit() {
    eeprom_i2c_deinit();
}

bool eepromProbe() {
    return eeprom_probe_device();
}

size_t eepromGetFirmwareSize() {
    return (size_t)gSizeFullFlashArray;
}

EepromCompareResult eepromCompare() {
    // Check firmware size sanity
    if (gSizeFullFlashArray <= 0 || (size_t)gSizeFullFlashArray > EEPROM_TOTAL_SIZE) {
        LOG_ERROR("[EEPROM] Invalid firmware size: %d bytes", gSizeFullFlashArray);
        return EepromCompareResult::READ_ERROR;
    }

    // Probe for device
    if (!eeprom_probe_device()) {
        LOG_ERROR("[EEPROM] Device not found at 0x%02X", EEPROM_I2C_ADDR);
        return EepromCompareResult::NO_DEVICE;
    }

    const uint8_t* fw_data = reinterpret_cast<const uint8_t*>(tps25750x_fullFlash_i2c_array);

    // Check if empty first
    if (eeprom_is_empty()) {
        LOG_INFO("[EEPROM] EEPROM appears to be empty");
        return EepromCompareResult::EMPTY;
    }

    // Compare against firmware
    if (eeprom_already_programmed(fw_data, gSizeFullFlashArray)) {
        LOG_INFO("[EEPROM] EEPROM matches firmware exactly");
        return EepromCompareResult::IDENTICAL;
    }

    LOG_INFO("[EEPROM] EEPROM has different configuration");
    return EepromCompareResult::DIFFERENT;
}

bool eepromFlash(EepromProgressCallback callback, void* user_data) {
    // Validate firmware
    if (gSizeFullFlashArray <= 0 || (size_t)gSizeFullFlashArray > EEPROM_TOTAL_SIZE) {
        LOG_ERROR("[EEPROM] Invalid firmware size: %d bytes", gSizeFullFlashArray);
        return false;
    }

    // Probe device
    if (!eeprom_probe_device()) {
        LOG_ERROR("[EEPROM] Device not found at 0x%02X", EEPROM_I2C_ADDR);
        return false;
    }

    const uint8_t* fw_data = reinterpret_cast<const uint8_t*>(tps25750x_fullFlash_i2c_array);
    size_t fw_size = (size_t)gSizeFullFlashArray;

    // Phase 0: Write
    LOG_INFO("[EEPROM] Phase 1: Writing %d bytes...", fw_size);

    // Buffer: 2 bytes address + up to EEPROM_PAGE_SIZE data bytes
    uint8_t buffer[EEPROM_PAGE_SIZE + 2];
    size_t written = 0;

    while (written < fw_size) {
        uint16_t mem_addr = written;
        uint16_t page_offset = mem_addr % EEPROM_PAGE_SIZE;
        size_t chunk_size = std::min((size_t)(EEPROM_PAGE_SIZE - page_offset), fw_size - written);

        buffer[0] = (mem_addr >> 8) & 0xFF;
        buffer[1] = mem_addr & 0xFF;
        memcpy(&buffer[2], &fw_data[written], chunk_size);

        int ret = i2c_write_timeout_us(EEPROM_I2C_INST, EEPROM_I2C_ADDR, buffer, chunk_size + 2, false, EEPROM_I2C_TIMEOUT_US);
        if (ret < 0) {
            LOG_ERROR("[EEPROM] Write failed at 0x%04X (ret=%d)", mem_addr, ret);
            return false;
        }

        sleep_ms(EEPROM_WRITE_DELAY_MS);

        written += chunk_size;

        // Progress callback
        if (callback) {
            uint8_t progress = (uint8_t)((written * 100) / fw_size);
            callback(0, progress, user_data);
        }
    }

    LOG_INFO("[EEPROM] Write complete.");

    // Phase 1: Verify
    LOG_INFO("[EEPROM] Phase 2: Verifying...");

    uint8_t read_buffer[128];
    size_t verified = 0;

    while (verified < fw_size) {
        size_t chunk = std::min(sizeof(read_buffer), fw_size - verified);

        if (!eeprom_read_block(verified, read_buffer, chunk)) {
            LOG_ERROR("[EEPROM] Verification read failed at 0x%04X", verified);
            return false;
        }

        if (memcmp(&fw_data[verified], read_buffer, chunk) != 0) {
            LOG_ERROR("[EEPROM] Verification mismatch at 0x%04X", verified);
            return false;
        }

        verified += chunk;

        // Progress callback
        if (callback) {
            uint8_t progress = (uint8_t)((verified * 100) / fw_size);
            callback(1, progress, user_data);
        }
    }

    LOG_INFO("[EEPROM] Verification complete - SUCCESS!");
    return true;
}

// =============================================================================
// Legacy Public Implementation
// =============================================================================

bool flashTps26750Eeprom() {
#if !ENABLE_TPS_EEPROM_FLASHING
    // Flashing disabled - this is the normal path
    return true;
#else
    LOG_SEPARATOR();
    LOG_INFO("[EEPROM] TPS26750 Patch Flash Enabled");
    LOG_SEPARATOR();

    // =========================================================================
    // Pre-flight checks
    // =========================================================================

    // Check 1: Validate firmware size is sane
    if (gSizeFullFlashArray <= 0) {
        LOG_ERROR("[EEPROM] Invalid firmware size: %d bytes", gSizeFullFlashArray);
        return false;
    }

    if ((size_t)gSizeFullFlashArray > EEPROM_TOTAL_SIZE) {
        LOG_ERROR("[EEPROM] Firmware too large: %d bytes (max %d)",
                  gSizeFullFlashArray, EEPROM_TOTAL_SIZE);
        return false;
    }

    LOG_INFO("[EEPROM] Firmware size: %d bytes (%.1f%% of EEPROM)",
             gSizeFullFlashArray,
             (float)gSizeFullFlashArray / EEPROM_TOTAL_SIZE * 100.0f);

    // Initialize I2C1 for EEPROM access
    eeprom_i2c_init();
    sleep_ms(50);  // Allow bus to stabilize (increased from 10ms)

    // Debug: Scan bus to see what's there
    eeprom_i2c_scan();

    // Check 2: Probe for EEPROM device presence with retries
    LOG_INFO("[EEPROM] Probing for device at 0x%02X...", EEPROM_I2C_ADDR);
    bool device_found = false;
    for (int retry = 0; retry < 3; retry++) {
        if (eeprom_probe_device()) {
            device_found = true;
            break;
        }
        LOG_WARN("[EEPROM] Probe attempt %d failed, retrying...", retry + 1);
        sleep_ms(100);  // Wait before retry
    }

    if (!device_found) {
        LOG_ERROR("[EEPROM] No device found at address 0x%02X after 3 attempts!", EEPROM_I2C_ADDR);
        LOG_ERROR("[EEPROM] Check I2C wiring: SDA=GPIO%d, SCL=GPIO%d",
                  EEPROM_I2C_SDA_PIN, EEPROM_I2C_SCL_PIN);
        eeprom_i2c_deinit();
        return false;
    }
    LOG_INFO("[EEPROM] Device found!");

    // Cast source array to uint8_t pointer
    const uint8_t* fw_data = reinterpret_cast<const uint8_t*>(tps25750x_fullFlash_i2c_array);

    // Check 3: See if EEPROM is already programmed correctly
    LOG_INFO("[EEPROM] Checking if already programmed...");
    if (eeprom_already_programmed(fw_data, gSizeFullFlashArray)) {
        LOG_INFO("[EEPROM] EEPROM already contains correct data - skipping write!");
        eeprom_i2c_deinit();
        return true;
    }
    LOG_INFO("[EEPROM] EEPROM needs programming.");

    // =========================================================================
    // Phase 1: Write firmware to EEPROM
    // =========================================================================
    LOG_INFO("[EEPROM] Phase 1: Writing %d bytes to EEPROM...", gSizeFullFlashArray);

    if (!eeprom_write_block(0x0000, fw_data, gSizeFullFlashArray)) {
        LOG_ERROR("[EEPROM] FAILED during write operation");
        eeprom_i2c_deinit();
        return false;
    }

    LOG_INFO("[EEPROM] Write complete.");

    // =========================================================================
    // Phase 2: Verify by reading back
    // =========================================================================
    LOG_INFO("[EEPROM] Phase 2: Verifying %d bytes...", gSizeFullFlashArray);

    // Read back in chunks to save RAM
    uint8_t read_buffer[128];
    size_t verified = 0;

    while (verified < (size_t)gSizeFullFlashArray) {
        size_t chunk = std::min(sizeof(read_buffer), (size_t)(gSizeFullFlashArray - verified));

        if (!eeprom_read_block(verified, read_buffer, chunk)) {
            LOG_ERROR("[EEPROM] Verification read failed at 0x%04X", verified);
            eeprom_i2c_deinit();
            return false;
        }

        // Compare with original data
        if (memcmp(&fw_data[verified], read_buffer, chunk) != 0) {
            // Find first mismatched byte for debugging
            for (size_t i = 0; i < chunk; i++) {
                if (fw_data[verified + i] != read_buffer[i]) {
                    LOG_ERROR("[EEPROM] Mismatch at 0x%04X: expected 0x%02X, got 0x%02X",
                              verified + i, fw_data[verified + i], read_buffer[i]);
                    break;
                }
            }
            eeprom_i2c_deinit();
            return false;
        }

        verified += chunk;

        // Progress indicator (every 4KB)
        if ((verified % 4096) == 0) {
            LOG_INFO("[EEPROM] Verified %u / %d bytes", verified, gSizeFullFlashArray);
        }
    }

    // =========================================================================
    // Success
    // =========================================================================
    eeprom_i2c_deinit();

    LOG_SEPARATOR();
    LOG_INFO("[EEPROM] SUCCESS! Patch written and verified.");
    LOG_INFO("[EEPROM] Power cycle the board to load new TPS26750 config.");
    LOG_INFO("[EEPROM] Then set ENABLE_TPS_EEPROM_FLASHING=0 and rebuild.");
    LOG_SEPARATOR();

    return true;
#endif
}
