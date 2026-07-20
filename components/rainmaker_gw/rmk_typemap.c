// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rmk_typemap.h"
#include <string.h>

static bool has(const rmk_expose_t* ex, size_t n, const char* name) {
    for (size_t i = 0; i < n; i++) if (!strcmp(ex[i].name, name)) return true;
    return false;
}
static size_t count_writable(const rmk_expose_t* ex, size_t n) {
    size_t c = 0; for (size_t i = 0; i < n; i++) if (ex[i].writable) c++; return c;
}

rmk_devtype_t rmk_classify(const rmk_expose_t* ex, size_t n) {
    if (has(ex,n,"brightness") || has(ex,n,"color_temp") ||
        has(ex,n,"color_x") || has(ex,n,"color_y")) return RMK_DEV_LIGHTBULB;
    if (has(ex,n,"occupancy")) return RMK_DEV_MOTION_SENSOR;
    if (has(ex,n,"contact"))   return RMK_DEV_CONTACT_SENSOR;
    if (has(ex,n,"state")) {
        // writable state and nothing ELSE controllable -> switch
        size_t w = count_writable(ex, n);
        bool state_writable = false;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(ex[i].name,"state") && ex[i].writable) state_writable = true;
        if (state_writable && w == 1) return RMK_DEV_SWITCH;
        return RMK_DEV_OTHER;
    }
    if (has(ex,n,"temperature") && count_writable(ex,n) == 0) return RMK_DEV_TEMP_SENSOR;
    return RMK_DEV_OTHER;
}

const char* rmk_devtype_str(rmk_devtype_t t) {
    switch (t) {
    case RMK_DEV_LIGHTBULB:      return "esp.device.lightbulb";
    case RMK_DEV_MOTION_SENSOR:  return "esp.device.motion-sensor";
    case RMK_DEV_CONTACT_SENSOR: return "esp.device.contact-sensor";
    case RMK_DEV_SWITCH:         return "esp.device.switch";
    case RMK_DEV_TEMP_SENSOR:    return "esp.device.temperature-sensor";
    default:                     return "esp.device.other";
    }
}
