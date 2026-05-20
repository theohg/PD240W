#include "drivers/display/st7789.h"
#include "drivers/display/font.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include <stdint.h>
#include <stdio.h>   // For snprintf (number formatting)
#include <stdlib.h>
#include <cmath>

namespace {

uint8_t expand5To8(uint8_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t expand6To8(uint8_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

int16_t computeRoundRectInset(int16_t dx, int16_t w, int16_t r) {
    if (r <= 0 || (dx >= r && dx < w - r)) {
        return 0;
    }

    int16_t corner_dx = (dx < r) ? (r - 1 - dx) : (dx - (w - r));
    int32_t radius = r - 1;
    int32_t inside = radius * radius - corner_dx * corner_dx;
    int16_t y_extent = 0;

    while ((y_extent + 1) * (y_extent + 1) <= inside) {
        ++y_extent;
    }

    return static_cast<int16_t>(radius - y_extent);
}

}  // namespace

ST7789::ST7789(spi_inst_t* spi, uint pinCS, uint pinDC, uint pinRST, uint pinBL)
    : _spi(spi), _pinCS(pinCS), _pinDC(pinDC), _pinRST(pinRST), _pinBL(pinBL), _pwm_slice(0) {}

bool ST7789::init() {
    // Initialize GPIOs
    gpio_init(_pinCS);  gpio_set_dir(_pinCS, GPIO_OUT);  gpio_put(_pinCS, 1);
    gpio_init(_pinDC);  gpio_set_dir(_pinDC, GPIO_OUT);  gpio_put(_pinDC, 1);
    gpio_init(_pinRST); gpio_set_dir(_pinRST, GPIO_OUT); gpio_put(_pinRST, 1);
    
    // Initialize backlight with PWM for brightness control
    gpio_set_function(_pinBL, GPIO_FUNC_PWM);
    _pwm_slice = pwm_gpio_to_slice_num(_pinBL);
    pwm_set_wrap(_pwm_slice, 255);  // 8-bit resolution
    pwm_set_gpio_level(_pinBL, 0);  // Start OFF to hide ghost image
    pwm_set_enabled(_pwm_slice, true);

    // 1. Hardware Reset Sequence (LCD.pdf Page 19 recommends ~100ms+ delays)
    gpio_put(_pinRST, 1); sleep_ms(100);
    gpio_put(_pinRST, 0); sleep_ms(100);
    gpio_put(_pinRST, 1); sleep_ms(200);

    // 2. Sleep Out
    writeCommand(0x11); 
    sleep_ms(120);      // Datasheet says delay 120ms required here

    // 3. Memory Data Access Control (MADCTL)
    // 0x00 = Vertical, normal orientation
    // 0xC0 = Vertical, 180° rotation (MY=1, MX=1)
    // 0xA0 = Horizontal (if you want landscape)
    writeCommand(0x36);
    writeData(0xC0);    // MY=1, MX=1 for 180° rotation    

    // 4. Pixel Format Set (16-bit color)
    writeCommand(0x3A); 
    writeData(0x05); 

    // 5. Porch Setting
    writeCommand(0xB2);
    writeData(0x0C);
    writeData(0x0C);
    writeData(0x00);
    writeData(0x33);
    writeData(0x33);

    // 6. Gate Control
    writeCommand(0xB7); 
    writeData(0x35); 

    // 7. VCOM Setting
    writeCommand(0xBB); 
    writeData(0x20); // Original code had 0x19, Datasheet p.19 says 0x20

    // 8. LCM Control
    writeCommand(0xC0); 
    writeData(0x2C); 

    // 9. VDV and VRH Command Enable
    writeCommand(0xC2); 
    writeData(0x01); 

    // 10. VRH Set
    writeCommand(0xC3); 
    writeData(0x0B); // Datasheet p.19 says 0x0B

    // 11. VDV Set
    writeCommand(0xC4); 
    writeData(0x20); 

    // 12. Frame Rate Control (60Hz)
    writeCommand(0xC6); 
    writeData(0x0F); 

    // 13. Power Control 1
    writeCommand(0xD0); 
    writeData(0xA4);
    writeData(0xA1);

    // 14. Positive Voltage Gamma Control (Page 19-20)
    writeCommand(0xE0);
    writeData(0xD0);
    writeData(0x03);
    writeData(0x09);
    writeData(0x0E);
    writeData(0x11);
    writeData(0x3D);
    writeData(0x47);
    writeData(0x55);
    writeData(0x53);
    writeData(0x1A);
    writeData(0x16);
    writeData(0x14);
    writeData(0x1F);
    writeData(0x22);

    // 15. Negative Voltage Gamma Control (Page 20)
    writeCommand(0xE1);
    writeData(0xD0);
    writeData(0x02);
    writeData(0x08);
    writeData(0x0D);
    writeData(0x12);
    writeData(0x2C);
    writeData(0x43);
    writeData(0x55);
    writeData(0x53);
    writeData(0x1E);
    writeData(0x1B);
    writeData(0x19);
    writeData(0x20);
    writeData(0x22);

    // 16. Display Inversion
    // IMPORTANT: Datasheet does not explicitly set INVON (0x21). 
    // If colors look inverted (Black is White), uncomment the line below.
    writeCommand(0x21); 

    // 17. Display ON
    writeCommand(0x29); 
    sleep_ms(20);

    // 18. Clear screen to black immediately
    fillScreen(COLOR_BLACK);

    // 19. Backlight stays OFF - will be turned on after first frame is rendered
    // This prevents any ghost image or uninitialized content from being visible

    return true;
}

void ST7789::setBacklight(bool on) {
    // Use PWM level: 255 = full on, 0 = off
    pwm_set_gpio_level(_pinBL, on ? 255 : 0);
}

void ST7789::setBacklightBrightness(uint8_t percent) {
    // Convert 0-100% to 0-255 PWM level with perceptual correction
    // Use square root curve (gamma 0.5) for more linear perceived brightness
    if (percent > 100) percent = 100;
    
    // Apply gamma 1.5 (softer than pure linear, avoids harsh low-end dropoff)
    // Formula: level = (percent/100)^1.5 * 255
    // Approximation using integer math: sqrt(p) * p / 100 * 255 / 10
    uint32_t p = percent;
    // Use lookup for common values or linear interpolation with floor
    uint32_t level;
    if (p == 0) {
        level = 0;
    } else if (p <= 10) {
        level = 8 + (p * 2);  // 10% -> 28 (~11% of 255)
    } else if (p <= 30) {
        level = 28 + ((p - 10) * 3);  // 30% -> 88 (~35% of 255)
    } else {
        // Above 30%, linear from 88 to 255
        level = 88 + ((p - 30) * 167 / 70);
    }
    if (level > 255) level = 255;
    
    pwm_set_gpio_level(_pinBL, level);
}

// ===== Low-level SPI communication =====

void ST7789::writeCommand(uint8_t cmd) {
    gpio_put(_pinCS, 0);  // CS low first
    gpio_put(_pinDC, 0);  // Command mode (DC/RS low)
    spi_write_blocking(_spi, &cmd, 1);
    gpio_put(_pinCS, 1);  // CS high to end transaction
}

void ST7789::writeData(uint8_t data) {
    gpio_put(_pinCS, 0);  // CS low first
    gpio_put(_pinDC, 1);  // Data mode (DC/RS high)
    spi_write_blocking(_spi, &data, 1);
    gpio_put(_pinCS, 1);  // CS high to end transaction
}

void ST7789::writeData16(uint16_t data) {
    uint8_t buffer[2] = {static_cast<uint8_t>(data >> 8), static_cast<uint8_t>(data & 0xFF)};
    gpio_put(_pinDC, 1);
    gpio_put(_pinCS, 0);
    spi_write_blocking(_spi, buffer, 2);
    gpio_put(_pinCS, 1);
}

void ST7789::setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // Column address set
    writeCommand(0x2A);
    writeData(x0 >> 8);
    writeData(x0 & 0xFF);
    writeData(x1 >> 8);
    writeData(x1 & 0xFF);

    // Row address set
    writeCommand(0x2B);
    writeData(y0 >> 8);
    writeData(y0 & 0xFF);
    writeData(y1 >> 8);
    writeData(y1 & 0xFF);

    // Write to RAM
    writeCommand(0x2C);
}

// ===== Basic drawing =====

void ST7789::fillScreen(uint16_t color) {
    fillRect(0, 0, WIDTH, HEIGHT, color);
}

void ST7789::drawPixel(int16_t x, int16_t y, uint16_t color) {
    // Bounds checking
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

    setAddressWindow(x, y, x, y);
    writeData16(color);
}

// ===== Shapes =====

void ST7789::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    // Bresenham's line algorithm
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    while (true) {
        drawPixel(x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int16_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ST7789::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // Draw four lines to form rectangle outline
    drawLine(x, y, x + w - 1, y, color);         // Top
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color); // Right
    drawLine(x + w - 1, y + h - 1, x, y + h - 1, color); // Bottom
    drawLine(x, y + h - 1, x, y, color);         // Left
}

void ST7789::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // Bounds checking and clipping
    if (x >= WIDTH || y >= HEIGHT) return;
    if (x + w > WIDTH) w = WIDTH - x;
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;

