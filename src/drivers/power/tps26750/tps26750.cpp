/**
 * @file tps26750.cpp
 * @brief Implementation of RP2040 Driver for TI TPS26750
 * Reference: TPS26750 Technical Reference Manual (SLVUCR7)
 */

#include "tps26750.h"
#include <cstring>

namespace {

constexpr uint8_t  PDO_TYPE_SHIFT = 30;
constexpr uint32_t PDO_TYPE_MASK = 0x03;
constexpr uint8_t  PDO_TYPE_AUGMENTED = 0x03;

constexpr uint8_t  APDO_TYPE_SHIFT = 28;
constexpr uint32_t APDO_TYPE_MASK = 0x03;
constexpr uint8_t  APDO_TYPE_PPS = 0x00;
constexpr uint8_t  APDO_TYPE_EPR_AVS = 0x01;
constexpr uint8_t  APDO_TYPE_SPR_AVS = 0x02;

constexpr uint8_t  FIXED_PDO_VOLTAGE_SHIFT = 10;
constexpr uint32_t FIXED_PDO_VOLTAGE_MASK = 0x3FF;
constexpr uint32_t FIXED_PDO_VOLTAGE_UNIT_MV = 50;
constexpr uint32_t FIXED_PDO_CURRENT_MASK = 0x3FF;
constexpr uint32_t FIXED_PDO_CURRENT_UNIT_MA = 10;

constexpr uint8_t  PPS_RDO_VOLTAGE_SHIFT = 9;
constexpr uint32_t PPS_RDO_VOLTAGE_MASK = 0xFFF;
constexpr uint32_t PPS_RDO_VOLTAGE_UNIT_MV = 20;

constexpr uint8_t  AVS_RDO_VOLTAGE_SHIFT = 9;
constexpr uint32_t AVS_RDO_VOLTAGE_MASK = 0x7FF;
constexpr uint32_t AVS_RDO_VOLTAGE_UNIT_MV = 25;

constexpr uint32_t APDO_RDO_CURRENT_MASK = 0x7F;
constexpr uint32_t APDO_RDO_CURRENT_UNIT_MA = 50;

constexpr uint8_t  EPR_AVS_PDO_MAX_VOLTAGE_SHIFT = 17;
constexpr uint32_t EPR_AVS_PDO_MAX_VOLTAGE_MASK = 0x1FF;
constexpr uint8_t  PROGRAMMABLE_PDO_MIN_VOLTAGE_SHIFT = 8;
constexpr uint32_t PROGRAMMABLE_PDO_MIN_VOLTAGE_MASK = 0xFF;
constexpr uint32_t PROGRAMMABLE_PDO_VOLTAGE_UNIT_MV = 100;
constexpr uint32_t EPR_AVS_PDO_PDP_MASK_W = 0xFF;

constexpr uint8_t  SPR_AVS_9_15_CURRENT_SHIFT = 10;
constexpr uint32_t SPR_AVS_CURRENT_MASK = 0x3FF;
constexpr uint32_t SPR_AVS_CURRENT_UNIT_MA = 10;

constexpr uint8_t  PPS_PDO_MAX_VOLTAGE_SHIFT = 17;
constexpr uint32_t PPS_PDO_MAX_VOLTAGE_MASK = 0xFF;
constexpr uint32_t PPS_PDO_CURRENT_MASK = 0x7F;
constexpr uint32_t PPS_PDO_CURRENT_UNIT_MA = 50;

constexpr uint8_t SPR_PDO_COUNT_MASK = 0x07;
constexpr uint8_t EPR_PDO_COUNT_SHIFT = 3;
constexpr uint8_t EPR_PDO_COUNT_MASK = 0x07;
constexpr uint8_t SPR_PDO_START_OFFSET = 1;
constexpr uint8_t EPR_PDO_START_OFFSET = 29;
constexpr uint8_t PDO_BYTES = 4;

constexpr uint32_t PPS_REQUEST_STEP_MV = 20;
constexpr uint32_t AVS_REQUEST_STEP_MV = 25;

constexpr uint32_t extractBits(uint32_t value, uint8_t shift, uint32_t mask) {
    return (value >> shift) & mask;
}

uint32_t readLe32(const uint8_t* buffer) {
    return static_cast<uint32_t>(buffer[0]) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);
}

}  // namespace

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
    uint32_t pdo = readLe32(pdoBuf);
    uint32_t rdo = readLe32(rdoBuf);

    // PDO bits 31:30 define type: 00=Fixed, 01=Battery, 10=Variable, 11=Augmented (PPS or AVS)
    uint8_t supplyType = static_cast<uint8_t>(extractBits(pdo, PDO_TYPE_SHIFT, PDO_TYPE_MASK));

    if (supplyType == PDO_TYPE_AUGMENTED) {
        // --- Augmented PDO (PPS or AVS) ---
        // Distinguish using APDO type bits (29:28) from the PDO
        uint8_t apdo_type = static_cast<uint8_t>(extractBits(pdo, APDO_TYPE_SHIFT, APDO_TYPE_MASK));
        
        if (apdo_type == APDO_TYPE_EPR_AVS || apdo_type == APDO_TYPE_SPR_AVS) {
            // === AVS Contract ===
            // EPR AVS (0x01) and SPR AVS (0x02) share the same RDO layout.
            // TPS26750 maps AVS RDO into PPS-compatible bit positions:
            // Voltage: Bits 19:9 (11 bits), 25mV units
            voltage_mv = extractBits(rdo, AVS_RDO_VOLTAGE_SHIFT, AVS_RDO_VOLTAGE_MASK) * AVS_RDO_VOLTAGE_UNIT_MV;
            // Current: Bits 6:0 (7 bits), 50mA units
            current_ma = extractBits(rdo, 0, APDO_RDO_CURRENT_MASK) * APDO_RDO_CURRENT_UNIT_MA;
        } else if (apdo_type == APDO_TYPE_PPS) {
            // === PPS Contract ===
            // PPS RDO Voltage: Bits 20:9 (12 bits), 20mV units
            voltage_mv = extractBits(rdo, PPS_RDO_VOLTAGE_SHIFT, PPS_RDO_VOLTAGE_MASK) * PPS_RDO_VOLTAGE_UNIT_MV;
            // PPS RDO Current: Bits 6:0 (7 bits), 50mA units
            current_ma = extractBits(rdo, 0, APDO_RDO_CURRENT_MASK) * APDO_RDO_CURRENT_UNIT_MA;
        }

    } else {
        // --- Fixed / Variable / Battery Contract ---
        // For Fixed: Voltage is in PDO bits 19:10 (10 bits), unit 50mV
        voltage_mv = extractBits(pdo, FIXED_PDO_VOLTAGE_SHIFT, FIXED_PDO_VOLTAGE_MASK) * FIXED_PDO_VOLTAGE_UNIT_MV;

        // Max Current from PDO bits 9:0 (10 bits), unit 10mA
        // NOTE: Using PDO max current, NOT RDO operating current.
        // The RDO operating current (bits 19:10) reflects what the TPS26750
        // auto-negotiated internally, which can be much lower than the PDO max.
        // The PDO max is what the contract actually allows.
        current_ma = extractBits(pdo, 0, FIXED_PDO_CURRENT_MASK) * FIXED_PDO_CURRENT_UNIT_MA;
    }

    return true;
}

