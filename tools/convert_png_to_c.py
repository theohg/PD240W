import sys
from PIL import Image

def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert_image(filename):
    # Load image
    img = Image.open(filename)
    
    # 1. Resize to fit screen (max width 220px to leave margin)
    target_width = 220
    w_percent = (target_width / float(img.size[0]))
    h_size = int((float(img.size[1]) * float(w_percent)))
    img = img.resize((target_width, h_size), Image.Resampling.LANCZOS)
    
    # 2. Handle Transparency (Paste onto black background)
    background = Image.new('RGB', img.size, (0, 0, 0)) # Black background
    if img.mode == 'RGBA':
        background.paste(img, mask=img.split()[3]) # Use alpha channel as mask
    else:
        background.paste(img)
    
    img_rgb = background

    # 3. Generate Header File
    output_str = f"#pragma once\n\n"
    output_str += f"#include <stdint.h>\n\n"
    output_str += f"const int SYNAPTICON_WIDTH = {img_rgb.width};\n"
    output_str += f"const int SYNAPTICON_HEIGHT = {img_rgb.height};\n\n"
    output_str += f"const uint16_t synapticon_data[] = {{\n"

    pixels = list(img_rgb.getdata())
    
    line_len = 0
    for r, g, b in pixels:
        val = rgb888_to_rgb565(r, g, b)
        output_str += f"0x{val:04X}, "
        line_len += 1
        if line_len >= 16:
            output_str += "\n"
            line_len = 0
            
    output_str += "\n};\n"

    with open("synapticon_logo.h", "w") as f:
        f.write(output_str)
        print(f"Created synapticon_logo.h ({img_rgb.width}x{img_rgb.height})")

if __name__ == "__main__":
    # If your file is named differently, change it here
    try:
        convert_image("synapticon.png")
    except Exception as e:
        print(f"Error: {e}")
        print("Make sure 'synapticon.png' is in the same folder.")