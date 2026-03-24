//    FILE: INA228.cpp
//  AUTHOR: Rob Tillaart
// VERSION: 0.4.1
//    DATE: 2024-05-09
// PURPOSE: Arduino library for the INA228, I2C, 20 bit, voltage, current and power sensor.
//     URL: https://github.com/RobTillaart/INA228
//          https://www.adafruit.com/product/5832           ( 10 A version)
//          https://www.mateksys.com/?portfolio=i2c-ina-bm  (200 A version))

//
//  Read the datasheet for the details


#include "ina228.h"
#include <math.h>

//      REGISTERS                   ADDRESS    BITS  RW
#define INA228_CONFIG               0x00    //  16   RW
#define INA228_ADC_CONFIG           0x01    //  16   RW
#define INA228_SHUNT_CAL            0x02    //  16   RW
#define INA228_SHUNT_TEMP_CO        0x03    //  16   RW
#define INA228_SHUNT_VOLTAGE        0x04    //  24   R-
#define INA228_BUS_VOLTAGE          0x05    //  24   R-
#define INA228_TEMPERATURE          0x06    //  16   R-
#define INA228_CURRENT              0x07    //  24   R-
#define INA228_POWER                0x08    //  24   R-
#define INA228_ENERGY               0x09    //  40   R-
#define INA228_CHARGE               0x0A    //  40   R-
#define INA228_DIAG_ALERT           0x0B    //  16   RW
#define INA228_SOVL                 0x0C    //  16   RW
#define INA228_SUVL                 0x0D    //  16   RW
#define INA228_BOVL                 0x0E    //  16   RW
#define INA228_BUVL                 0x0F    //  16   RW
#define INA228_TEMP_LIMIT           0x10    //  16   RW
#define INA228_POWER_LIMIT          0x11    //  16   RW
#define INA228_MANUFACTURER         0x3E    //  16   R-
#define INA228_DEVICE_ID            0x3F    //  16   R-


//  CONFIG MASKS (register 0)
#define INA228_CFG_RST              0x8000
#define INA228_CFG_RSTACC           0x4000
#define INA228_CFG_CONVDLY          0x3FC0
#define INA228_CFG_TEMPCOMP         0x0020
#define INA228_CFG_ADCRANGE         0x0010
#define INA228_CFG_RESERVED         0x000F  //  all unused bits


//  ADC MASKS (register 1)
#define INA228_ADC_MODE             0xF000
#define INA228_ADC_VBUSCT           0x0E00
#define INA228_ADC_VSHCT            0x01C0
#define INA228_ADC_VTCT             0x0038
#define INA228_ADC_AVG              0x0007


////////////////////////////////////////////////////////
//
//  CONSTRUCTOR
//
INA228::INA228(uint8_t address, i2c_inst_t *i2c, float shuntResistor, float maxCurrent, uint16_t shuntTempCoPpm)
{
  _address          = address;
  _i2c              = i2c;
  _shunt            = shuntResistor;
  _maxCurrent       = maxCurrent;
  _overcurrentLimit = 0.0f;
  _shuntTempCoPpm   = shuntTempCoPpm;
  _current_LSB      = _maxCurrent * pow(2, -19);
  _error            = 0;
}


bool INA228::init()
{
  if (!isConnected()) return false;

  // Read current ADC range setting
  getADCRange();

  // Configure shunt calibration with provided values
  setMaxCurrentShunt(_maxCurrent, _shunt);

  // Set averaging to 16 samples for stable readings
  setAverage(INA228_16_SAMPLES);

  // Set VSHCT (Shunt Voltage Conversion Time) 
  setShuntVoltageConversionTime(INA228_50_us);

  // Enable shunt temperature compensation (corrects for resistor drift with temp)
  setShuntTemperatureCoefficent(_shuntTempCoPpm);
  setTemperatureCompensation(true);

  return true;
}


bool INA228::isConnected()
{
  uint8_t rxdata;
  // Attempt to read 1 byte to check for ACK. 
  // Returns bytes read (positive) or error code (negative)
  int ret = i2c_read_blocking(_i2c, _address, &rxdata, 1, false);
  return (ret >= 0);
}


uint8_t INA228::getAddress()
{
  return _address;
}


