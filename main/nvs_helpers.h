// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include "nvs.h"

// Single-shot reader for the mqtt_cfg namespace. Three call sites read
// the same {enabled,broker_url,root_topic,client_id} bundle (boot
// staging, /api/status, mqtt-toggle re-read in /api/settings). Missing
// keys leave the corresponding out-param zeroed. Returns false if
// nvs_open itself fails.
static inline bool nvs_read_mqtt_cfg(uint8_t* enabled,
                                      char* url,  size_t url_cap,
                                      char* root, size_t root_cap,
                                      char* cid,  size_t cid_cap) {
    if (enabled)             *enabled = 0;
    if (url  && url_cap)     url[0]  = '\0';
    if (root && root_cap)    root[0] = '\0';
    if (cid  && cid_cap)     cid[0]  = '\0';
    nvs_handle_t h;
    if (nvs_open("mqtt_cfg", NVS_READONLY, &h) != ESP_OK) return false;
    if (enabled) nvs_get_u8(h, "enabled", enabled);
    if (url  && url_cap)  { size_t len = url_cap;  nvs_get_str(h, "broker_url", url,  &len); }
    if (root && root_cap) { size_t len = root_cap; nvs_get_str(h, "root_topic", root, &len); }
    if (cid  && cid_cap)  { size_t len = cid_cap;  nvs_get_str(h, "client_id",  cid,  &len); }
    nvs_close(h);
    return true;
}