    setAddressWindow(x, y, x + w - 1, y + h - 1);

    // Write pixels efficiently in blocks
    gpio_put(_pinDC, 1); // Data mode
    gpio_put(_pinCS, 0);

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    uint8_t buffer[64];  // Buffer for faster SPI writes

    // Fill buffer with color pattern
    for (int i = 0; i < 64; i += 2) {
        buffer[i] = hi;
        buffer[i + 1] = lo;
    }

    uint32_t total_pixels = w * h;
    uint32_t blocks = total_pixels / 32; // 32 pixels per buffer (64 bytes)
    uint32_t remainder = total_pixels % 32;

    // Write full blocks
    for (uint32_t i = 0; i < blocks; i++) {
        spi_write_blocking(_spi, buffer, 64);
    }

    // Write remaining pixels
    for (uint32_t i = 0; i < remainder; i++) {
        spi_write_blocking(_spi, buffer, 2);
    }

    gpio_put(_pinCS, 1);
}

void ST7789::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    // Draw the straight edges (inset by radius)
    drawLine(x + r, y, x + w - r - 1, y, color);              // Top
    drawLine(x + r, y + h - 1, x + w - r - 1, y + h - 1, color); // Bottom
    drawLine(x, y + r, x, y + h - r - 1, color);              // Left
    drawLine(x + w - 1, y + r, x + w - 1, y + h - r - 1, color); // Right

    // Draw four corners using midpoint circle algorithm
    int16_t cx1 = x + r;
    int16_t cy1 = y + r;
    int16_t cx2 = x + w - r - 1;
    int16_t cy2 = y + h - r - 1;

    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t px = 0;
    int16_t py = r;

    while (px <= py) {
        // Top-left corner
        drawPixel(cx1 - py, cy1 - px, color);
        drawPixel(cx1 - px, cy1 - py, color);
        // Top-right corner
        drawPixel(cx2 + py, cy1 - px, color);
        drawPixel(cx2 + px, cy1 - py, color);
        // Bottom-left corner
        drawPixel(cx1 - py, cy2 + px, color);
        drawPixel(cx1 - px, cy2 + py, color);
        // Bottom-right corner
        drawPixel(cx2 + py, cy2 + px, color);
        drawPixel(cx2 + px, cy2 + py, color);

        if (f >= 0) {
            py--;
            ddF_y += 2;
            f += ddF_y;
        }
        px++;
        ddF_x += 2;
        f += ddF_x;
    }
}

