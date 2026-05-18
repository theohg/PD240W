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

    // Read Active PDO (0x34)
    // The register is 6 bytes: 4 bytes PDO + 2 bytes padding/control
    if (!readRegister(TPS_REG_ACTIVE_CONTRACT_PDO, pdoBuf, 6)) return false;

    // Read Active RDO (0x35)
    if (!readRegister(TPS_REG_ACTIVE_CONTRACT_RDO, rdoBuf, 4)) return false;

    // Convert to 32-bit integers
    uint32_t pdo = pdoBuf[0] | (pdoBuf[1] << 8) | (pdoBuf[2] << 16) | (pdoBuf[3] << 24);
    uint32_t rdo = rdoBuf[0] | (rdoBuf[1] << 8) | (rdoBuf[2] << 16) | (rdoBuf[3] << 24);

    // PDO bits 31:30 define type: 00=Fixed, 01=Battery, 10=Variable, 11=Augmented (PPS or AVS)
    uint8_t supplyType = (pdo >> 30) & 0x03;

    if (supplyType == 0x03) { 
        // --- Augmented PDO (PPS or AVS) ---
        // Distinguish using APDO type bits (29:28) from the PDO
        uint8_t apdo_type = (pdo >> 28) & 0x03;
        
        if (apdo_type == 0x01) { 
            // === AVS Contract ===
            // TPS26750 maps AVS RDO into PPS-compatible bit positions:
            // Voltage: Bits 19:9 (11 bits), 25mV units
            voltage_mv = ((rdo >> 9) & 0x7FF) * 25; 
            // Current: Bits 6:0 (7 bits), 50mA units
            current_ma = (rdo & 0x7F) * 50;
        } else if (apdo_type == 0x00) {
            // === PPS Contract ===
            // PPS RDO Voltage: Bits 20:9 (12 bits), 20mV units
            voltage_mv = ((rdo >> 9) & 0xFFF) * 20; 
            // PPS RDO Current: Bits 6:0 (7 bits), 50mA units
            current_ma = (rdo & 0x7F) * 50;
        }

    } else {
        // --- Fixed / Variable / Battery Contract ---
        // For Fixed: Voltage is in PDO bits 19:10 (10 bits), unit 50mV
        voltage_mv = ((pdo >> 10) & 0x3FF) * 50;

        // Max Current from PDO bits 9:0 (10 bits), unit 10mA
        // NOTE: Using PDO max current, NOT RDO operating current.
        // The RDO operating current (bits 19:10) reflects what the TPS26750
        // auto-negotiated internally, which can be much lower than the PDO max.
        // The PDO max is what the contract actually allows.
        current_ma = (pdo & 0x3FF) * 10;
    }

    return true;
}

bool TPS26750::getPdStatus(uint8_t* status_buf) {
    // TPS_REG_PD3_STATUS (0x41) contains the Port Partner negotiated spec revision
    // According to TPS26750 TRM, the PortPartnerNegSpecRev field is in this register
    // Note: The exact bit position may vary - dump bytes if needed for debugging
    return readRegister(TPS_REG_PD3_STATUS, status_buf, 4);
}

