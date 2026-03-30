#pragma once

#include <stdio.h>
#include "pico/stdlib.h"

// Logging System for PD240W
// Provides categorized logging with easy enable/disable for debug messages

// Enable/disable debug logging (disabled in production for performance)
#ifndef DEBUG_BUILD
    #define DEBUG_BUILD 1  // Set to 0 for production builds
#endif

// CLI log suppression gate (set by LOG:OFF command)
// Declared as extern to avoid circular include with cli.h
extern bool g_cli_log_enabled;

// ANSI color codes for better readability (optional, works with most terminals)
#define LOG_COLOR_RESET   "\033[0m"
#define LOG_COLOR_RED     "\033[31m"
#define LOG_COLOR_YELLOW  "\033[33m"
#define LOG_COLOR_BLUE    "\033[34m"
#define LOG_COLOR_CYAN    "\033[36m"
#define LOG_COLOR_GREEN   "\033[32m"

// Logging macros with color-coded prefixes
#if DEBUG_BUILD
    #define LOG_DEBUG(fmt, ...) \
        do { \
            if (g_cli_log_enabled) \
                printf(LOG_COLOR_CYAN "[DEBUG] " LOG_COLOR_RESET fmt "\n", ##__VA_ARGS__); \
        } while(0)
#else
    #define LOG_DEBUG(fmt, ...) ((void)0)  // No-op in production
#endif

#define LOG_INFO(fmt, ...) \
    do { \
        if (g_cli_log_enabled) \
            printf(LOG_COLOR_GREEN "[INFO]  " LOG_COLOR_RESET fmt "\n", ##__VA_ARGS__); \
    } while(0)

#define LOG_WARN(fmt, ...) \
    do { \
        if (g_cli_log_enabled) \
            printf(LOG_COLOR_YELLOW "[WARN]  " LOG_COLOR_RESET fmt "\n", ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(fmt, ...) \
    do { \
        if (g_cli_log_enabled) \
            printf(LOG_COLOR_RED "[ERROR] " LOG_COLOR_RESET fmt "\n", ##__VA_ARGS__); \
    } while(0)

// Special logging for critical safety events (always output, even when logs suppressed)
#define LOG_CRITICAL(fmt, ...) \
    do { \
        printf(LOG_COLOR_RED "!!! [CRITICAL] " fmt " !!!" LOG_COLOR_RESET "\n", ##__VA_ARGS__); \
    } while(0)

// Helper macro for hardware initialization logging
#define LOG_HW_INIT(component, status) \
    do { \
        if (status) { \
            LOG_INFO("%s initialized successfully", component); \
        } else { \
            LOG_ERROR("%s initialization FAILED", component); \
        } \
    } while(0)

// Helper macro for value logging with units
#define LOG_VALUE(name, value, unit) \
    LOG_DEBUG("%s: %.3f %s", name, (double)(value), unit)

// Separator for readability in logs
#define LOG_SEPARATOR() \
    printf("========================================\n")
