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
#ifdef __cplusplus
}
#endif
