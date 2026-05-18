#pragma once

#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include "src/drivers/display/aa_font.h"

// ST7789 LCD Driver for PD240W
// Resolution: 240x320 (2.4" display, model HS20HS072RX)

class ST7789 {
public:
    // Display dimensions
    static constexpr uint16_t WIDTH = 240;
    static constexpr uint16_t HEIGHT = 320;

    // Common RGB565 colors
    static constexpr uint16_t COLOR_BLACK   = 0x0000;
    static constexpr uint16_t COLOR_WHITE   = 0xFFFF;
    static constexpr uint16_t COLOR_RED     = 0xF800;
    static constexpr uint16_t COLOR_GREEN   = 0x07E0;
    static constexpr uint16_t COLOR_BLUE    = 0x001F;
    static constexpr uint16_t COLOR_YELLOW  = 0xFFE0;
    static constexpr uint16_t COLOR_CYAN    = 0x07FF;
    static constexpr uint16_t COLOR_MAGENTA = 0xF81F;

    ST7789(spi_inst_t* spi, uint pinCS, uint pinDC, uint pinRST, uint pinBL);

    // Initialization (returns true on success)
    bool init();

    // Backlight control
    void setBacklight(bool on);
    void setBacklightBrightness(uint8_t percent);  // 0-100%

    // Basic drawing
    void fillScreen(uint16_t color);
    void drawPixel(int16_t x, int16_t y, uint16_t color);

    // Shapes
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
    void fillGradientRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t start_color, uint16_t end_color);
    void fillRoundRectGradient(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                               uint16_t start_color, uint16_t end_color);
    void fillRoundRectGradientColumns(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                                      int16_t start_column, int16_t end_column,
                                      uint16_t start_color, uint16_t end_color);

    // Text rendering
    void drawChar(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size = 1);
    void drawString(int16_t x, int16_t y, const char* str, uint16_t color, uint16_t bg, uint8_t size = 1);
    // Number rendering with formatting
    void drawInt(int16_t x, int16_t y, int value, uint16_t color, uint16_t bg, uint8_t size = 1);
    void drawFloat(int16_t x, int16_t y, float value, uint8_t decimals, uint16_t color, uint16_t bg, uint8_t size = 1);

    // Anti-aliased text rendering (4-bit alpha blended)
    void drawCharAA(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, const AAFont* font);
    void drawStringAA(int16_t x, int16_t y, const char* str, uint16_t color, uint16_t bg, const AAFont* font);
    static int getStringWidthAA(const char* str, const AAFont* font);

    // Bitmap drawing
    void drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data);
    void drawBitmapScaled(int16_t x, int16_t y, int16_t out_w, int16_t out_h,
                          int16_t src_w, int16_t src_h, const uint16_t* data);

    // Utility
    static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

private:
    spi_inst_t* _spi;
    uint _pinCS, _pinDC, _pinRST, _pinBL;
    uint _pwm_slice;  // PWM slice for backlight brightness control

    // Low-level communication
    void writeCommand(uint8_t cmd);
    void writeData(uint8_t data);
    void writeData16(uint16_t data);
    void setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

    // Helper for fillRoundRect
    void fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                          uint8_t cornermask, int16_t delta, uint16_t color);
    void drawVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
};
