/**
 * @file tps26750.h
 * @brief RP2040 C++ Driver for TI TPS26750 USB PD Controller
 * @details Implements the "Unique Address Interface" I2C protocol specific to TPS26750.
 * Reference: TPS26750 Technical Reference Manual (SLVUCR7)
 */

#ifndef TPS26750_H
#define TPS26750_H

#include <cstdint>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

// I2C Default Address Options (Decoded from ADCINx pins)
// #1: 0x20, #2: 0x21, #3: 0x22, #4: 0x23
#define TPS26750_I2C_ADDR_DEFAULT 0x21 

// Represents one "line item" from the charger (e.g., "15V @ 3A")
struct SourceCapability {
    uint32_t voltage_mv;           // Fixed: Voltage. PPS/AVS: Max Voltage.
    uint32_t max_current_ma;       // Max Current (15-20V band for SPR AVS; overall for others)
    bool is_pps;                   // Programmable Power Supply (SPR)
    bool is_avs;                   // Adjustable Voltage Supply (SPR or EPR)
    // For PPS/AVS: voltage_mv is the MAX voltage, current is max current
    uint32_t min_voltage_mv;       // Only valid if is_pps or is_avs = true
    /// @brief SPR AVS only: max current for the 9-15V band (USB PD 3.2 §6.4.2).
    /// Zero for all other PDO types, including EPR AVS.
    uint32_t max_current_9_15_ma;
};

// ============================================================================
// Register Map (TRM Table 4-1) 
// ============================================================================
enum TPS_Reg : uint8_t {
    TPS_REG_MODE                        = 0x03, // 4 bytes
    TPS_REG_CUST_USE                    = 0x06, // 8 bytes
    TPS_REG_CMD1                        = 0x08, // 4 bytes
    TPS_REG_DATA1                       = 0x09, // 64 bytes
    TPS_REG_INT_EVENT1                  = 0x14, // 11 bytes
    TPS_REG_INT_MASK1                   = 0x16, // 11 bytes
    TPS_REG_INT_CLEAR1                  = 0x18, // 11 bytes
    TPS_REG_STATUS                      = 0x1A, // 5 bytes
    TPS_REG_POWER_PATH_STATUS           = 0x26, // 5 bytes
    TPS_REG_PORT_CONFIG                 = 0x28, // 17 bytes
    TPS_REG_PORT_CONTROL                = 0x29, // 4 bytes
    TPS_REG_BOOT_FLAGS                  = 0x2D, // 5 bytes
    TPS_REG_RX_SOURCE_CAPS              = 0x30, // 53 bytes (Updated for EPR)
    TPS_REG_RX_SINK_CAPS                = 0x31, // 29 bytes
    TPS_REG_TX_SOURCE_CAPS              = 0x32, // 31 bytes
    TPS_REG_TX_SINK_CAPS                = 0x33, // 29 bytes
    TPS_REG_ACTIVE_CONTRACT_PDO         = 0x34, // 6 bytes
    TPS_REG_ACTIVE_CONTRACT_RDO         = 0x35, // 4 bytes
    TPS_REG_AUTONEGOTIATE_SINK          = 0x37, // 24 bytes
    TPS_REG_POWER_STATUS                = 0x3F, // 2 bytes
    TPS_REG_PD_STATUS                   = 0x40, // 4 bytes
    TPS_REG_PD3_STATUS                  = 0x41, // 4 bytes - Contains PortPartnerNegSpecRev
    TPS_REG_PD3_CONFIG                  = 0x42, // 4 bytes
    TPS_REG_IO_CONFIG                   = 0x5C, // 49 bytes
    TPS_REG_TYPEC_STATE                 = 0x69, // 4 bytes
    TPS_REG_ADC_RESULTS                 = 0x6A, // 13 bytes
    TPS_REG_SLEEP_CONTROL               = 0x70, // 1 byte
    TPS_REG_GPIO_STATUS                 = 0x72, // 8 bytes
    TPS_REG_TX_SOURCE_CAPS_EXT          = 0x77, // 15 bytes
    TPS_REG_TX_SOURCE_INFO              = 0x78, // 4 bytes
    TPS_REG_TX_PPS_STATUS               = 0x7A, // 4 bytes
    TPS_REG_TX_BATTERY_STATUS           = 0x7B, // 16 bytes
    TPS_REG_TX_BATTERY_CAPS             = 0x7D, // 36 bytes
    TPS_REG_TX_SINK_CAPS_EXT            = 0x7E, // 14 bytes
    TPS_REG_LIQUID_DETECTION            = 0x98, // 11 bytes
};

