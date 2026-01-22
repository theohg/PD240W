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
    pdController(i2c0), 
    powerMonitor(0x40, i2c0), 
    display(spi0, Board::PIN_LCD_CS, Board::PIN_LCD_DC, Board::PIN_LCD_RST, Board::PIN_LCD_BL)
{}

void Hardware::init() {
    stdio_init_all();
    
    // I2C Init
    i2c_init(i2c0, 400 * 1000); // 400kHz
    gpio_set_function(Board::PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(Board::PIN_I2C_SCL, GPIO_FUNC_I2C);

    // SPI Init (Display)
    // Note: ST7789 max is typically 10MHz for reliable operation
    // If display still doesn't work, try reducing to 4MHz
    spi_init(spi0, 10 * 1000 * 1000); // 10MHz (reduced from 24MHz)
    // spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); // 8-bit, Mode 0
    spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    printf("[SPI0] Initialized at 10MHz, Mode 1 (CPOL=1, CPHA=1)\n");
    gpio_set_function(Board::PIN_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(Board::PIN_LCD_MOSI, GPIO_FUNC_SPI);
    
    // Driver Inits
    encoder.init();
    buzzer.init();
    adc.init();
    pdController.init();
    display.init();

    // --- INA228 SETUP ---
    // Using .begin() instead of .init() based on the ported library
    if (!powerMonitor.begin()) {
        printf("ERROR: INA228 Power Monitor not connected!\n");
    } else {
        printf("INA228 Connected.\n");
        
        // IMPORTANT: Set your Shunt Resistor and Max Current here!
        // 8 milliOhm shunt, Max expected 5 Amps
        powerMonitor.setMaxCurrentShunt(5.0, 0.008); 
        
        // Optional: Average 16 samples for stable readings
        powerMonitor.setAverage(INA228_16_SAMPLES); 

        // 2. Configure Overcurrent Threshold (40mA)
        // The INA228 compares Shunt Voltage, not Current directly.
        // V_shunt = I * R = 0.040A * 0.015 Ohm = 0.0006V (600uV)
        
        float currentLimit = 0.040; // 40mA
        float vShuntTarget = currentLimit * 0.008;

        // Get LSB size based on current ADC Range setting
        // Range 0: 5.0 uV LSB | Range 1: 1.25 uV LSB
        bool highPrecision = powerMonitor.getADCRange(); 
        float lsbVal = highPrecision ? 1.25e-6 : 5.0e-6;

        uint16_t rawThreshold = (uint16_t)(vShuntTarget / lsbVal);
        
        // Write the threshold to the Shunt Over-Voltage Limit register
        powerMonitor.setShuntOvervoltageTH(rawThreshold);

        // 3. Configure ALERT Pin Behavior
        // Reset alerts first
        powerMonitor.setDiagnoseAlert(0x0000); 
        
        // Enable Shunt Over Limit (SOL) alert
        powerMonitor.setDiagnoseAlertBit(INA228_DIAG_SHUNT_OVER_LIMIT);
        
        // Enable Latch (ALATCH) - Alert stays low until read, ensuring we don't miss it
        powerMonitor.setDiagnoseAlertBit(INA228_DIAG_ALERT_LATCH);
        
        // Polarity: Default is Active Low (Open Drain). Ensure MCU pin is pulled UP.
        // powerMonitor.setDiagnoseAlertBit(INA228_DIAG_ALERT_POLARITY); // Uncomment only if you need Active High
    }

    // Initial Output States
    loadSwitch.off(); 
    debugLed.on();

    //Play small melody to indicate startup
    buzzer.playTone(1000, 200);

    // make the lcd turn blue
    display.fillScreen(0x001F); // Blue background check

    // Light up RGB LED to indicate ready
    rgbLed.init();
    rgbLed.setColor(255, 0, 0, 50); // Medium brightness 
    rgbLed.startBlink(125, 1000);
}

void Hardware::update() {
    // Poll buttons etc if needed
}