uint8_t TPS26750::getSourceCapabilities(SourceCapability* caps, uint8_t max_caps) {
    // Register 0x30 
    // Structure:
    // Byte 0: Info (Bits 2-0 = SPR Valid count, Bits 5-3 = EPR Valid count)
    // Bytes 1-28: SPR PDOs 1-7 (4 bytes each)
    // Bytes 29-52: EPR PDOs 8-13 (4 bytes each)
    
    // We read 53 bytes to cover all possible PDOs (SPR + EPR).
    uint8_t raw_data[53] = {0};
    
    if (!readRegister(TPS_REG_RX_SOURCE_CAPS, raw_data, 53)) {
        return 0;
    }

    // Extract SPR count (0-7) and EPR count (0-6)
    uint8_t num_spr = raw_data[0] & 0x07;
    uint8_t num_epr = (raw_data[0] >> 3) & 0x07;
    uint8_t total_available = num_spr + num_epr;

    uint8_t to_parse = (total_available < max_caps) ? total_available : max_caps;
    uint8_t valid_count = 0;  // Track valid PDOs after filtering

    // Parse PDOs and validate
    for (uint8_t i = 0; i < to_parse; i++) {
        uint8_t offset;

        // Determine offset in buffer
        if (i < num_spr) {
            // SPR PDOs start at Byte 1
            offset = 1 + (i * 4);
        } else {
            // EPR PDOs start at Byte 29 (1 + 28)
            // i - num_spr gives index into EPR list (0 to 5)
            offset = 29 + ((i - num_spr) * 4);
        }

        uint32_t pdo = raw_data[offset] |
                      (raw_data[offset+1] << 8)  |
                      (raw_data[offset+2] << 16) |
                      (raw_data[offset+3] << 24);

        uint8_t type = (pdo >> 30) & 0x03;

        // Temporary storage for validation
        SourceCapability temp;
        temp.is_pps = false;
        temp.is_avs = false;
        temp.voltage_mv = 0;
        temp.max_current_ma = 0;
        temp.min_voltage_mv = 0;

        if (type == 0x03) {
            // --- Augmented PDO ---
            // Distinguish AVS from PPS by reading APDO type bits (29:28)
            uint8_t apdo_type = (pdo >> 28) & 0x03;

            if (apdo_type == 0x01) {
                // === EPR AVS ===
                temp.is_avs = true;
                // AVS Max Voltage: Bits 25-17 (9 bits), 100mV units
                temp.voltage_mv = ((pdo >> 17) & 0x1FF) * 100;
                // AVS Min Voltage: Bits 15-8 (8 bits), 100mV units
                temp.min_voltage_mv = ((pdo >> 8) & 0xFF) * 100;
                // AVS specifies Max Power (PDP) in Watts in Bits 7-0.
                // Calculate Max Current at Max Voltage for compatibility:
                uint32_t max_power_w = pdo & 0xFF;
                if (temp.voltage_mv > 0) {
                    temp.max_current_ma = (max_power_w * 1000UL * 1000UL) / temp.voltage_mv;
                } else {
                    temp.max_current_ma = 0;
                }
            } else if (apdo_type == 0x02) {
                // === SPR AVS ===
                temp.is_avs = true;
                
                // SPR AVS does NOT use the EPR AVS layout.
                // Bits 19:10 = Max Current for 9V-15V (in 10mA units)
                // Bits 9:0   = Max Current for 15V-20V (in 10mA units)
                uint32_t max_curr_9_15_ma  = ((pdo >> 10) & 0x3FF) * 10;
                uint32_t max_curr_15_20_ma = (pdo & 0x3FF) * 10;

                // USB PD 3.2 dictates that SPR AVS minimum voltage is always 9V
                temp.min_voltage_mv = 9000;
                
                // The max voltage is 20V if the 15V-20V current field is populated (>0).
                // Otherwise, the max voltage is 15V.
                if (max_curr_15_20_ma > 0) {
                    temp.voltage_mv = 20000;
                    temp.max_current_ma = max_curr_15_20_ma;
                } else {
                    temp.voltage_mv = 15000;
                    temp.max_current_ma = max_curr_9_15_ma;
                }
            } else if (apdo_type == 0x00) {
                // === SPR PPS ===
                temp.is_pps = true;
                // PPS Max Voltage: Bits 24-17 (8 bits), 100mV units
                temp.voltage_mv = ((pdo >> 17) & 0xFF) * 100;
                // PPS Min Voltage: Bits 15-8 (8 bits), 100mV units
                temp.min_voltage_mv = ((pdo >> 8) & 0xFF) * 100;
                // PPS Max Current: Bits 6-0 (7 bits), 50mA units
                temp.max_current_ma = (pdo & 0x7F) * 50;
            }
        } else {
            // --- Fixed / Variable / Battery ---
            temp.voltage_mv = ((pdo >> 10) & 0x3FF) * 50;
            temp.max_current_ma = (pdo & 0x3FF) * 10;
            temp.min_voltage_mv = 0;
        }

        // Validate PDO - skip invalid entries
        bool valid = true;
        if (temp.voltage_mv == 0) {
            valid = false;
        }
        if (temp.max_current_ma == 0) {
            valid = false;
        }
        // For PPS/AVS, check range validity
        if ((temp.is_pps || temp.is_avs) && temp.min_voltage_mv >= temp.voltage_mv) {
            valid = false;
        }

        if (valid) {
            // Deduplication: skip if identical PDO already exists
            bool duplicate = false;
            for (uint8_t j = 0; j < valid_count; j++) {
                if (caps[j].voltage_mv == temp.voltage_mv &&
                    caps[j].max_current_ma == temp.max_current_ma &&
                    caps[j].is_pps == temp.is_pps &&
                    caps[j].is_avs == temp.is_avs &&
                    caps[j].min_voltage_mv == temp.min_voltage_mv) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                caps[valid_count] = temp;
                valid_count++;
            }
        }
    }

    return valid_count;
}