////////////////////////////////////////////////////////
//
//  CORE FUNCTIONS
//
//  PAGE 25
float INA228::getBusVoltage()
{
  //  always positive, remove reserved bits.
  int32_t value = _readRegister(INA228_BUS_VOLTAGE, 3) >> 4;
  float bus_LSB = 195.3125e-6;  //  195.3125 uV
  float voltage = value * bus_LSB;
  return voltage;
}

//  PAGE 25
float INA228::getShuntVoltage()
{
  //  shunt_LSB depends on ADCRANGE in INA228_CONFIG register.
  float shunt_LSB = 312.5e-9;  //  312.5 nV
  if (_ADCRange == true)
  {
    shunt_LSB = 78.125e-9;     //  78.125 nV
  }

  //  remove reserved bits.
  int32_t value = _readRegister(INA228_SHUNT_VOLTAGE, 3) >> 4;
  //  handle negative values (20 bit)
  if (value & 0x00080000)
  {
    value |= 0xFFF00000;
  }
  float voltage = value * shunt_LSB;
  return voltage;
}

int32_t INA228::getShuntVoltageRAW()
{
  //  remove reserved bits.
  uint32_t value = _readRegister(INA228_SHUNT_VOLTAGE, 3) >> 4;
  //  handle negative values (20 bit)
  if (value & 0x00080000)
  {
    value |= 0xFFF00000;
  }
  return (int32_t)value;
}

//  PAGE 25 + 8.1.2
float INA228::getCurrent()
{
  //  remove reserved bits.
  int32_t value = _readRegister(INA228_CURRENT, 3) >> 4;
  //  handle negative values (20 bit)
  if (value & 0x00080000)
  {
    value |= 0xFFF00000;
  }
  float current = value * _current_LSB;
  return current;
}

//  PAGE 26 + 8.1.2
float INA228::getPower()
{
  uint32_t value = _readRegister(INA228_POWER, 3);
  //  PAGE 31 (8.1.2)
  return value * 3.2 * _current_LSB;
}

//  PAGE 25
float INA228::getTemperature()
{
  uint32_t value = _readRegister(INA228_TEMPERATURE, 2);
  float LSB = 7.8125e-3;  //  milli degree Celsius
  return value * LSB;
}

//  PAGE 26 + 8.1.2
double INA228::getEnergy()
{
  //  read 40 bit UNSIGNED as a double to prevent 64 bit integers
  //  double might be 8 or 4 byte, depends on platform
  //  40 bit ==> O(10^12)
  double value = _readRegisterF(INA228_ENERGY, 'U');
  //  PAGE 31 (8.1.2)
  return value * (16 * 3.2) * _current_LSB;
}


//  PAGE 26 + 8.1.2
double INA228::getCharge()
{
  //  read 40 bit SIGNED as a float to prevent 64 bit integers
  //  double might be 8 or 4 byte, depends on platform
  //  40 bit ==> O(10^12)
  double value = _readRegisterF(INA228_CHARGE, 'S');
  //  PAGE 32 (8.1.2)
  return value * _current_LSB;
}


////////////////////////////////////////////////////////
//
//  CONFIG REGISTER 0
//
void INA228::reset()
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  value |= INA228_CFG_RST;
  _writeRegister(INA228_CONFIG, value);
}

bool INA228::setAccumulation(uint8_t value)
{
  if (value > 1) return false;
  uint16_t reg = _readRegister(INA228_CONFIG, 2);
  if (value == 1) reg |= INA228_CFG_RSTACC;
  else            reg &= ~INA228_CFG_RSTACC;
  _writeRegister(INA228_CONFIG, reg);
  return true;
}

bool INA228::getAccumulation()
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  return (value & INA228_CFG_RSTACC) > 0;
}

void INA228::setConversionDelay(uint8_t steps)
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  value &= ~INA228_CFG_CONVDLY;
  value |= (steps << 6);
  _writeRegister(INA228_CONFIG, value);
}

uint8_t INA228::getConversionDelay()
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  return (value >> 6) & 0xFF;
}

void INA228::setTemperatureCompensation(bool on)
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  if (on) value |= INA228_CFG_TEMPCOMP;
  else    value &= ~INA228_CFG_TEMPCOMP;
  _writeRegister(INA228_CONFIG, value);
}

