#include "hardware.h"
#include "utils/logging.h"

Hardware hw;

Hardware::Hardware() :
    btn1(Board::PIN_BTN_1, ButtonPull::PULL_NONE, 50, true),
    btn2(Board::PIN_BTN_2, ButtonPull::PULL_NONE, 50, true),
    btnEnc(Board::PIN_ENC_BTN, ButtonPull::PULL_NONE, 50, true),
    overcurrentAlert(Board::PIN_SWITCH_EN_READ, IOMode::INPUT),
    pdInterrupt(Board::PIN_USB_PD_IRQ, IOMode::INPUT),
    encoder(Board::PIN_ENC_A, Board::PIN_ENC_B),
    debugLed(Board::PIN_DEBUG_LED, IOMode::OUTPUT),
    EN_17V(Board::PIN_17V_EN, IOMode::OUTPUT),
    loadSwitch(Board::PIN_SWITCH_EN, IOMode::OUTPUT),
    buzzer(Board::PIN_BUZZER),
    rgbLed(Board::PIN_RGB_LED, pio0, Board::LED_IS_RGBW),
    pdController(i2c0, Board::I2C_ADDR_TPS26750),
    powerMonitor(Board::I2C_ADDR_INA228, i2c0, Board::INA228_SHUNT_RESISTOR, Board::INA228_MAX_CURRENT),
    display(spi0, Board::PIN_LCD_CS, Board::PIN_LCD_DC, Board::PIN_LCD_RST, Board::PIN_LCD_BL)
{}

void Hardware::init() {
    // Sleep to let time to the TPS26750 to negotiate power:
    sleep_ms(250);
    stdio_init_all();

    // =========================================================================
    // Communication Bus Init
    // =========================================================================

    // I2C0 (400kHz) - for INA228 and TPS26750
    i2c_init(i2c0, Board::I2C_SPEED_HZ);
    gpio_set_function(Board::PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(Board::PIN_I2C_SCL, GPIO_FUNC_I2C);

    // SPI0 (10MHz) - for ST7789 display
    spi_init(spi0, Board::SPI_SPEED_HZ);
    spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(Board::PIN_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(Board::PIN_LCD_MOSI, GPIO_FUNC_SPI);

    // =========================================================================
    // Peripheral Driver Init
    // =========================================================================

    // Simple drivers (no external communication)
    encoder.init();
    buzzer.init();
    LOG_HW_INIT("ADC", adc.init());

    // Display (SPI)
    LOG_HW_INIT("ST7789 Display", display.init());

    // I2C devices
    LOG_HW_INIT("TPS26750 PD Controller", pdController.init());
    LOG_HW_INIT("INA228 Power Monitor", powerMonitor.init());

    // RGB LED (PIO)
    LOG_HW_INIT("SK6812 RGB LED", rgbLed.init());

    // =========================================================================
    // Initial Output States
    // =========================================================================
    loadSwitch.off();
    rgbLed.setColor(0, 255, 0, 50);  // Green = ready
}

void Hardware::update() {
    // Update drivers that need periodic polling
    rgbLed.update();
    debugLed.update();
}