// ============================================================================
// Contract Negotiation Logic
// ============================================================================

bool TPS26750::modifySinkRegister(uint32_t min_v, uint32_t max_v, uint32_t op_i,
                                  uint32_t pps_v, uint32_t pps_i, bool pps_en,
                                  uint32_t avs_v, uint32_t avs_i, bool avs_en)
{
    // 1. Read existing register (0x37, 24 bytes) to preserve reserved bits
    uint8_t buf[24] = {0};
    if (!readRegister(TPS_REG_AUTONEGOTIATE_SINK, buf, 24)) {
        printf("[DEBUG] Failed to read AUTONEGOTIATE_SINK\n");
        return false;
    }

    // === FIX: Force Manual Mode ===
    // Clear bits 6, 5, 4, 2 to disable auto-compute and auto-select features
    // Set bit 3 (No Capability Mismatch) to accept lower-power contracts
    buf[0] &= ~((1 << 6) | (1 << 5) | (1 << 4) | (1 << 2));
    buf[0] |= (1 << 3);

    // === Clear Power Requirement Fields ===
    buf[2] &= 0x3F;  
    buf[3] = 0x00;   
    buf[6] &= 0x0F;  
    buf[7] &= 0xC0;  

    // --- Update Standard Fields ---
    uint16_t max_v_val = max_v / 50;
    buf[4] = (max_v_val & 0xFF);
    buf[5] = (buf[5] & 0xFC) | ((max_v_val >> 8) & 0x03);

    uint16_t min_v_val = min_v / 50;
    buf[5] = (buf[5] & 0x03) | ((min_v_val & 0x3F) << 2);
    buf[6] = (buf[6] & 0xF0) | ((min_v_val >> 6) & 0x0F);

    uint16_t max_i_val = op_i / 10;
    buf[1] = (buf[1] & 0x0F) | ((max_i_val & 0x0F) << 4);
    buf[2] = (buf[2] & 0xC0) | ((max_i_val >> 4) & 0x3F);

    // --- Update PPS Fields ---
    if (pps_en) buf[8] |= 0x01; else buf[8] &= ~0x01;

    if (pps_en) {
        uint8_t pps_i_val = (pps_i / 50) & 0x7F;
        buf[12] = (buf[12] & 0x80) | pps_i_val; 
        
        uint16_t pps_v_val = pps_v / 20;
        buf[13] = (buf[13] & 0x01) | ((pps_v_val & 0x7F) << 1);
        buf[14] = (buf[14] & 0xF0) | ((pps_v_val >> 7) & 0x0F);
    }

    // --- Update AVS Fields ---
    if (avs_en) buf[16] |= 0x01; else buf[16] &= ~0x01;

    if (avs_en) {
        uint8_t avs_i_val = (avs_i / 50) & 0x7F;
        buf[20] = (buf[20] & 0x80) | avs_i_val;

        // AVS voltage uses 25mV units (not 50mV like PPS uses 20mV)
        uint16_t avs_v_val = avs_v / 25; 
        buf[21] = (buf[21] & 0x01) | ((avs_v_val & 0x7F) << 1);
        buf[22] = (buf[22] & 0xE0) | ((avs_v_val >> 7) & 0x1F);
    }

    // 2. Write back
    if (!writeRegister(TPS_REG_AUTONEGOTIATE_SINK, buf, 24)) {
        printf("[DEBUG] Failed to write AUTONEGOTIATE_SINK\n");
        return false;
    }

    // 3. Trigger Re-negotiation via PPS Toggle Trick
    // Instead of relying purely on GSrC (which LG monitors ignore),
    // we toggle PPSEnableSinkMode to force the TPS26750 to automatically re-evaluate
    // and send a Request message directly based on currently cached capabilities.
    uint8_t toggle_buf[24];
    memcpy(toggle_buf, buf, 24);
    
    // Flip the PPS Enable bit
    toggle_buf[8] ^= 0x01;
    writeRegister(TPS_REG_AUTONEGOTIATE_SINK, toggle_buf, 24);
    
    sleep_ms(2); // Give TPS26750 a moment to register the change
    
    // Restore and write final intended state
    writeRegister(TPS_REG_AUTONEGOTIATE_SINK, buf, 24);

    // printf("Triggered re-negotiation via PPS toggle trick\n");
    return true;
}