bool TPS26750::getPdStatus(uint8_t* status_buf) {
    // TPS_REG_PD3_STATUS (0x41) contains the Port Partner negotiated spec revision
    // According to TPS26750 TRM, the PortPartnerNegSpecRev field is in this register
    // Note: The exact bit position may vary - dump bytes if needed for debugging
    return readRegister(TPS_REG_PD3_STATUS, status_buf, 4);
}

bool TPS26750::getStatus(uint8_t* status_buf) {
    return readRegister(TPS_REG_STATUS, status_buf, 5);
}

bool TPS26750::waitForCommandClear(uint32_t timeout_ms) {
    absolute_time_t start = get_absolute_time();
    uint8_t cmd_buf[4] = {0};

    while (true) {
        if (!readRegister(TPS_REG_CMD1, cmd_buf, 4)) {
            return false;
        }

        if (cmd_buf[0] == 0 && cmd_buf[1] == 0 && cmd_buf[2] == 0 && cmd_buf[3] == 0) {
            return true;
        }

        uint32_t elapsed_ms = absolute_time_diff_us(start, get_absolute_time()) / 1000;
        if (elapsed_ms >= timeout_ms) {
            return false;
        }

        sleep_ms(10);
    }
}

bool TPS26750::sendGppiAndRead(uint16_t gppi_header,
                               const uint8_t* payload,
                               uint8_t payload_len,
                               uint8_t* out_buf,
                               uint8_t max_read_len,
                               uint16_t* actual_read_len) {
    if (!out_buf || max_read_len == 0) {
        return false;
    }

    if (payload_len > 62) {
        return false;
    }

    uint8_t safe_read_len = (max_read_len > 62) ? 62 : max_read_len;

    memset(out_buf, 0, safe_read_len);
    if (actual_read_len) {
        *actual_read_len = 0;
    }

    uint8_t data1_write[64] = {0};
    data1_write[0] = static_cast<uint8_t>(gppi_header & 0xFF);
    data1_write[1] = static_cast<uint8_t>((gppi_header >> 8) & 0xFF);
    if (payload && payload_len > 0) {
        memcpy(&data1_write[2], payload, payload_len);
    }

    if (!writeRegister(TPS_REG_DATA1, data1_write, static_cast<uint8_t>(2 + payload_len))) {
        return false;
    }

    if (!sendCommand(TPS_CMD_GPPI) || !waitForCommandClear()) {
        return false;
    }

    uint8_t gppi_status = 0xFF;
    if (!readRegister(TPS_REG_DATA1, &gppi_status, 1)) {
        return false;
    }

    if (gppi_status != 0x00) {
        return false;
    }

    uint32_t mbrd_cfg = (1UL << 22) | ((static_cast<uint32_t>(safe_read_len) & 0x3FUL) << 16);
    uint8_t mbrd_buf[4] = {
        static_cast<uint8_t>(mbrd_cfg & 0xFF),
        static_cast<uint8_t>((mbrd_cfg >> 8) & 0xFF),
        static_cast<uint8_t>((mbrd_cfg >> 16) & 0xFF),
        static_cast<uint8_t>((mbrd_cfg >> 24) & 0xFF),
    };

    if (!writeRegister(TPS_REG_DATA1, mbrd_buf, sizeof(mbrd_buf))) {
        return false;
    }

    if (!sendCommand(TPS_CMD_MBRD) || !waitForCommandClear()) {
        return false;
    }

    uint8_t data1_read[64] = {0};
    if (!readRegister(TPS_REG_DATA1, data1_read, static_cast<uint8_t>(safe_read_len + 2))) {
        return false;
    }

    uint16_t msg_size = static_cast<uint16_t>(data1_read[0]) |
                        (static_cast<uint16_t>(data1_read[1]) << 8);

    if (actual_read_len) {
        *actual_read_len = msg_size;
    }

    if (msg_size == 0 || msg_size > safe_read_len) {
        return false;
    }

    memcpy(out_buf, &data1_read[2], msg_size);
    return true;
}

