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
    // A writable `state` wins even when other writable config knobs exist —
    // real plugs (Tuya TS011F: state + child_lock + power_on_behavior +
    // read-only metering) must not fall through to OTHER. Regression from
    // Gate B, 2026-07-21.
    { rmk_expose_t ex[] = {E("state",true), E("mode",true)};
      assert(rmk_classify(ex,2) == RMK_DEV_SWITCH); }
    { rmk_expose_t ex[] = {E("state",true), E("child_lock",true),
                           E("power_on_behavior",true), E("power",false),
                           E("energy",false)};
      assert(rmk_classify(ex,5) == RMK_DEV_SWITCH); }
    // read-only `state` is NOT a switch (no way to command it)
    { rmk_expose_t ex[] = {E("state",false), E("battery",false)};
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
static const rmk_param_t* find_p(const rmk_param_t* p, size_t n, const char* type) {
    for (size_t i=0;i<n;i++) if (!strcmp(p[i].rm_type,type)) return &p[i];
    return nullptr;
}
static void test_param_plan() {
    rmk_param_t out[RMK_MAX_PARAMS];
    { rmk_expose_t ex[] = {E("state",true), E("brightness",true), E("color_temp",true)};
      size_t n = rmk_build_params(RMK_DEV_LIGHTBULB, ex, 3, out, RMK_MAX_PARAMS);
      const rmk_param_t* pw = find_p(out,n,"esp.param.power");
      assert(pw && pw->primary && pw->writable && pw->data_type=='b'
             && !strcmp(pw->rm_ui,"esp.ui.toggle") && !strcmp(pw->zhac_key,"state"));
      const rmk_param_t* br = find_p(out,n,"esp.param.brightness");
      assert(br && br->min==0 && br->max==100 && br->step==1 && br->conv==RMK_CONV_BRI);
      const rmk_param_t* cct = find_p(out,n,"esp.param.cct");
      assert(cct && cct->min==2700 && cct->max==6500 && cct->conv==RMK_CONV_CCT); }
    { rmk_expose_t ex[] = {E("temperature",false), E("humidity",false)};
      size_t n = rmk_build_params(RMK_DEV_TEMP_SENSOR, ex, 2, out, RMK_MAX_PARAMS);
      const rmk_param_t* t = find_p(out,n,"esp.param.temperature");
      assert(t && t->primary && !t->writable && t->data_type=='f' && t->conv==RMK_CONV_DIV100);
      assert(find_p(out,n,"esp.param.humidity")); }         // extra expose carried
    { rmk_expose_t ex[] = {E("occupancy",false)};
      size_t n = rmk_build_params(RMK_DEV_MOTION_SENSOR, ex, 1, out, RMK_MAX_PARAMS);
      const rmk_param_t* m = find_p(out,n,"esp.param.motion-detection-state");
      assert(m && m->primary && !m->writable); }
    { rmk_expose_t ex[] = {E("contact",false)};
      size_t n = rmk_build_params(RMK_DEV_CONTACT_SENSOR, ex, 1, out, RMK_MAX_PARAMS);
      const rmk_param_t* c = find_p(out,n,"esp.param.contact-detection-state");
      assert(c && c->conv==RMK_CONV_CONTACT); }
    // unknown exposes are skipped, never overflow
    { rmk_expose_t ex[] = {E("state",true), E("weird1",false), E("weird2",false)};
      size_t n = rmk_build_params(RMK_DEV_SWITCH, ex, 3, out, RMK_MAX_PARAMS);
      assert(n == 1 && find_p(out,n,"esp.param.power")); }
    printf("param plan ok\n");
}
int main(){ test_classifier(); test_conversions(); test_param_plan(); printf("ALL OK\n"); return 0; }
