/**
 * @file src/platform/macos/virtual_display.h
 * @brief Declarations for CGVirtualDisplay-based virtual display management on macOS.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Create an additional virtual display with the specified resolution and refresh rate.
 * @param width Display width in pixels.
 * @param height Display height in pixels.
 * @param fps Refresh rate in Hz.
 * @return The CGDirectDisplayID of the created display, or 0 on failure.
 */
uint32_t virtual_display_create(int width, int height, int fps);

/**
 * @brief Destroy one virtual display created by virtual_display_create().
 * @param display_id The CGDirectDisplayID returned on creation.
 */
void virtual_display_destroy(uint32_t display_id);

#ifdef __cplusplus
}
#endif
