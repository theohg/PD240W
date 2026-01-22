/**
 * @file tps26750.cpp
 * @brief Implementation of RP2040 Driver for TI TPS26750
 * Reference: TPS26750 Technical Reference Manual (SLVUCR7)
 */

#include "tps26750.h"
#include <cstdio> // For NULL

// ============================================================================
// Constructor & Init
// ============================================================================

TPS26750::TPS26750(i2c_inst_t* i2c, uint8_t addr) 
    : _i2c(i2c), _addr(addr) {
}

bool TPS26750::init() {
    // Attempt to read the MODE register (0x03) to verify device presence.
    // A successful read implies the device is ACK-ing and the protocol is matching.
    char modeStr[5];
    if (!getMode(modeStr)) {
        return false;
    }
    return true;
}

// ============================================================================
// Core Register Access (Unique Address Protocol)
// ============================================================================

bool TPS26750::readRegister(uint8_t reg, uint8_t* dest, uint8_t len) {
    if (!dest || len == 0) return false;

    // 1. Write Register Address (No Stop)
    int ret = i2c_write_blocking(_i2c, _addr, &reg, 1, true);
    if (ret == PICO_ERROR_GENERIC || ret == PICO_ERROR_TIMEOUT) return false;

    // 2. Read Byte Count + Data
    // We strictly read N+1 bytes.
    uint8_t tempBuffer[len + 1];
    ret = i2c_read_blocking(_i2c, _addr, tempBuffer, len + 1, false);

    if (ret > 0) {
        // TRM 1.3.1: "The Byte Count... can be longer than the number of bytes actually written"
        // Validating the returned byte count is good practice.
        uint8_t bytesReturned = tempBuffer[0];
        
        // Safety clamp: verify device didn't send 0 or way too much (though i2c read limits us)
        if (bytesReturned == 0) return false; 
        
        // Copy the minimum of what we asked for vs what device said it sent
        uint8_t bytesToCopy = (bytesReturned < len) ? bytesReturned : len;
        
        memcpy(dest, &tempBuffer[1], bytesToCopy);
        return true;
    }

    return false;
}

bool TPS26750::writeRegister(uint8_t reg, const uint8_t* src, uint8_t len) {
    if (!src && len > 0) return false;

    // Protocol: Start -> [Reg] [ByteCount] [Data...] -> Stop
    // We allocate a temporary buffer to send the entire frame in one atomic transaction.
    // Size = 1 (Reg) + 1 (ByteCount) + len (Data)
    uint8_t buffer[len + 2];

    buffer[0] = reg;
    buffer[1] = len; // The TPS26750 requires the length of the data payload here.
    
    if (len > 0) {
        memcpy(&buffer[2], src, len);
    }

    // Send the whole block with a Stop condition (false at end).
    int ret = i2c_write_blocking(_i2c, _addr, buffer, len + 2, false);
    
    return (ret == (len + 2));
}

// ============================================================================
// High Level Functions
// ============================================================================

bool TPS26750::getMode(char* modeStr) {
    // MODE register is 4 bytes of ASCII (e.g., "APP ", "BOOT").
    // Reading 4 bytes + null terminator.
    uint8_t buffer[4];
    if (readRegister(TPS_REG_MODE, buffer, 4)) {
        memcpy(modeStr, buffer, 4);
        modeStr[4] = '\0'; // Ensure null termination
        return true;
    }
    return false;
}

bool TPS26750::sendCommand(const char* cmd) {
    if (!cmd || strlen(cmd) != 4) return false;
    
    // CMD1 register accepts 4 bytes (the 4CC code).
    // Cast char* to uint8_t* for the write function.
    return writeRegister(TPS_REG_CMD1, (const uint8_t*)cmd, 4);
}

bool TPS26750::readInterrupts(uint8_t* events) {
    // INT_EVENT1 is 11 bytes wide.
    return readRegister(TPS_REG_INT_EVENT1, events, 11);
}

bool TPS26750::clearInterrupts(const uint8_t* mask) {
    // INT_CLEAR1 is 11 bytes wide. Writing 1s clears the corresponding events.
    return writeRegister(TPS_REG_INT_CLEAR1, mask, 11);
}

bool TPS26750::isInterruptSet(const uint8_t* buffer, uint8_t bitIndex) {
    if (!buffer || bitIndex > 87) return false;
    
    uint8_t byteIndex = bitIndex / 8;
    uint8_t bitOffset = bitIndex % 8;
    
    return (buffer[byteIndex] & (1 << bitOffset)) != 0;
}

