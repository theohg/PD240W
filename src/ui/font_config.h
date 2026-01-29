#pragma once

// ============================================================================
// Font Configuration
// ============================================================================
// Change the font includes and pointer assignments here to adjust all
// screen text sizes from a single location.
//
// To regenerate fonts at a different size, run from the tools/ directory:
//   /Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
//       fonts/Inter-Bold.ttf 28 ../src/drivers/display/font_inter_28b.h \
//       --chars digits --name font_inter_28b
//
//   /Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
//       fonts/Inter-SemiBold.ttf 20 ../src/drivers/display/font_inter_20sb.h \
//       --name font_inter_20sb
//
//   /Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
//       fonts/Inter-Regular.ttf 14 ../src/drivers/display/font_inter_14.h \
//       --name font_inter_14
// ============================================================================

#include "drivers/display/font_inter_28b.h"
#include "drivers/display/font_inter_20sb.h"
#include "drivers/display/font_inter_14.h"

// Large: main screen power values (voltage, current, power numbers)
static const AAFont* const FONT_LARGE = &font_inter_28b;

// Medium: headers, contract info, adjustment screen values
static const AAFont* const FONT_MEDIUM = &font_inter_20sb;

// Small: labels, menus, hints, secondary text, temperature
static const AAFont* const FONT_SMALL = &font_inter_14;