bool INA228::getTemperatureCompensation()
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  return (value & INA228_CFG_TEMPCOMP) > 0;
}

bool INA228::setADCRange(bool flag)
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  _ADCRange = (value & INA228_CFG_ADCRANGE) > 0;
  //  nothing changed ==> we're done.
  if (flag == _ADCRange) return true;

  _ADCRange = flag;
  if (flag) value |= INA228_CFG_ADCRANGE;
  else      value &= ~INA228_CFG_ADCRANGE;
  _writeRegister(INA228_CONFIG, value);
  //  Fix #26, issue where shunt_cal was not modified
  bool rv = setMaxCurrentShunt(getMaxCurrent(), getShunt()) == 0;
  return rv;
}

bool INA228::getADCRange()
{
  uint16_t value = _readRegister(INA228_CONFIG, 2);
  _ADCRange = (value & INA228_CFG_ADCRANGE) > 0;
  return _ADCRange;
}


////////////////////////////////////////////////////////
//
//  CONFIG ADC REGISTER 1
//
bool INA228::setMode(uint8_t mode)
{
  if (mode > 0x0F) return false;
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  value &= ~INA228_ADC_MODE;
  value |= (mode << 12);
  _writeRegister(INA228_ADC_CONFIG, value);
  return true;
}

uint8_t INA228::getMode()
{
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  return (value & INA228_ADC_MODE) >> 12;
}

bool INA228::setBusVoltageConversionTime(uint8_t bvct)
{
  if (bvct > 7) return false;
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  value &= ~INA228_ADC_VBUSCT;
  value |= (bvct << 9);
  _writeRegister(INA228_ADC_CONFIG, value);
  return true;
}

uint8_t INA228::getBusVoltageConversionTime()
{
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  return (value & INA228_ADC_VBUSCT) >> 9;
}

bool INA228::setShuntVoltageConversionTime(uint8_t svct)
{
  if (svct > 7) return false;
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  value &= ~INA228_ADC_VSHCT;
  value |= (svct << 6);
  _writeRegister(INA228_ADC_CONFIG, value);
  return true;
}

uint8_t INA228::getShuntVoltageConversionTime()
{
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  return (value & INA228_ADC_VSHCT) >> 6;
}

bool INA228::setTemperatureConversionTime(uint8_t tct)
{
  if (tct > 7) return false;
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  value &= ~INA228_ADC_VTCT;
  value |= (tct << 3);
  _writeRegister(INA228_ADC_CONFIG, value);
  return true;
}

uint8_t INA228::getTemperatureConversionTime()
{
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  return (value & INA228_ADC_VTCT) >> 3;
}

bool INA228::setAverage(uint8_t avg)
{
  if (avg > 7) return false;
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  value &= ~INA228_ADC_AVG;
  value |= avg;
  _writeRegister(INA228_ADC_CONFIG, value);
  return true;
}

uint8_t INA228::getAverage()
{
  uint16_t value = _readRegister(INA228_ADC_CONFIG, 2);
  return (value & INA228_ADC_AVG);
}


////////////////////////////////////////////////////////
//
//  SHUNT CALIBRATION REGISTER 2
//
int INA228::setMaxCurrentShunt(float maxCurrent, float shunt)
{
  //  Shunt can be really small
  if (shunt < 0.0001) return -2;    //  TODO error code
  if (maxCurrent < 0.0) return -3;  //  TODO error code
  _maxCurrent = maxCurrent;
  _shunt = shunt;
  _current_LSB = _maxCurrent * 1.9073486328125e-6;  //  pow(2, -19);

  //  PAGE 31 (8.1.2)
  float shunt_cal = 13107.2e6 * _current_LSB * _shunt;
  //  depends on ADCRANGE in INA228_CONFIG register.
  if (_ADCRange == true)
  {
    shunt_cal *= 4;
  }
  //  shunt_cal must be written to its REGISTER.
  _writeRegister(INA228_SHUNT_CAL, shunt_cal);

  return 0;
}

float INA228::getMaxCurrent()
{
  return _maxCurrent;
}

float INA228::getShunt()
{
  return _shunt;
}

float INA228::getCurrentLSB()
{
  return _current_LSB;
}