void ST7789::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
    // Clamp radius to half of smallest dimension
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        fillRect(x, y, w, h, color);
        return;
    }

    // Fill the central rectangle (full width, middle portion)
    fillRect(x, y + r, w, h - 2 * r, color);

    // Fill the corners using the helper
    fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);  // Right side
    fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);          // Left side

    // Fill top and bottom strips between corners
    fillRect(x + r, y, w - 2 * r, r, color);
    fillRect(x + r, y + h - r, w - 2 * r, r, color);
}

void ST7789::fillGradientRect(int16_t x, int16_t y, int16_t w, int16_t h,
                              uint16_t start_color, uint16_t end_color) {
    // Route directly to the updated columns function with 0 radius
    fillRoundRectGradientColumns(x, y, w, h, 0, 0, w, start_color, end_color);
}

void ST7789::fillRoundRectGradient(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                                   uint16_t start_color, uint16_t end_color) {
    fillRoundRectGradientColumns(x, y, w, h, r, 0, w, start_color, end_color);
}

void ST7789::fillRoundRectGradientColumns(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                                         int16_t start_column, int16_t end_column,
                                         uint16_t start_color, uint16_t end_color) {
    if (w <= 0 || h <= 0) return;

    if (start_column < 0) start_column = 0;
    if (end_column > w) end_column = w;
    if (start_column >= end_column) return;

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    // Fast exit for solid colors to save processing
    if (start_color == end_color || w == 1) {
        for (int16_t dx = start_column; dx < end_column; ++dx) {
            int16_t inset = computeRoundRectInset(dx, w, r);
            int16_t column_height = h - inset * 2;
            if (column_height > 0) {
                fillRect(x + dx, y + inset, 1, column_height, start_color);
            }
        }
        return;
    }

    // Extract native 8-bit color channels
    int sr = expand5To8((start_color >> 11) & 0x1F);
    int sg = expand6To8((start_color >> 5) & 0x3F);
    int sb = expand5To8(start_color & 0x1F);
    
    int er = expand5To8((end_color >> 11) & 0x1F);
    int eg = expand6To8((end_color >> 5) & 0x3F);
    int eb = expand5To8(end_color & 0x1F);

    // Square the start/end values for Gamma-Corrected (sRGB) blending
    int32_t sr2 = sr * sr;
    int32_t sg2 = sg * sg;
    int32_t sb2 = sb * sb;
    int32_t er2 = er * er;
    int32_t eg2 = eg * eg;
    int32_t eb2 = eb * eb;

    // 8x8 Bayer Matrix for ultra-smooth Ordered Dithering (64 levels)
    static const uint8_t bayer[8][8] = {
        {  0, 32,  8, 40,  2, 34, 10, 42 },
        { 48, 16, 56, 24, 50, 18, 58, 26 },
        { 12, 44,  4, 36, 14, 46,  6, 38 },
        { 60, 28, 52, 20, 62, 30, 54, 22 },
        {  3, 35, 11, 43,  1, 33,  9, 41 },
        { 51, 19, 59, 27, 49, 17, 57, 25 },
        { 15, 47,  7, 39, 13, 45,  5, 37 },
        { 63, 31, 55, 23, 61, 29, 53, 21 }
    };

    int32_t max_step = w - 1;

    for (int16_t dx = start_column; dx < end_column; ++dx) {
        int16_t inset = computeRoundRectInset(dx, w, r);
        int16_t column_height = h - inset * 2;
        if (column_height <= 0) continue;

        // Gamma-correct interpolation: computed once per column, so std::sqrt is very fast
        int r8 = std::sqrt(sr2 + ((er2 - sr2) * dx) / max_step);
        int g8 = std::sqrt(sg2 + ((eg2 - sg2) * dx) / max_step);
        int b8 = std::sqrt(sb2 + ((eb2 - sb2) * dx) / max_step);

        int16_t cx = x + dx;
        int16_t cy = y + inset;

        setAddressWindow(cx, cy, cx, cy + column_height - 1);
        gpio_put(_pinDC, 1);
        gpio_put(_pinCS, 0);

        uint8_t line_buf[480]; 
        
        for (int16_t dy = 0; dy < column_height; ++dy) {
            // Apply 8x8 noise matrix based on absolute screen coordinates
            uint8_t d = bayer[(cy + dy) & 7][cx & 7];
            
            // Red/Blue need up to +7 noise. Matrix is 0-63, so >> 3 scales it to 0-7.
            // Green needs up to +3 noise. Matrix is 0-63, so >> 4 scales it to 0-3.
            int r_val = r8 + (d >> 3);
            int g_val = g8 + (d >> 4);
            int b_val = b8 + (d >> 3);
            
            uint16_t r5 = (r_val > 255 ? 255 : r_val) >> 3;
            uint16_t g6 = (g_val > 255 ? 255 : g_val) >> 2;
            uint16_t b5 = (b_val > 255 ? 255 : b_val) >> 3;
            
            uint16_t color = (r5 << 11) | (g6 << 5) | b5;
            line_buf[dy * 2]     = color >> 8;
            line_buf[dy * 2 + 1] = color & 0xFF;
        }
        
        spi_write_blocking(_spi, line_buf, column_height * 2);
        gpio_put(_pinCS, 1);
    }
}

