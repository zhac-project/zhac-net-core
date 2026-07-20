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
int main(){ test_classifier(); printf("ALL OK\n"); return 0; }
