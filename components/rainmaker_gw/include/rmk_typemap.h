// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { RMK_DEV_LIGHTBULB=0, RMK_DEV_MOTION_SENSOR, RMK_DEV_CONTACT_SENSOR,
               RMK_DEV_SWITCH, RMK_DEV_TEMP_SENSOR, RMK_DEV_OTHER } rmk_devtype_t;
typedef struct { char name[24]; bool writable; } rmk_expose_t;
rmk_devtype_t rmk_classify(const rmk_expose_t* ex, size_t n);
const char*   rmk_devtype_str(rmk_devtype_t t);
int    rmk_bri_to_rm(int zb);       // 0..254 -> 0..100 (round, clamp)
int    rmk_bri_from_rm(int rm);     // 0..100 -> 0..254
int    rmk_mired_to_kelvin(int mired);  // clamp 2700..6500
int    rmk_kelvin_to_mired(int k);
double rmk_div100(int v100);
bool   rmk_contact_to_rm(bool zhac_contact);  // polarity pinned here

typedef enum { RMK_CONV_NONE=0, RMK_CONV_BRI, RMK_CONV_CCT, RMK_CONV_DIV100, RMK_CONV_CONTACT } rmk_conv_t;
typedef struct {
    const char* zhac_key;   // shadow key to read/write
    const char* rm_name;    // param name shown in app ("Power")
    const char* rm_type;    // "esp.param.power"
    const char* rm_ui;      // "esp.ui.toggle" | "esp.ui.slider" | NULL
    char data_type;         // 'b','i','f'
    bool writable;
    int min, max, step;     // 0,0,0 = no bounds
    rmk_conv_t conv;
    bool primary;
} rmk_param_t;
#define RMK_MAX_PARAMS 8
size_t rmk_build_params(rmk_devtype_t t, const rmk_expose_t* ex, size_t n,
                        rmk_param_t* out, size_t cap);
#ifdef __cplusplus
}
#endif