bool TPS26750::requestFixedProfile(uint32_t voltage_mv, uint32_t max_current_ma) {
    // Disable PPS/AVS, set Min/Max voltage to target +/- tolerance.
    // AutoComputeSinkMin/MaxVoltage are bits 4 and 5. We should clear them (Byte 0) 
    // to allow our manual settings to take effect.
    // However, for simplicity here, I rely on the modify function. 
    // To ensure "Fixed" is selected, we set PPS/AVS enables to false.
    
    // IMPORTANT: You might also need to ensure "AutoComputeSinkMaxVoltage" (Bit 5) 
    // and "AutoComputeSinkMinVoltage" (Bit 4) are set to 0 (Host Provided) in Byte 0.
    // The previous implementation didn't strictly toggle these, but let's assume 
    // the user defaults are suitable or we add logic here.
    // For now, setting the register limits and disabling PPS/AVS is the standard way.
    
    // Setting range slightly wide to catch the PDO (e.g. +/- 5%)
    uint32_t min_v = voltage_mv - (voltage_mv / 20); // 95%
    uint32_t max_v = voltage_mv + (voltage_mv / 20); // 105%

    return modifySinkRegister(min_v, max_v, max_current_ma, 
                              0, 0, false,  // PPS Disabled
                              0, 0, false); // AVS Disabled
}

bool TPS26750::requestPPSProfile(uint32_t voltage_mv, uint32_t current_ma) {
    // Enable PPS, Disable AVS.
    // We also set Min/Max voltage wide or 0 to not interfere with standard negotiation fallback
    // if PPS fails, but strictly we just want to enable the PPS bit.
    return modifySinkRegister(5000, 21000, 3000, // Fallback defaults
                              voltage_mv, current_ma, true, 
                              0, 0, false);
}

bool TPS26750::requestAVSProfile(uint32_t voltage_mv, uint32_t current_ma) {
    // Enable AVS, Disable PPS.
    // Provide sufficient headroom for max_v so the internal policy engine accepts the request
    uint32_t max_v = voltage_mv + 2000;
    return modifySinkRegister(5000, max_v, 5000, // Fallback defaults with headroom
                              0, 0, false, 
                              voltage_mv, current_ma, true);
}