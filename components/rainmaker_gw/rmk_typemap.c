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
        // A writable `state` makes this a switch/plug, regardless of whatever
        // ELSE the device exposes as writable.
        //
        // This deliberately does NOT require `state` to be the only writable
        // expose. The original rule did, and it misfired on the first real
        // device tested at Gate B: a Tuya TS011F smart plug exposes writable
        // `state` PLUS writable config knobs (child_lock, power_on_behavior)
        // and read-only metering, so it fell through to esp.device.other —
        // even though plugs are explicitly inside v1 scope. Config knobs do
        // not change what the device fundamentally IS; the extra exposes are
        // simply not part of the RainMaker param plan (rmk_build_params only
        // maps names it knows), so classifying as a switch loses nothing and
        // gains correct rendering plus Alexa/Google category mapping.
        for (size_t i = 0; i < n; i++)
            if (!strcmp(ex[i].name,"state") && ex[i].writable) return RMK_DEV_SWITCH;
        return RMK_DEV_OTHER;   // read-only `state` (a contact-style sensor, etc.)
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

static int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }
int rmk_bri_to_rm(int zb)   { zb = clampi(zb, 0, 254); return (zb * 100 + 127) / 254; }
int rmk_bri_from_rm(int rm) { rm = clampi(rm, 0, 100); return (rm * 254 + 50) / 100; }
int rmk_mired_to_kelvin(int mired) {
    if (mired <= 0) return 6500;
    return clampi(1000000 / mired, 2700, 6500);
}
int rmk_kelvin_to_mired(int k) { k = clampi(k, 2700, 6500); return 1000000 / k; }
double rmk_div100(int v100) { return (double)v100 / 100.0; }
bool rmk_contact_to_rm(bool zhac_contact) { return zhac_contact; }

typedef struct { const char* key; const char* rm_name; const char* rm_type;
                 const char* rm_ui; char dt; int min,max,step; rmk_conv_t conv; } map_row_t;
static const map_row_t kMap[] = {
    {"state",       "Power",       "esp.param.power",                  "esp.ui.toggle", 'b', 0,0,0,       RMK_CONV_NONE},
    {"brightness",  "Brightness",  "esp.param.brightness",             "esp.ui.slider", 'i', 0,100,1,     RMK_CONV_BRI},
    {"color_temp",  "CCT",         "esp.param.cct",                    "esp.ui.slider", 'i', 2700,6500,100,RMK_CONV_CCT},
    {"temperature", "Temperature", "esp.param.temperature",            NULL,            'f', 0,0,0,       RMK_CONV_DIV100},
    {"humidity",    "Humidity",    "esp.param.humidity",               NULL,            'f', 0,0,0,       RMK_CONV_DIV100},
    {"occupancy",   "Motion",      "esp.param.motion-detection-state", "esp.ui.toggle", 'b', 0,0,0,       RMK_CONV_NONE},
    {"contact",     "Contact",     "esp.param.contact-detection-state","esp.ui.toggle", 'b', 0,0,0,       RMK_CONV_CONTACT},
};
static const char* primary_for(rmk_devtype_t t) {
    switch (t) {
    case RMK_DEV_LIGHTBULB: case RMK_DEV_SWITCH: return "esp.param.power";
    case RMK_DEV_MOTION_SENSOR:  return "esp.param.motion-detection-state";
    case RMK_DEV_CONTACT_SENSOR: return "esp.param.contact-detection-state";
    case RMK_DEV_TEMP_SENSOR:    return "esp.param.temperature";
    default: return "";
    }
}
size_t rmk_build_params(rmk_devtype_t t, const rmk_expose_t* ex, size_t n,
                        rmk_param_t* out, size_t cap) {
    size_t cnt = 0;
    const char* prim = primary_for(t);
    for (size_t i = 0; i < n && cnt < cap; i++) {
        for (size_t r = 0; r < sizeof kMap / sizeof kMap[0]; r++) {
            if (strcmp(ex[i].name, kMap[r].key)) continue;
            rmk_param_t* p = &out[cnt++];
            p->zhac_key = kMap[r].key;   p->rm_name = kMap[r].rm_name;
            p->rm_type  = kMap[r].rm_type; p->rm_ui = kMap[r].rm_ui;
            p->data_type = kMap[r].dt;   p->writable = ex[i].writable;
            p->min = kMap[r].min; p->max = kMap[r].max; p->step = kMap[r].step;
            p->conv = kMap[r].conv;
            p->primary = (strcmp(kMap[r].rm_type, prim) == 0);
            break;
        }
    }
    return cnt;
}