// Helper to fill rounded corners using vertical lines
// cornermask: bit 0 = right side (fills right), bit 1 = left side (fills left)
void ST7789::fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                               uint8_t cornermask, int16_t delta, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t px = 0;
    int16_t py = r;

    while (px <= py) {
        if (cornermask & 0x1) {
            // Right side corners
            drawVLine(x0 + px, y0 - py, 2 * py + 1 + delta, color);
            drawVLine(x0 + py, y0 - px, 2 * px + 1 + delta, color);
        }
        if (cornermask & 0x2) {
            // Left side corners
            drawVLine(x0 - px, y0 - py, 2 * py + 1 + delta, color);
            drawVLine(x0 - py, y0 - px, 2 * px + 1 + delta, color);
        }

        if (f >= 0) {
            py--;
            ddF_y += 2;
            f += ddF_y;
        }
        px++;
        ddF_x += 2;
        f += ddF_x;
    }
}

// Optimized vertical line
void ST7789::drawVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    if (h <= 0) return;
    fillRect(x, y, 1, h, color);
}

// ===== Text rendering =====

void ST7789::drawChar(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, uint8_t size) {
    const uint8_t* char_data = getFontChar(c);

    for (uint8_t col = 0; col < FONT_WIDTH; col++) {
        uint8_t column = char_data[col];

        for (uint8_t row = 0; row < FONT_HEIGHT; row++) {
            bool pixel_on = column & (1 << row);

            // Draw scaled pixel
            if (size == 1) {
                drawPixel(x + col, y + row, pixel_on ? color : bg);
            } else {
                fillRect(x + col * size, y + row * size, size, size, pixel_on ? color : bg);
            }
        }
    }

    // Add spacing column
    if (size == 1) {
        for (uint8_t row = 0; row < FONT_HEIGHT; row++) {
            drawPixel(x + FONT_WIDTH, y + row, bg);
        }
    } else {
        fillRect(x + FONT_WIDTH * size, y, size, FONT_HEIGHT * size, bg);
    }
}