// ============================================================================
// Bit Masks & Definitions
// ============================================================================

// --- STATUS Register (0x1A) Masks ---
#define TPS_STATUS_PLUG_PRESENT         (1 << 0)
#define TPS_STATUS_CONN_STATE_MASK      (0x0E)  // Bits 3-1
#define TPS_STATUS_CONN_NO_CONNECTION   (0x00)
#define TPS_STATUS_CONN_AUDIO           (0x04)  // 010b << 1
#define TPS_STATUS_CONN_DEBUG           (0x06)  // 011b << 1
#define TPS_STATUS_CONN_PRESENT_NO_RA   (0x0C)  // 110b << 1
#define TPS_STATUS_CONN_PRESENT_RA      (0x0E)  // 111b << 1
#define TPS_STATUS_ORIENTATION          (1 << 4) // 0 = Upside-up (CC1), 1 = Upside-down (CC2)
#define TPS_STATUS_PORT_ROLE            (1 << 5) // 0 = Sink, 1 = Source
#define TPS_STATUS_DATA_ROLE            (1 << 6) // 0 = UFP, 1 = DFP
#define TPS_STATUS_VBUS_STATUS_MASK     (0x300000) // Bits 21-20 (Byte 2)
#define TPS_STATUS_VBUS_SAFE0V          (0x00)
#define TPS_STATUS_VBUS_SAFE5V          (0x100000)
#define TPS_STATUS_VBUS_VALID           (0x200000)

// --- POWER_PATH_STATUS Register (0x26) Masks ---
#define TPS_PP_STATUS_PP_CABLE1_SW_MASK (0x03)
#define TPS_PP_STATUS_PP1_SWITCH_MASK   (0x1C0) // Bits 8-6 (5V Path)
#define TPS_PP_STATUS_PP3_SWITCH_MASK   (0x7000)// Bits 14-12 (HV/Ext Path)
#define TPS_PP_STATUS_PP1_OVERCURRENT   (1 << 28)
#define TPS_PP_STATUS_POWER_SOURCE_MASK (0xC000000000) // Bits 39-38
#define TPS_PP_STATUS_SOURCE_VIN3V3     (0x4000000000) // 01b
#define TPS_PP_STATUS_SOURCE_VBUS       (0x8000000000) // 10b (Dead Battery)

// --- INT_EVENT1 Register (0x14) Masks ---
// Note: This register is 11 bytes wide. These are bit offsets from byte 0.
#define TPS_INT_PD_HARD_RESET           (1ULL << 1)
#define TPS_INT_PLUG_INSERT_REMOVAL     (1ULL << 3)
#define TPS_INT_POWER_SWAP_COMPLETE     (1ULL << 4)
#define TPS_INT_DATA_SWAP_COMPLETE      (1ULL << 5)
#define TPS_INT_NEW_CONTRACT_AS_SINK    (1ULL << 12)
#define TPS_INT_NEW_CONTRACT_AS_SOURCE  (1ULL << 13)
#define TPS_INT_SOURCE_CAP_RX           (1ULL << 14)
#define TPS_INT_CMD1_COMPLETE           (1ULL << 30)
#define TPS_INT_ERROR_PROTOCOL          (1ULL << 38)
#define TPS_INT_READY_FOR_PATCH         (1ULL << 81) // Byte 10, bit 1

// --- 4CC Commands (for CMD1 Register) ---
#define TPS_CMD_GAID "Gaid" // Warm Restart
#define TPS_CMD_GAID_COLD "GAID" // Cold Reset
#define TPS_CMD_SWSK "SWSk" // Swap to Sink
#define TPS_CMD_SWSr "SWSr" // Swap to Source
#define TPS_CMD_GSrC "GSrC" // Get Source Caps (Used to re-negotiate Sink Contract)
#define TPS_CMD_GSkC "GSkC" // Get Sink Caps
#define TPS_CMD_ESrC "ESrC" // EPR Get Source Caps (Request EPR profiles 28V/36V/48V)
#define TPS_CMD_GPPI "GPPI" // Get Port Partner Information
#define TPS_CMD_MBRD "MBRd" // Message Buffer Read
#define TPS_CMD_PBMe "PBMe" // Patch Bundle Mode Exit

