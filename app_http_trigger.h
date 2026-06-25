/*
 * app_http_trigger.h — HTTP Request Sender
 *
 * Each "screen" is one HTTP-triggered widget defined in /setup/setup.txt:
 *   - button   — tap fires request, shows response briefly
 *   - toggle   — two states, each fires a different request
 *   - slider   — vertical VU-meter, sends selected value on release
 *   - display  — fetches on load + on tap, shows value (with JSON_PATH)
 *
 * Hardware:
 *   BOOT short — next screen
 *   PWR  short — previous screen
 *   PWR  long  — back to launcher (handled by common_tick)
 *   Touch      — widget-specific
 *
 * USB: plugging into a PC mounts the SD card as a removable drive (USB MSC).
 * No write-protect, no on-screen browser — purely so setup.txt can be edited
 * from the host. Config is re-read on the next boot of the app.
 */

#pragma once
#include "Arduino_GFX_Library.h"

void app_http_trigger_setup(Arduino_OLED *gfx);
void app_http_trigger_loop();
