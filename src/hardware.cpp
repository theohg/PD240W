#include "hardware.h"
#include "config/app_config.h"
#include "utils/logging.h"
#include "config/version.h"
#include "utils/tps_eeprom_loader.h"

Hardware hw;

Hardware::Hardware() :
    btn1(Board::PIN_BTN_1, ButtonPull::PULL_NONE, 10, true),
    btn2(Board::PIN_BTN_2, ButtonPull::PULL_NONE, 10, true),
    btnEnc(Board::PIN_ENC_BTN, ButtonPull::PULL_NONE, 50, true),
    overcurrentAlert(Board::PIN_SWITCH_EN_READ, IOMode::INPUT),
    pdInterrupt(Board::PIN_USB_PD_IRQ, IOMode::INPUT),
    encoder(Board::PIN_ENC_A, Board::PIN_ENC_B),
    EN_17V(Board::PIN_17V_EN, IOMode::OUTPUT),
    loadSwitch(Board::PIN_SWITCH_EN, IOMode::OUTPUT),
    buzzer(Board::PIN_BUZZER),
    rgbLed(Board::PIN_RGB_LED, pio0, Board::LED_IS_RGBW),
    pdController(i2c0, Board::I2C_ADDR_TPS26750),
    powerMonitor(i2c0, Board::I2C_ADDR_INA228, Board::INA228_SHUNT_RESISTOR, Board::INA228_MEASUREMENT_MAX_CURRENT, Board::INA228_SHUNT_TEMPCO_PPM),
    display(spi0, Board::PIN_LCD_CS, Board::PIN_LCD_DC, Board::PIN_LCD_RST, Board::PIN_LCD_BL)
{}

void Hardware::init() {
    // Short delay to let TPS26750 start up - actual PD negotiation is handled
    // adaptively during the boot screen via pdManager.waitForPdos()
    sleep_ms(50);
    stdio_init_all();

    LOG_SEPARATOR();
    LOG_INFO("%s %s - %s", Version::PRODUCT_NAME, Version::PRODUCT_SUBTITLE, Version::FIRMWARE_VERSION);
    LOG_SEPARATOR();

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
    if (powerMonitor.init()) {
        if (!powerMonitor.setADCRange(Board::INA228_USE_LOW_ADC_RANGE)) {
            LOG_WARN("INA228 ADC range configuration failed");
        }
        LOG_INFO("INA228 Power monitor initialized successfully with FW %s",
                 Version::INA_FIRMWARE_VERSION);
    } else {
        LOG_ERROR("INA228 Power monitor initialization FAILED");
    }

    // RGB LED (PIO)
    LOG_HW_INIT("SK6812 RGB LED", rgbLed.init());

    // =========================================================================
    // Initial Output States
    // =========================================================================
    loadSwitch.off();
    rgbLed.setColor(LedColor::BLUE, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
}

void Hardware::update() {
    // Update drivers that need periodic polling
    rgbLed.update();
}