enum class GppiFrameType : uint8_t {
    SOP = 0,
    SOP_PRIME = 1,
    SOP_DBL_PRIME = 2,
};

// ============================================================================
// Class Definition
// ============================================================================

class TPS26750 {
public:
    /**
     * @brief Constructor
     * @param i2c Pointer to the RP2040 hardware I2C instance (i2c0 or i2c1).
     * @param addr I2C Slave address (Default 0x20 for Address Index #1).
     */
    TPS26750(i2c_inst_t* i2c, uint8_t addr = TPS26750_I2C_ADDR_DEFAULT);

    /**
     * @brief Initialize I2C and check device presence.
     * @return true if device acknowledges, false otherwise.
     */
    bool init();

    // ------------------------------------------------------------------------
    // Core Register Access (Implements Unique Address Protocol)
    // ------------------------------------------------------------------------

    /**
     * @brief Read a register using the specific TPS26750 protocol.
     * @details Protocol: Start -> Addr(Wr) -> Reg -> Sr -> Addr(Rd) -> ByteCount(N) -> Data... -> Stop.
     * The ByteCount is read from the device but stripped from the output buffer.
     *
     * @param reg Register offset (from TPS_Reg enum).
     * @param dest Buffer to store read data.
     * @param len Number of bytes to read (must match register size).
     * @return true on success.
     */
    bool readRegister(uint8_t reg, uint8_t* dest, uint8_t len);

    /**
     * @brief Write a register using the specific TPS26750 protocol.
     * @details Protocol: Start -> Addr(Wr) -> Reg -> ByteCount(N) -> Data... -> Stop.
     * The ByteCount is automatically prepended.
     * @param reg Register offset.
     * @param src Data to write.
     * @param len Number of bytes to write.
     * @return true on success.
     */
    bool writeRegister(uint8_t reg, const uint8_t* src, uint8_t len);

    // ------------------------------------------------------------------------
    // High Level Functions
    // ------------------------------------------------------------------------

    /**
     * @brief Get the current Mode string (e.g., "APP ", "BOOT", "PTCH").
     * @param modeStr Buffer of at least 5 bytes (4 chars + null).
     */
    bool getMode(char* modeStr);

    /**
     * @brief Send a 4CC Command.
     * @param cmd 4-character string (e.g., "Gaid").
     */
    bool sendCommand(const char* cmd);

    /**
     * @brief Read the Interrupt Event Register.
     * @param events Buffer of 11 bytes to store the event flags.
     */
    bool readInterrupts(uint8_t* events);

    /**
     * @brief Clear specific interrupts.
     * @param mask Buffer of 11 bytes representing bits to clear.
     */
    bool clearInterrupts(const uint8_t* mask);

    /**
     * @brief Check if a specific interrupt bit is set in the provided buffer.
     * @param buffer 11-byte interrupt buffer.
     * @param bitIndex Bit index (0-87) to check.
     */
    bool isInterruptSet(const uint8_t* buffer, uint8_t bitIndex);

    /**
     * @brief Read the active contract Voltage and Current.
     * @details Correctly parses Fixed, PPS, and AVS contracts.
     * @param voltage_mv Output voltage in mV.
     * @param current_ma Output current in mA.
     * @return true if read successful.
     */
    bool getActiveContract(uint32_t& voltage_mv, uint32_t& current_ma);

    /**
     * @brief Read the PD_STATUS register.
     * @details Contains PD spec revision and contract status.
     * @param status_buf Buffer of at least 4 bytes to store status.
     * @return true if read successful.
     */
    bool getPdStatus(uint8_t* status_buf);

    /**
     * @brief Read the STATUS register.
     * @details Exposes connection/orientation state needed by higher-level diagnostics.
     * @param status_buf Buffer of at least 5 bytes to store status.
     * @return true if read successful.
     */
    bool getStatus(uint8_t* status_buf);

    /**
     * @brief Send a GPPI message and read the resulting message buffer.
     * @details Executes the TI 'GPPI' and 'MBRd' 4CC tasks synchronously.
     * @param gppi_header Encoded GPPI header written to DATA1 byte 0..1.
     * @param payload Optional payload bytes written after the GPPI header.
     * @param payload_len Number of payload bytes.
     * @param out_buf Buffer receiving the raw message-buffer payload bytes.
     * @param max_read_len Maximum number of payload bytes to fetch from MBRd.
     * @param actual_read_len Optional output for the message size returned by MBRd.
     * @return true when GPPI succeeds and MBRd returns a non-zero payload length.
     */
    bool sendGppiAndRead(uint16_t gppi_header,
                         const uint8_t* payload,
                         uint8_t payload_len,
                         uint8_t* out_buf,
                         uint8_t max_read_len,
                         uint16_t* actual_read_len = nullptr);

