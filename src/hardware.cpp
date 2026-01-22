#include "hardware.h"
#include <stdio.h>

Hardware hw;

Hardware::Hardware() :
    btn1(Board::PIN_BTN_1, ButtonPull::PULL_NONE, 50, true),
    btn2(Board::PIN_BTN_2, ButtonPull::PULL_NONE, 50, true),
    btnEnc(Board::PIN_ENC_BTN, ButtonPull::PULL_NONE, 50, true),
    over_current(Board::PIN_SWITCH_EN_READ, IOMode::INPUT),
    encoder(Board::PIN_ENC_A, Board::PIN_ENC_B),
    debugLed(Board::PIN_DEBUG_LED, IOMode::OUTPUT),
    EN_17V(Board::PIN_17V_EN, IOMode::OUTPUT),
    loadSwitch(Board::PIN_SWITCH_EN, IOMode::OUTPUT),
    buzzer(Board::PIN_BUZZER),
    rgbLed(Board::PIN_RGB_LED, pio0, Config::LED_IS_RGBW),
    pdController(i2c0, Board::I2C_ADDR_TPS26750),
    powerMonitor(Board::I2C_ADDR_INA228, i2c0, Config::INA228_SHUNT_RESISTOR, Config::INA228_MAX_CURRENT),
    display(spi0, Board::PIN_LCD_CS, Board::PIN_LCD_DC, Board::PIN_LCD_RST, Board::PIN_LCD_BL)
{}

void Hardware::init() {
    stdio_init_all();

    // I2C Init
    i2c_init(i2c0, 400 * 1000); // 400kHz
    gpio_set_function(Board::PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(Board::PIN_I2C_SCL, GPIO_FUNC_I2C);

    // SPI Init (Display)
    spi_init(spi0, 10 * 1000 * 1000); // 10MHz
    spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(Board::PIN_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(Board::PIN_LCD_MOSI, GPIO_FUNC_SPI);

    // Driver Inits
    encoder.init();
    buzzer.init();
    adc.init();
    pdController.init();
    display.init();

    // INA228 Power Monitor Init
    if (!powerMonitor.init()) {
        printf("ERROR: INA228 Power Monitor not connected!\n");
    } else {
        printf("INA228 Connected.\n");

        // Configure Overcurrent Threshold (40mA for testing)
        // The INA228 compares Shunt Voltage, not Current directly.
        // V_shunt = I * R_shunt
        float currentLimit = 0.040f; // 40mA
        float vShuntTarget = currentLimit * Config::INA228_SHUNT_RESISTOR;

        // Get LSB size based on current ADC Range setting
        // Range 0: 5.0 uV LSB | Range 1: 1.25 uV LSB
        bool highPrecision = powerMonitor.getADCRange();
        float lsbVal = highPrecision ? 1.25e-6f : 5.0e-6f;

        uint16_t rawThreshold = (uint16_t)(vShuntTarget / lsbVal);
        powerMonitor.setShuntOvervoltageTH(rawThreshold);

        // Configure ALERT Pin Behavior
        powerMonitor.setDiagnoseAlert(0x0000); // Reset alerts first
        powerMonitor.setDiagnoseAlertBit(INA228_DIAG_SHUNT_OVER_LIMIT); // Enable SOL alert
        powerMonitor.setDiagnoseAlertBit(INA228_DIAG_ALERT_LATCH); // Latch until read
    }

    // Initial Output States
    loadSwitch.off();
    debugLed.on();

    // Play startup tone
    buzzer.playTone(1000, 200);

    // Initial display test
    display.fillScreen(0x001F); // Blue background

    // RGB LED ready indication
    rgbLed.init();
    rgbLed.setColor(255, 0, 0, 50);
    rgbLed.startBlink(125, 1000);
}

void Hardware::update() {
    // Poll buttons etc if needed
}
