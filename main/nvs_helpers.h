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
// A broker URL, root topic or client id is printable ASCII or it is nothing:
// a stale NVS layout from an earlier firmware left a client id of "\x04" on
// a hub, and ArduinoJson writes such bytes into status JSON unescaped, so the
// web page could not parse it. Anything else is dropped as a whole.
static inline void nvs_clean_ascii(char* s) {
    for (char* p = s; p && *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) { s[0] = '\0'; return; }
    }
}

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
    if (url  && url_cap)  nvs_clean_ascii(url);
    if (root && root_cap) nvs_clean_ascii(root);
    if (cid  && cid_cap)  nvs_clean_ascii(cid);
    return true;
}
