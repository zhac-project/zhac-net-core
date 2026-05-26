// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote_nvs.h"
#include "remote_nvs_kv.h"
#include <cstring>
#include <cstdio>

#ifndef REMOTE_NVS_HOST_TEST  // Real IDF impl lives here.
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

extern "C" bool kv_get_str(const char* ns, const char* key,
                           char* out, size_t cap) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = cap;
    esp_err_t e = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    return e == ESP_OK;
}
extern "C" bool kv_set_str(const char* ns, const char* key, const char* val) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val ? val : "");
    nvs_close(h);
    return e == ESP_OK;
}
extern "C" bool kv_get_u8(const char* ns, const char* key, uint8_t* out) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_u8(h, key, out);
    nvs_close(h);
    return e == ESP_OK;
}
extern "C" bool kv_set_u8(const char* ns, const char* key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, key, v);
    nvs_close(h);
    return e == ESP_OK;
}
extern "C" bool kv_erase(const char* ns, const char* key) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_key(h, key);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
extern "C" bool kv_commit(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static void default_devid(char* out, size_t cap) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#else  // REMOTE_NVS_HOST_TEST defined by host CMakeLists.

static void default_devid(char* out, size_t cap) {
    if (cap > 0) snprintf(out, cap, "TEST123456");
}

#endif  // REMOTE_NVS_HOST_TEST

static constexpr const char* NS = "zhac_remote";

extern "C" bool remote_nvs_load(bool* enabled_out,
                                char* url_out,    size_t url_cap,
                                char* token_out,  size_t token_cap,
                                char* devid_out,  size_t devid_cap) {
    uint8_t en = 0;
    kv_get_u8(NS, "enabled", &en);
    *enabled_out = (en != 0);

    if (!kv_get_str(NS, "url",   url_out,   url_cap))   url_out[0]   = 0;
    if (!kv_get_str(NS, "token", token_out, token_cap)) token_out[0] = 0;
    if (!kv_get_str(NS, "dev_id", devid_out, devid_cap) || devid_out[0] == 0) {
        default_devid(devid_out, devid_cap);
    }
    return true;
}

extern "C" bool remote_nvs_save(bool enabled,
                                const char* url,
                                const char* token,
                                const char* devid) {
    bool ok = true;
    ok = ok && kv_set_u8(NS, "enabled", enabled ? 1 : 0);
    if (url   && *url)   ok = ok && kv_set_str(NS, "url",    url);
    if (token && *token) ok = ok && kv_set_str(NS, "token",  token);
    if (devid && *devid) ok = ok && kv_set_str(NS, "dev_id", devid);
    ok = ok && kv_commit(NS);
    return ok;
}

extern "C" bool remote_nvs_forget_creds(void) {
    bool ok = true;
    ok = ok && kv_erase(NS, "url");
    ok = ok && kv_erase(NS, "token");
    ok = ok && kv_commit(NS);
    return ok;
}