void ST7789::drawString(int16_t x, int16_t y, const char* str, uint16_t color, uint16_t bg, uint8_t size) {
    int16_t cursor_x = x;
    int16_t char_width = (FONT_WIDTH + 1) * size; // +1 for spacing

    while (*str) {
        if (*str == '\n') {
            cursor_x = x;
            y += (FONT_HEIGHT + 1) * size;
        } else {
            drawChar(cursor_x, y, *str, color, bg, size);
            cursor_x += char_width;
        }
        str++;
    }
}

// ===== Number rendering =====

void ST7789::drawInt(int16_t x, int16_t y, int value, uint16_t color, uint16_t bg, uint8_t size) {
    char buffer[12]; // Enough for INT_MIN
    snprintf(buffer, sizeof(buffer), "%d", value);
    drawString(x, y, buffer, color, bg, size);
}

void ST7789::drawFloat(int16_t x, int16_t y, float value, uint8_t decimals, uint16_t color, uint16_t bg, uint8_t size) {
    char buffer[20];
    char format[8];

    // Create format string like "%.2f" based on decimals parameter
    snprintf(format, sizeof(format), "%%.%df", decimals);
    snprintf(buffer, sizeof(buffer), format, value);

    drawString(x, y, buffer, color, bg, size);
}

// ===== Anti-aliased text rendering =====