bool TPS26750::getManufacturerInfo(GppiFrameType frame_type,
                                   uint8_t* out_buf,
                                   uint8_t max_read_len,
                                   uint16_t* actual_read_len) {
    constexpr uint8_t manufacturer_info_payload[2] = {0x00, 0x00};
    constexpr uint16_t GPPI_NUM_BYTES_2 = (2u << 8);
    constexpr uint16_t GPPI_EXTENDED_MESSAGE = (2u << 5);
    constexpr uint16_t GPPI_GET_MANUFACTURER_INFO = 0x06u;
    uint16_t header = (static_cast<uint16_t>(frame_type) << 13) |
                      GPPI_NUM_BYTES_2 |
                      GPPI_EXTENDED_MESSAGE |
                      GPPI_GET_MANUFACTURER_INFO;

    return sendGppiAndRead(header,
                           manufacturer_info_payload,
                           sizeof(manufacturer_info_payload),
                           out_buf,
                           max_read_len,
                           actual_read_len);
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
    uint8_t num_spr = raw_data[0] & SPR_PDO_COUNT_MASK;
    uint8_t num_epr = (raw_data[0] >> EPR_PDO_COUNT_SHIFT) & EPR_PDO_COUNT_MASK;
    uint8_t total_available = num_spr + num_epr;

    uint8_t to_parse = (total_available < max_caps) ? total_available : max_caps;
    uint8_t valid_count = 0;  // Track valid PDOs after filtering

    // Parse PDOs and validate
    for (uint8_t i = 0; i < to_parse; i++) {
        uint8_t offset;

        // Determine offset in buffer
        if (i < num_spr) {
            // SPR PDOs start at Byte 1
            offset = SPR_PDO_START_OFFSET + (i * PDO_BYTES);
        } else {
            // EPR PDOs start at Byte 29 (1 + 28)
            // i - num_spr gives index into EPR list (0 to 5)
            offset = EPR_PDO_START_OFFSET + ((i - num_spr) * PDO_BYTES);
        }

        uint32_t pdo = readLe32(&raw_data[offset]);

        uint8_t type = static_cast<uint8_t>(extractBits(pdo, PDO_TYPE_SHIFT, PDO_TYPE_MASK));

        // Temporary storage for validation
        SourceCapability temp;
        temp.is_pps = false;
        temp.is_avs = false;
        temp.voltage_mv = 0;
        temp.max_current_ma = 0;
        temp.min_voltage_mv = 0;
        temp.max_current_9_15_ma = 0;  // SPR AVS 9-15V band limit; 0 for all other types

        if (type == PDO_TYPE_AUGMENTED) {
            // --- Augmented PDO ---
            // Distinguish AVS from PPS by reading APDO type bits (29:28)
            uint8_t apdo_type = static_cast<uint8_t>(extractBits(pdo, APDO_TYPE_SHIFT, APDO_TYPE_MASK));

            if (apdo_type == APDO_TYPE_EPR_AVS) {
                // === EPR AVS ===
                temp.is_avs = true;
                // AVS Max Voltage: Bits 25-17 (9 bits), 100mV units
                temp.voltage_mv = extractBits(pdo, EPR_AVS_PDO_MAX_VOLTAGE_SHIFT, EPR_AVS_PDO_MAX_VOLTAGE_MASK) * PROGRAMMABLE_PDO_VOLTAGE_UNIT_MV;
                // AVS Min Voltage: Bits 15-8 (8 bits), 100mV units
                temp.min_voltage_mv = extractBits(pdo, PROGRAMMABLE_PDO_MIN_VOLTAGE_SHIFT, PROGRAMMABLE_PDO_MIN_VOLTAGE_MASK) * PROGRAMMABLE_PDO_VOLTAGE_UNIT_MV;
                // AVS specifies Max Power (PDP) in Watts in Bits 7-0.
                // Calculate Max Current at Max Voltage for compatibility:
                uint32_t max_power_w = extractBits(pdo, 0, EPR_AVS_PDO_PDP_MASK_W);
                if (temp.voltage_mv > 0) {
                    temp.max_current_ma = (max_power_w * 1000UL * 1000UL) / temp.voltage_mv;
                } else {
                    temp.max_current_ma = 0;
                }
            } else if (apdo_type == APDO_TYPE_SPR_AVS) {
                // === SPR AVS ===
                temp.is_avs = true;
                
                // SPR AVS does NOT use the EPR AVS layout.
                // Bits 19:10 = Max Current for 9V-15V (in 10mA units)
                // Bits 9:0   = Max Current for 15V-20V (in 10mA units)
                uint32_t max_curr_9_15_ma = extractBits(pdo, SPR_AVS_9_15_CURRENT_SHIFT, SPR_AVS_CURRENT_MASK) * SPR_AVS_CURRENT_UNIT_MA;
                uint32_t max_curr_15_20_ma = extractBits(pdo, 0, SPR_AVS_CURRENT_MASK) * SPR_AVS_CURRENT_UNIT_MA;

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
                // Both bands share the same 9V floor; store lower-band limit separately
                // so requestAvsVoltage() can cap the current for voltages below 15V.
                temp.max_current_9_15_ma = max_curr_9_15_ma;
            } else if (apdo_type == APDO_TYPE_PPS) {
                // === SPR PPS ===
                temp.is_pps = true;
                // PPS Max Voltage: Bits 24-17 (8 bits), 100mV units
                temp.voltage_mv = extractBits(pdo, PPS_PDO_MAX_VOLTAGE_SHIFT, PPS_PDO_MAX_VOLTAGE_MASK) * PROGRAMMABLE_PDO_VOLTAGE_UNIT_MV;
                // PPS Min Voltage: Bits 15-8 (8 bits), 100mV units
                temp.min_voltage_mv = extractBits(pdo, PROGRAMMABLE_PDO_MIN_VOLTAGE_SHIFT, PROGRAMMABLE_PDO_MIN_VOLTAGE_MASK) * PROGRAMMABLE_PDO_VOLTAGE_UNIT_MV;
                // PPS Max Current: Bits 6-0 (7 bits), 50mA units
                temp.max_current_ma = extractBits(pdo, 0, PPS_PDO_CURRENT_MASK) * PPS_PDO_CURRENT_UNIT_MA;
            }
        } else {
            // --- Fixed / Variable / Battery ---
            temp.voltage_mv = extractBits(pdo, FIXED_PDO_VOLTAGE_SHIFT, FIXED_PDO_VOLTAGE_MASK) * FIXED_PDO_VOLTAGE_UNIT_MV;
            temp.max_current_ma = extractBits(pdo, 0, FIXED_PDO_CURRENT_MASK) * FIXED_PDO_CURRENT_UNIT_MA;
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
        return false;
    }

    // 3. Trigger re-negotiation.
    uint8_t toggle_buf[24];
    memcpy(toggle_buf, buf, sizeof(toggle_buf));

    toggle_buf[8] ^= 0x01;
    if (!writeRegister(TPS_REG_AUTONEGOTIATE_SINK, toggle_buf, 24)) {
        return false;
    }

    sleep_ms(2);

    if (!writeRegister(TPS_REG_AUTONEGOTIATE_SINK, buf, 24)) {
        return false;
    }

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

bool TPS26750::requestPPSProfile(uint32_t voltage_mv, uint32_t current_ma,
                                 uint32_t pdo_min_mv, uint32_t pdo_max_mv) {
    // Enable PPS, Disable AVS.
    // Narrow the standard fallback window so its upper bound is strictly below the
    // requested PPS voltage. Any fixed PDO AT or ABOVE the requested voltage would
    // otherwise outrank a PPS contract of lower power (e.g. a 15 V fixed PDO beats
    // PPS at 11 V on a 3.3-16 V APDO). Clamping max to voltage_mv - 20 mV (the
    // minimum PPS step) excludes those higher-voltage fixed PDOs from contention.
    uint32_t std_max_v = (voltage_mv >= pdo_min_mv + PPS_REQUEST_STEP_MV)
                             ? (voltage_mv - PPS_REQUEST_STEP_MV)
                             : pdo_min_mv;
    return modifySinkRegister(pdo_min_mv, std_max_v, current_ma,
                              voltage_mv, current_ma, true,
                              0, 0, false);
}

bool TPS26750::requestAVSProfile(uint32_t voltage_mv, uint32_t current_ma,
                                 uint32_t pdo_min_mv, uint32_t pdo_max_mv) {
    // Enable AVS, Disable PPS.
    // Same narrowing strategy as PPS: keep the standard window max strictly below
    // the requested AVS voltage (25 mV minimum AVS step) so that fixed PDOs at or
    // above the target cannot outrank the AVS contract.
    uint32_t std_max_v = (voltage_mv >= pdo_min_mv + AVS_REQUEST_STEP_MV)
                             ? (voltage_mv - AVS_REQUEST_STEP_MV)
                             : pdo_min_mv;
    return modifySinkRegister(pdo_min_mv, std_max_v, current_ma,
                              0, 0, false,
                              voltage_mv, current_ma, true);
}