////////////////////////////////////////////////////////
//
//  SHUNT TEMPERATURE COEFFICIENT REGISTER 3
//
bool INA228::setShuntTemperatureCoefficent(uint16_t ppm)
{
  if (ppm > 16383) return false;
  _writeRegister(INA228_SHUNT_TEMP_CO, ppm);
  return true;
}

uint16_t INA228::getShuntTemperatureCoefficent()
{
  uint16_t value = _readRegister(INA228_SHUNT_TEMP_CO, 2);
  return value;
}


////////////////////////////////////////////////////////
//
//  DIAGNOSE ALERT REGISTER 11
//
void INA228::setDiagnoseAlert(uint16_t flags)
{
  _writeRegister(INA228_DIAG_ALERT, flags);
}

uint16_t INA228::getDiagnoseAlert()
{
  return _readRegister(INA228_DIAG_ALERT, 2);
}

//  INA228.h has an enum for the bit fields.
void INA228::setDiagnoseAlertBit(uint8_t bit)
{
  uint16_t value = _readRegister(INA228_DIAG_ALERT, 2);
  uint16_t mask = (1 << bit);
  //  only write new value if bit not set
  if ((value & mask) == 0)
  {
    value |= mask;
    _writeRegister(INA228_DIAG_ALERT, value);
  }
}

void INA228::clearDiagnoseAlertBit(uint8_t bit)
{
  uint16_t value = _readRegister(INA228_DIAG_ALERT, 2);
  uint16_t mask = (1 << bit);
  //  only write new value if bit not set.
  if ((value & mask ) != 0)
  {
    value &= ~mask;
    _writeRegister(INA228_DIAG_ALERT, value);
  }
}

uint16_t INA228::getDiagnoseAlertBit(uint8_t bit)
{
  uint16_t value = _readRegister(INA228_DIAG_ALERT, 2);
  return (value >> bit) & 0x01;
}


////////////////////////////////////////////////////////
//
//  OVERCURRENT PROTECTION
//
bool INA228::setOvercurrentLimit(float current_limit_amps, bool latch)
{
  // Validate input
  if (current_limit_amps <= 0.0f) {
    disableOvercurrentLimit();
    return true;
  }
  if (current_limit_amps > _maxCurrent) {
    return false;  // Exceeds configured max current
  }

  // Calculate shunt voltage threshold: V_shunt = I * R_shunt
  float v_shunt_threshold = current_limit_amps * _shunt;

  // Get LSB based on ADC range setting
  // ADCRANGE = 0: ±163.84 mV range, 5.0 µV/LSB
  // ADCRANGE = 1: ±40.96 mV range, 1.25 µV/LSB
  float lsb_uv = _ADCRange ? 1.25e-6f : 5.0e-6f;

  // Calculate raw threshold value
  uint16_t raw_threshold = static_cast<uint16_t>(v_shunt_threshold / lsb_uv);

  // Set the shunt overvoltage threshold
  setShuntOvervoltageTH(raw_threshold);

  // Configure ALERT pin behavior:
  // 1. Clear any existing alerts
  // 2. Enable Shunt Over Limit (SOL) alert
  // 3. Optionally enable latching
  setDiagnoseAlert(0x0000);  // Reset alerts
  setDiagnoseAlertBit(INA228_DIAG_SHUNT_OVER_LIMIT);  // Enable SOL

  if (latch) {
    setDiagnoseAlertBit(INA228_DIAG_ALERT_LATCH);  // Latch until read
  }

  // Store the configured limit
  _overcurrentLimit = current_limit_amps;

  return true;
}

void INA228::disableOvercurrentLimit()
{
  // Clear the SOL alert bit
  clearDiagnoseAlertBit(INA228_DIAG_SHUNT_OVER_LIMIT);
  _overcurrentLimit = 0.0f;
}

float INA228::getOvercurrentLimit()
{
  return _overcurrentLimit;
}


