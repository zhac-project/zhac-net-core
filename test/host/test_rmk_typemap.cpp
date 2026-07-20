// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cassert>
#include <cstring>
#include <cstdio>
#include "rmk_typemap.h"

static rmk_expose_t E(const char* n, bool w){ rmk_expose_t e{}; strncpy(e.name,n,sizeof e.name-1); e.writable=w; return e; }

static void test_classifier() {
    { rmk_expose_t ex[] = {E("state",true), E("brightness",true)};
      assert(rmk_classify(ex,2) == RMK_DEV_LIGHTBULB); }
    { rmk_expose_t ex[] = {E("state",true), E("color_temp",true)};
      assert(rmk_classify(ex,2) == RMK_DEV_LIGHTBULB); }
    // precedence: light beats sensor exposes
    { rmk_expose_t ex[] = {E("occupancy",false), E("brightness",true)};
      assert(rmk_classify(ex,2) == RMK_DEV_LIGHTBULB); }
    { rmk_expose_t ex[] = {E("occupancy",false), E("battery",false)};
      assert(rmk_classify(ex,2) == RMK_DEV_MOTION_SENSOR); }
    { rmk_expose_t ex[] = {E("contact",false), E("battery",false)};
      assert(rmk_classify(ex,2) == RMK_DEV_CONTACT_SENSOR); }
    // occupancy beats contact (spec §3 order)
    { rmk_expose_t ex[] = {E("contact",false), E("occupancy",false)};
      assert(rmk_classify(ex,2) == RMK_DEV_MOTION_SENSOR); }
    { rmk_expose_t ex[] = {E("state",true)};
      assert(rmk_classify(ex,1) == RMK_DEV_SWITCH); }
    // writable state + other writable -> not plain switch -> OTHER
    { rmk_expose_t ex[] = {E("state",true), E("mode",true)};
      assert(rmk_classify(ex,2) == RMK_DEV_OTHER); }
    { rmk_expose_t ex[] = {E("temperature",false), E("humidity",false)};
      assert(rmk_classify(ex,2) == RMK_DEV_TEMP_SENSOR); }
    { rmk_expose_t ex[] = {E("illuminance",false)};
      assert(rmk_classify(ex,1) == RMK_DEV_OTHER); }
    assert(strcmp(rmk_devtype_str(RMK_DEV_LIGHTBULB), "esp.device.lightbulb") == 0);
    assert(strcmp(rmk_devtype_str(RMK_DEV_MOTION_SENSOR), "esp.device.motion-sensor") == 0);
    assert(strcmp(rmk_devtype_str(RMK_DEV_CONTACT_SENSOR), "esp.device.contact-sensor") == 0);
    assert(strcmp(rmk_devtype_str(RMK_DEV_SWITCH), "esp.device.switch") == 0);
    assert(strcmp(rmk_devtype_str(RMK_DEV_TEMP_SENSOR), "esp.device.temperature-sensor") == 0);
    assert(strcmp(rmk_devtype_str(RMK_DEV_OTHER), "esp.device.other") == 0);
    printf("classifier ok\n");
}
static void test_conversions() {
    assert(rmk_bri_to_rm(0) == 0);
    assert(rmk_bri_to_rm(254) == 100);
    assert(rmk_bri_to_rm(127) == 50);
    assert(rmk_bri_to_rm(300) == 100);           // clamp
    assert(rmk_bri_from_rm(0) == 0);
    assert(rmk_bri_from_rm(100) == 254);
    assert(rmk_bri_from_rm(50) == 127);
    for (int rm = 0; rm <= 100; rm++)            // round-trip stability
        assert(rmk_bri_to_rm(rmk_bri_from_rm(rm)) == rm);
    assert(rmk_mired_to_kelvin(250) == 4000);
    assert(rmk_mired_to_kelvin(500) == 2700);    // 2000K clamps up
    assert(rmk_mired_to_kelvin(100) == 6500);    // 10000K clamps down
    assert(rmk_mired_to_kelvin(0) == 6500);      // div-by-zero guard -> max
    assert(rmk_kelvin_to_mired(4000) == 250);
    assert(rmk_div100(2550) == 25.5);
    assert(rmk_div100(-500) == -5.0);
    // POLARITY (spec §3): z2m contact==true means CLOSED. RainMaker
    // contact-detection-state: pinned as detected == closed == true.
    // If HW/app check (Task 14) disproves, flip HERE + here only.
    assert(rmk_contact_to_rm(true) == true);
    printf("conversions ok\n");
}
int main(){ test_classifier(); test_conversions(); printf("ALL OK\n"); return 0; }
