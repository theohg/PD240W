#pragma once

#include <cstdint>

namespace PdVoltage {

/**
 * @brief Align a programmable PD voltage request down to the nearest protocol step.
 * @param voltage_mv Requested voltage in millivolts.
 * @param step_mv Protocol step size in millivolts.
 * @return The largest step-aligned voltage that does not exceed @p voltage_mv.
 */
constexpr uint32_t alignDown(uint32_t voltage_mv, uint32_t step_mv) {
    return (step_mv == 0) ? voltage_mv : (voltage_mv / step_mv) * step_mv;
}

}  // namespace PdVoltage