bool TPS26750::getActiveContract(uint32_t& voltage_mv, uint32_t& current_ma) {
    uint8_t pdoBuf[6] = {0}; 
    uint8_t rdoBuf[4] = {0}; 

    // Read Active PDO (0x34) [cite: 606]
    // The register is 6 bytes: 4 bytes PDO + 2 bytes padding/control
    if (!readRegister(TPS_REG_ACTIVE_CONTRACT_PDO, pdoBuf, 6)) return false;

    // Read Active RDO (0x35) [cite: 620]
    if (!readRegister(TPS_REG_ACTIVE_CONTRACT_RDO, rdoBuf, 4)) return false;

    // Convert to 32-bit integers
    uint32_t pdo = pdoBuf[0] | (pdoBuf[1] << 8) | (pdoBuf[2] << 16) | (pdoBuf[3] << 24);
    uint32_t rdo = rdoBuf[0] | (rdoBuf[1] << 8) | (rdoBuf[2] << 16) | (rdoBuf[3] << 24);

    // PDO bits 31:30 define type: 00=Fixed, 01=Battery, 10=Variable, 11=Augmented (PPS)
    uint8_t supplyType = (pdo >> 30) & 0x03;

    if (supplyType == 0x03) { 
        // --- PPS (Augmented) Contract ---
        // Voltage is in RDO bits 20:9 (12 bits), unit 20mV
        voltage_mv = ((rdo >> 9) & 0xFFF) * 20; 
        
        // Current is in RDO bits 6:0 (7 bits), unit 50mA
        current_ma = (rdo & 0x7F) * 50;
    } else {
        // --- Fixed Contract ---
        // Voltage is in PDO bits 19:10 (10 bits), unit 50mV
        voltage_mv = ((pdo >> 10) & 0x3FF) * 50;

        // Operating Current is in RDO bits 19:10 (10 bits), unit 10mA
        // (Bits 9:0 are Min/Max current, 19:10 is Operating)
        current_ma = ((rdo >> 10) & 0x3FF) * 10;
    }

    return true;
}

uint8_t TPS26750::getSourceCapabilities(SourceCapability* caps, uint8_t max_caps) {
    // Register 0x30 layout:
    // Byte 0: Info (Bits 2-0 = Num Valid PDOs)
    // Bytes 1-4: PDO 1
    // Bytes 5-8: PDO 2
    // ... up to 7 standard PDOs.
    
    // We read 29 bytes (1 Header + 7 * 4 bytes data) to cover all standard PDOs.
    uint8_t raw_data[29] = {0};
    
    // Read RX_SOURCE_CAPS (0x30)
    if (!readRegister(TPS_REG_RX_SOURCE_CAPS, raw_data, 29)) {
        return 0;
    }

    // Extract the number of valid PDOs from Byte 0 (Bits 2-0)
    uint8_t num_pdo = raw_data[0] & 0x07;
    
    // Safety check to not overflow user's array
    uint8_t count = (num_pdo < max_caps) ? num_pdo : max_caps;

    for (uint8_t i = 0; i < count; i++) {
        // Calculate offset: Header(1) + i * 4
        uint8_t offset = 1 + (i * 4);
        
        // Convert 4 bytes to 32-bit integer (Little Endian)
        uint32_t pdo = raw_data[offset] | 
                      (raw_data[offset+1] << 8)  | 
                      (raw_data[offset+2] << 16) | 
                      (raw_data[offset+3] << 24);

        // Parse based on USB PD Spec (Fixed vs PPS)
        // Bits 31-30 determine type: 00=Fixed, 11=Augmented(PPS)
        uint8_t type = (pdo >> 30) & 0x03;

        if (type == 0x03) { 
            // --- PPS (Augmented) PDO ---
            caps[i].is_pps = true;
            // Bits 24-17: Max Voltage (100mV units)
            caps[i].voltage_mv = ((pdo >> 17) & 0xFF) * 100;
            // Bits 15-8: Min Voltage (100mV units)
            caps[i].min_voltage_mv = ((pdo >> 8) & 0xFF) * 100;
            // Bits 6-0: Max Current (50mA units)
            caps[i].max_current_ma = (pdo & 0x7F) * 50;
        } else { 
            // --- Fixed Supply PDO (Type 00) ---
            // (Also covers Variable/Battery for simplicity here, but mostly Fixed)
            caps[i].is_pps = false;
            // Bits 19-10: Voltage (50mV units)
            caps[i].voltage_mv = ((pdo >> 10) & 0x3FF) * 50;
            // Bits 9-0: Max Current (10mA units)
            caps[i].max_current_ma = (pdo & 0x3FF) * 10;
            caps[i].min_voltage_mv = 0;
        }
    }

    return count;
}