////////////////////////////////////////////////////////
//
//  THRESHOLD AND LIMIT REGISTERS 12-17
//
//  TODO  (sync INA228)
//  - API ?
//  - return bool for setters
//  - float voltage interface instead of uint16_t?  breaking!
void INA228::setShuntOvervoltageTH(uint16_t threshold)
{
  //  TODO ADCRANGE DEPENDENT
  //  Conversion Factor: 5 μV/LSB when ADCRANGE = 0
  //  1.25 μV/LSB when ADCRANGE = 1.
  //  float LSB = 5.0e-6;
  //  if (_ADCRange == 1) LSB = 1.25e-6;
  _writeRegister(INA228_SOVL, threshold);
}

uint16_t INA228::getShuntOvervoltageTH()
{
  //  TODO ADCRANGE DEPENDENT
  //  float LSB = 5.0e-6;
  //  if (_ADCRange == 1) LSB = 1.25e-6;
  return _readRegister(INA228_SOVL, 2);
}

void INA228::setShuntUndervoltageTH(uint16_t threshold)
{
  //  TODO ADCRANGE DEPENDENT
  //  float LSB = 5.0e-6;
  //  if (_ADCRange == 1) LSB = 1.25e-6;
  _writeRegister(INA228_SUVL, threshold);
}

uint16_t INA228::getShuntUndervoltageTH()
{
  //  TODO ADCRANGE DEPENDENT
  //  float LSB = 5.0e-6;
  //  if (_ADCRange == 1) LSB = 1.25e-6;
  return _readRegister(INA228_SUVL, 2);
}

void INA228::setBusOvervoltageTH(uint16_t threshold)
{
  if (threshold > 0x7FFF) return;  //  false;
  //float LSB = 3.125e-3;  //  3.125 mV/LSB.
  _writeRegister(INA228_BOVL, threshold);
}

uint16_t INA228::getBusOvervoltageTH()
{
  //float LSB = 3.125e-3;  //  3.125 mV/LSB.
  return _readRegister(INA228_BOVL, 2);
}

void INA228::setBusUndervoltageTH(uint16_t threshold)
{
  if (threshold > 0x7FFF) return;
  //float LSB = 3.125e-3;  //  3.125 mV/LSB.
  _writeRegister(INA228_BUVL, threshold);
}

uint16_t INA228::getBusUndervoltageTH()
{
  //float LSB = 3.125e-3;  //  3.125 mV/LSB.
  return _readRegister(INA228_BUVL, 2);
}

void INA228::setTemperatureOverLimitTH(uint16_t threshold)
{
  //float LSB = 7.8125e-3;  //  milli degrees Celsius
  _writeRegister(INA228_TEMP_LIMIT, threshold);
}

uint16_t INA228::getTemperatureOverLimitTH()
{
  //float LSB = 7.8125e-3;  //  milli degrees Celsius
  return _readRegister(INA228_TEMP_LIMIT, 2);
}

void INA228::setPowerOverLimitTH(uint16_t threshold)
{
  //  P29
  //  Conversion factor: 256 × Power LSB.
  _writeRegister(INA228_POWER_LIMIT, threshold);
}

uint16_t INA228::getPowerOverLimitTH()
{
  //  P29
  //  Conversion factor: 256 × Power LSB.
  return _readRegister(INA228_POWER_LIMIT, 2);
}


////////////////////////////////////////////////////////
//
//  FLOAT-BASED THRESHOLD APIs
//
bool INA228::setShuntOvervoltageLimit_mV(float millivolts)
{
  float lsb_mv = _ADCRange ? 1.25e-3f : 5.0e-3f;
  float raw_f = millivolts / lsb_mv;
  if (raw_f < 0.0f || raw_f > 32767.0f) return false;
  setShuntOvervoltageTH(static_cast<uint16_t>(raw_f));
  return true;
}

bool INA228::setShuntUndervoltageLimit_mV(float millivolts)
{
  float lsb_mv = _ADCRange ? 1.25e-3f : 5.0e-3f;
  float raw_f = millivolts / lsb_mv;
  if (raw_f < -32768.0f || raw_f > 32767.0f) return false;
  setShuntUndervoltageTH(static_cast<uint16_t>(static_cast<int16_t>(raw_f)));
  return true;
}

bool INA228::setBusOvervoltageLimit_mV(float millivolts)
{
  float raw_f = millivolts / 3.125f;
  if (raw_f < 0.0f || raw_f > 32767.0f) return false;
  setBusOvervoltageTH(static_cast<uint16_t>(raw_f));
  return true;
}