// Blend a single RGB565 channel using 4-bit alpha (0-15)
static inline uint16_t blendRgb565(uint16_t fg, uint16_t bg, uint8_t alpha) {
    if (alpha == 0) return bg;
    if (alpha == 15) return fg;

    // Extract RGB565 channels
    uint8_t fg_r = (fg >> 11) & 0x1F;
    uint8_t fg_g = (fg >> 5) & 0x3F;
    uint8_t fg_b = fg & 0x1F;

    uint8_t bg_r = (bg >> 11) & 0x1F;
    uint8_t bg_g = (bg >> 5) & 0x3F;
    uint8_t bg_b = bg & 0x1F;

    // Blend: result = (fg * alpha + bg * (15 - alpha)) / 15
    uint8_t inv = 15 - alpha;
    uint8_t r = (fg_r * alpha + bg_r * inv) / 15;
    uint8_t g = (fg_g * alpha + bg_g * inv) / 15;
    uint8_t b = (fg_b * alpha + bg_b * inv) / 15;

    return (r << 11) | (g << 5) | b;
}

void ST7789::drawCharAA(int16_t x, int16_t y, char c, uint16_t color, uint16_t bg, const AAFont* font) {
    if (c < font->firstChar || c > font->lastChar) return;

    const AAGlyph& glyph = font->glyphs[c - font->firstChar];

    // Render the full advance rectangle (xAdvance x lineHeight) in one SPI burst.
    // Background pixels outside the glyph bitmap get bg color.
    // Glyph pixels get alpha-blended color. No separate fillRect = no flicker.
    int16_t adv_w = glyph.xAdvance;
    int16_t adv_h = font->lineHeight;
    if (adv_w <= 0 || adv_h <= 0) return;

    // Clip to screen
    int16_t draw_x1 = x + adv_w - 1;
    int16_t draw_y1 = y + adv_h - 1;
    if (x >= WIDTH || y >= HEIGHT || draw_x1 < 0 || draw_y1 < 0) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (draw_x1 >= WIDTH) draw_x1 = WIDTH - 1;
    if (draw_y1 >= HEIGHT) draw_y1 = HEIGHT - 1;

    int16_t draw_w = draw_x1 - x + 1;
    int16_t draw_h = draw_y1 - y + 1;

    // Glyph bitmap position
    int16_t gx = x + glyph.xOffset;
    int16_t gy = y + glyph.yOffset;

    // Pre-compute bg pixel bytes
    uint8_t bg_hi = bg >> 8;
    uint8_t bg_lo = bg & 0xFF;

    setAddressWindow(x, y, draw_x1, draw_y1);

    gpio_put(_pinDC, 1);
    gpio_put(_pinCS, 0);

    uint8_t line_buf[480]; // Max 240 pixels * 2 bytes

    for (int16_t row = 0; row < draw_h; row++) {
        int16_t screen_y = y + row;
        int16_t src_y = screen_y - gy;

        for (int16_t col = 0; col < draw_w; col++) {
            int16_t screen_x = x + col;
            int16_t src_x = screen_x - gx;

            // Check if this pixel falls within the glyph bitmap
            if (glyph.width > 0 && glyph.height > 0 &&
                src_x >= 0 && src_x < glyph.width &&
                src_y >= 0 && src_y < glyph.height) {
                // Read 4-bit alpha from packed bitmap
                int pixel_idx = src_y * glyph.width + src_x;
                int byte_idx = glyph.dataOffset + (pixel_idx / 2);
                uint8_t packed = font->bitmap[byte_idx];
                uint8_t alpha = (pixel_idx % 2 == 0) ? (packed >> 4) : (packed & 0x0F);

                uint16_t pixel = blendRgb565(color, bg, alpha);
                line_buf[col * 2]     = pixel >> 8;
                line_buf[col * 2 + 1] = pixel & 0xFF;
            } else {
                // Background pixel
                line_buf[col * 2]     = bg_hi;
                line_buf[col * 2 + 1] = bg_lo;
            }
        }

        spi_write_blocking(_spi, line_buf, draw_w * 2);
    }

    gpio_put(_pinCS, 1);
}

