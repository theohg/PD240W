#!/usr/bin/env python3
"""Clean the Synapticon logo by replacing noisy colors with pink or white."""

import re
import os

# Target colors in RGB565
PINK = 0xE009  # Synapticon pink
WHITE = 0xFFFF

def rgb565_to_rgb(c):
    """Convert RGB565 to RGB888 tuple"""
    r = ((c >> 11) & 0x1F) << 3
    g = ((c >> 5) & 0x3F) << 2
    b = (c & 0x1F) << 3
    return (r, g, b)

def color_distance(c1, c2):
    """Euclidean distance between two RGB565 colors"""
    r1, g1, b1 = rgb565_to_rgb(c1)
    r2, g2, b2 = rgb565_to_rgb(c2)
    return ((r1-r2)**2 + (g1-g2)**2 + (b1-b2)**2) ** 0.5

def closest_color(c):
    """Return PINK or WHITE based on which is closer"""
    if c == PINK or c == WHITE:
        return c
    d_pink = color_distance(c, PINK)
    d_white = color_distance(c, WHITE)
    return PINK if d_pink < d_white else WHITE

def replace_color(match):
    """Regex replacement function"""
    hex_val = int(match.group(0), 16)
    new_val = closest_color(hex_val)
    return f'0x{new_val:04X}'

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    logo_path = os.path.join(script_dir, '..', 'src', 'ui', 'assets', 'synapticon_logo.h')
    
    with open(logo_path, 'r') as f:
        content = f.read()
    
    # Only replace in the data array section
    marker = 'synapticon_data[] = {'
    header_end = content.find(marker)
    
    if header_end != -1:
        header = content[:header_end + len(marker)]
        data_part = content[header_end + len(marker):]
        
        # Replace colors in data part
        cleaned_data = re.sub(r'0x[0-9A-Fa-f]{4}', replace_color, data_part)
        
        cleaned_content = header + cleaned_data
        
        with open(logo_path, 'w') as f:
            f.write(cleaned_content)
        
        print("Logo cleaned successfully!")
        print(f"File: {logo_path}")
    else:
        print("Could not find data array")

if __name__ == '__main__':
    main()
