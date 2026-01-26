#include "drivers/input/adc_inputs.h"
#include "utils/logging.h"

bool ADCInputs::init() {
    // Initialize ADC hardware
    adc_init();

    // Initialize GPIO pins for ADC use
    adc_gpio_init(Board::PIN_ADC_VOLT);  // GP26 - Voltage measurement
    adc_gpio_init(Board::PIN_ADC_TEMP);  // GP27 - Temperature measurement
    
    return true;
}

uint16_t ADCInputs::readADCRaw(uint8_t channel) const {
    // Validate channel (RP2040 has ADC channels 0-3, plus internal temp sensor on 4)
    if (channel > 3) {
        LOG_ERROR("Invalid ADC channel: %d", channel);
        return 0;
    }

    // Select ADC channel
    adc_select_input(channel);

    // Read with averaging for noise reduction
    uint32_t sum = 0;
    for (uint8_t i = 0; i < AVERAGING_SAMPLES; i++) {
        sum += adc_read();
    }

    return sum / AVERAGING_SAMPLES;
}

uint16_t ADCInputs::readADC(uint8_t channel) const {
    return readADCRaw(channel);
}

float ADCInputs::readVoltage(uint8_t channel) const {
    uint16_t adc_raw = readADCRaw(channel);

    // Convert to voltage using ADC reference (3.3V) and 12-bit resolution (4096)
    float voltage = adc_raw * Board::ADC_CONVERT;

    return voltage;
}

float ADCInputs::getVBUS() const {
    // Read voltage from ADC channel 0 (GP26)
    float v_adc = readVoltage(Board::ADC_CH_VOLTAGE);

    // Compensate for voltage divider (150k / 10k)
    // V_bus = V_adc * (R1 + R2) / R2
    float voltage_divider_ratio = (Board::VOLTAGE_DIVIDER_TOP + Board::VOLTAGE_DIVIDER_BOT) /
                                   Board::VOLTAGE_DIVIDER_BOT;

    float v_bus = v_adc * voltage_divider_ratio;

    return v_bus;
}

float ADCInputs::getTemperature() const {
    // Read voltage from ADC channel 1 (GP27)
    float v_ntc = readVoltage(Board::ADC_CH_TEMP);

    // Calculate NTC resistance from voltage divider
    // Circuit: 3.3V -> R_series (4.7k) -> NTC -> GND
    // V_ntc = 3.3V * (R_ntc / (R_series + R_ntc))
    // Solving for R_ntc:
    // R_ntc = R_series * V_ntc / (3.3V - V_ntc)

    float denominator = Board::ADC_REF_VOLTAGE - v_ntc;
    if (denominator <= 0.0f) {
        // Prevent division by zero or negative values
        LOG_WARN("Invalid NTC voltage reading: %.3fV", v_ntc);
        return -273.15f;  // Return absolute zero as error indicator
    }

    float r_ntc = Board::NTC_SERIES_RESISTOR * v_ntc / denominator;

    // Simplified Steinhart-Hart equation (Beta parameter equation)
    // T = 1 / (1/T0 + (1/Beta) * ln(R/R0))
    // Where:
    //   T0 = Reference temperature in Kelvin (25°C = 298.15K)
    //   R0 = Reference resistance (10kΩ @ 25°C)
    //   Beta = NTC Beta coefficient (3950)
    //   R = Measured resistance

    float t0_kelvin = Board::NTC_REF_TEMP_C + 273.15f;  // Convert to Kelvin
    float inv_t = (1.0f / t0_kelvin) + (1.0f / Board::NTC_BETA) * logf(r_ntc / Board::NTC_REF_RESISTOR);

    float t_kelvin = 1.0f / inv_t;
    float t_celsius = t_kelvin - 273.15f;

    return t_celsius;
}