void ST7789::drawStringAA(int16_t x, int16_t y, const char* str, uint16_t color, uint16_t bg, const AAFont* font) {
    int16_t cursor_x = x;

    while (*str) {
        if (*str == '\n') {
            cursor_x = x;
            y += font->lineHeight;
        } else {
            drawCharAA(cursor_x, y, *str, color, bg, font);

            // Advance cursor
            if (*str >= font->firstChar && *str <= font->lastChar) {
                cursor_x += font->glyphs[*str - font->firstChar].xAdvance;
            }
        }
        str++;
    }
}

int ST7789::getStringWidthAA(const char* str, const AAFont* font) {
    int width = 0;
    while (*str) {
        if (*str >= font->firstChar && *str <= font->lastChar) {
            width += font->glyphs[*str - font->firstChar].xAdvance;
        }
        str++;
    }
    return width;
}

// ===== Bitmap drawing =====

void ST7789::drawBitmap(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t* data) {
    if (x >= WIDTH || y >= HEIGHT || w <= 0 || h <= 0) return;

    int16_t x1 = x + w - 1;
    int16_t y1 = y + h - 1;
    if (x1 >= WIDTH) x1 = WIDTH - 1;
    if (y1 >= HEIGHT) y1 = HEIGHT - 1;
    int16_t draw_w = x1 - x + 1;
    int16_t draw_h = y1 - y + 1;

    setAddressWindow(x, y, x1, y1);

    gpio_put(_pinDC, 1);
    gpio_put(_pinCS, 0);

    // Send row by row with byte-swap (RP2040 little-endian, ST7789 big-endian)
    uint8_t line_buf[480]; // Max 240 pixels x 2 bytes per row

    for (int16_t row = 0; row < draw_h; row++) {
        const uint16_t* src = &data[row * w];
        for (int16_t col = 0; col < draw_w; col++) {
            uint16_t pixel = src[col];
            line_buf[col * 2]     = pixel >> 8;
            line_buf[col * 2 + 1] = pixel & 0xFF;
        }
        spi_write_blocking(_spi, line_buf, draw_w * 2);
    }

    gpio_put(_pinCS, 1);
}

void ST7789::drawBitmapScaled(int16_t x, int16_t y, int16_t out_w, int16_t out_h,
                               int16_t src_w, int16_t src_h, const uint16_t* data) {
    if (x >= WIDTH || y >= HEIGHT || out_w <= 0 || out_h <= 0) return;

    int16_t x1 = x + out_w - 1;
    int16_t y1 = y + out_h - 1;
    if (x1 >= WIDTH) x1 = WIDTH - 1;
    if (y1 >= HEIGHT) y1 = HEIGHT - 1;
    int16_t draw_w = x1 - x + 1;
    int16_t draw_h = y1 - y + 1;

    setAddressWindow(x, y, x1, y1);

    gpio_put(_pinDC, 1);
    gpio_put(_pinCS, 0);

    uint8_t line_buf[480];

    for (int16_t row = 0; row < draw_h; row++) {
        int16_t src_y = (row * src_h) / out_h;
        const uint16_t* src_row = &data[src_y * src_w];
        for (int16_t col = 0; col < draw_w; col++) {
            int16_t src_x = (col * src_w) / out_w;
            uint16_t pixel = src_row[src_x];
            line_buf[col * 2]     = pixel >> 8;
            line_buf[col * 2 + 1] = pixel & 0xFF;
        }
        spi_write_blocking(_spi, line_buf, draw_w * 2);
    }

    gpio_put(_pinCS, 1);
}

// ===== Utility =====

uint16_t ST7789::rgb565(uint8_t r, uint8_t g, uint8_t b) {
    // Convert 8-bit RGB to 16-bit RGB565
    // RGB565: RRRRR GGGGGG BBBBB
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