    /**
     * @brief Request USB PD Manufacturer Info from a PD partner.
     * @param frame_type SOP for the source partner; other frame types are controller-dependent.
     * @param out_buf Buffer receiving the raw Manufacturer Info response payload.
     * @param max_read_len Maximum number of payload bytes to fetch from MBRd.
     * @param actual_read_len Optional output for the message size returned by MBRd.
     * @return true when the Manufacturer Info exchange succeeds and returns payload bytes.
     */
    bool getManufacturerInfo(GppiFrameType frame_type,
                             uint8_t* out_buf,
                             uint8_t max_read_len,
                             uint16_t* actual_read_len = nullptr);
    
    /**
     * @brief Reads the list of available power contracts offered by the source.
     * @details Reads both SPR (PDO 1-7) and EPR (PDO 8-13) capabilities.
     * @param caps Array to store the parsed capabilities.
     * @param max_caps Size of the 'caps' array (TPS26750 supports up to 13 total).
     * @return Number of capabilities found.
     */
    uint8_t getSourceCapabilities(SourceCapability* caps, uint8_t max_caps);

    // ------------------------------------------------------------------------
    // Contract Negotiation (Auto Negotiate Sink - Register 0x37)
    // ------------------------------------------------------------------------

    /**
     * @brief Requests a standard Fixed Voltage contract (e.g., 5V, 9V, 15V, 20V, 28V, 48V).
     * @details Sets Min/Max voltage to target +/- small tolerance, disables PPS/AVS, 
     * and issues GSrC to trigger negotiation.
     *
     * @param voltage_mv Target voltage in mV.
     * @param max_current_ma Max current required in mA.
     * @return true if register write and command sent successfully.
     */
    bool requestFixedProfile(uint32_t voltage_mv, uint32_t max_current_ma);

    /**
     * @brief Requests a PPS (Programmable Power Supply) contract.
     * @details Enables PPS mode and constrains the standard fallback window to the
     * selected APDO's range, preventing the chip from picking an overlapping APDO
     * or falling back to a higher fixed rail.
     * @param voltage_mv Target voltage in mV (20mV steps).
     * @param current_ma Limit current in mA (50mA steps).
     * @param pdo_min_mv Minimum voltage of the selected PPS APDO in mV.
     * @param pdo_max_mv Maximum voltage of the selected PPS APDO in mV.
     * @return true if request sent.
     */
    bool requestPPSProfile(uint32_t voltage_mv, uint32_t current_ma,
                           uint32_t pdo_min_mv, uint32_t pdo_max_mv);

    /**
     * @brief Requests an AVS (Adjustable Voltage Supply) contract (SPR or EPR).
     * @details Enables AVS mode and constrains the standard fallback window to the
     * selected APDO's range.
     * @param voltage_mv Target voltage in mV (25mV steps).
     * @param current_ma Limit current in mA (50mA steps).
     * @param pdo_min_mv Minimum voltage of the selected AVS APDO in mV.
     * @param pdo_max_mv Maximum voltage of the selected AVS APDO in mV.
     * @return true if request sent.
     */
    bool requestAVSProfile(uint32_t voltage_mv, uint32_t current_ma,
                           uint32_t pdo_min_mv, uint32_t pdo_max_mv);

private:
    i2c_inst_t* _i2c;
    uint8_t _addr;
    static constexpr uint32_t TASK_WAIT_TIMEOUT_MS = 1500;

    // Helper to block until CMD1 is cleared by the internal task engine.
    bool waitForCommandClear(uint32_t timeout_ms = TASK_WAIT_TIMEOUT_MS);

    // Helper to read/modify/write the large AUTONEGOTIATE_SINK register (24 bytes)
    bool modifySinkRegister(uint32_t min_v, uint32_t max_v, uint32_t op_i, 
                            uint32_t pps_v, uint32_t pps_i, bool pps_en,
                            uint32_t avs_v, uint32_t avs_i, bool avs_en);
};

#endif // TPS26750_H