bool INA228::setBusUndervoltageLimit_mV(float millivolts)
{
  float raw_f = millivolts / 3.125f;
  if (raw_f < 0.0f || raw_f > 32767.0f) return false;
  setBusUndervoltageTH(static_cast<uint16_t>(raw_f));
  return true;
}

bool INA228::setCurrentLimit_A(float amps)
{
  if (amps <= 0.0f || amps > _maxCurrent) return false;
  float v_shunt_mv = amps * _shunt * 1000.0f;
  return setShuntOvervoltageLimit_mV(v_shunt_mv);
}


////////////////////////////////////////////////////////
//
//  DIAGNOSTIC FLAG HELPERS
//
bool INA228::hasMathOverflow()
{
  return getDiagnoseAlertBit(INA228_DIAG_MATH_OVERFLOW) != 0;
}

bool INA228::hasEnergyOverflow()
{
  return getDiagnoseAlertBit(INA228_DIAG_ENERGY_OVERFLOW) != 0;
}

bool INA228::hasChargeOverflow()
{
  return getDiagnoseAlertBit(INA228_DIAG_CHARGE_OVERFLOW) != 0;
}


////////////////////////////////////////////////////////
//
//  MANUFACTURER and ID REGISTER 3E/3F
//
uint16_t INA228::getManufacturer()
{
  uint16_t value = _readRegister(INA228_MANUFACTURER, 2);
  return value;
}

uint16_t INA228::getDieID()
{
  uint16_t value = _readRegister(INA228_DEVICE_ID, 2);
  return (value >> 4) & 0x0FFF;
}

uint16_t INA228::getRevision()
{
  uint16_t value = _readRegister(INA228_DEVICE_ID, 2);
  return value & 0x000F;
}


////////////////////////////////////////////////////////
//
//  ERROR HANDLING
//
int INA228::getLastError()
{
  int e = _error;
  _error = 0;
  return e;
}


////////////////////////////////////////////////////////
//
//  PRIVATE
//
uint32_t INA228::_readRegister(uint8_t reg, uint8_t bytes)
{
  _error = 0;
  
  // Write register pointer
  int ret = i2c_write_blocking(_i2c, _address, &reg, 1, true); // true = no stop (repeated start)
  if (ret < 0)
  {
    _error = -1;
    return 0;
  }

  uint8_t buffer[8]; // Enough for largest register
  if (bytes > 8) bytes = 8; // Safety

  // Read data
  ret = i2c_read_blocking(_i2c, _address, buffer, bytes, false);
  
  if (ret < 0)
  {
    _error = -2;
    return 0;
  }

  uint32_t value = 0;
  for (int i = 0; i < bytes; i++)
  {
    value <<= 8;
    value |= buffer[i];
  }
  
  return value;
}


//  always 5 bytes
double INA228::_readRegisterF(uint8_t reg, char mode)
{
  _error = 0;
  
  // Write register pointer
  int ret = i2c_write_blocking(_i2c, _address, &reg, 1, true);
  if (ret < 0)
  {
    _error = -1;
    return 0;
  }

  uint8_t buffer[5];
  
  // Read 5 bytes
  ret = i2c_read_blocking(_i2c, _address, buffer, 5, false);

  if (ret < 0)
  {
    _error = -2;
    return 0;
  }

  double value = 0;
  uint32_t val = 0;
  
  //  fetch 4 MSB bytes first.
  for (int i = 0; i < 4; i++)
  {
    val <<= 8;
    val |= buffer[i];
  }
  
  //  handle signed / unsigned by casting.
  if (mode == 'U') value = val;
  else             value = (int32_t) val;
  
  //  process last byte
  value *= 256;
  //  note: mar05c
  uint8_t n = buffer[4];
  value += n;

  return value;
}


uint16_t INA228::_writeRegister(uint8_t reg, uint16_t value)
{
  uint8_t buffer[3];
  buffer[0] = reg;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value & 0xFF);

  // i2c_write_blocking returns number of bytes written or error code
  int n = i2c_write_blocking(_i2c, _address, buffer, 3, false);
  
  if (n != 3)
  {
    _error = -1;
    return 1; // Return non-zero to indicate failure, matching original API spirit
  }
  return 0; // Success
}