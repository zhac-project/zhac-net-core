// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared command handlers — domain split from former api_handlers.cpp.
// Handlers here are PURE business logic. Auth, rate-limiting, URI parsing,
// and transport framing live in rest_*.cpp (HTTP) and ws_bridge.cpp (WS).

#include "api_handlers.h"
#include "s3_internal.h"
#include "log_ring.h"
#include "groups_store.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "wifi_mgr.h"
#include "ArduinoJson.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char* TAG_API = "api_devices";

// ── Devices ──────────────────────────────────────────────────────────────

// Coalesce burst calls. The SPA's auto-refresh + manual click can queue a
// dozen `device.list` cmds in <100 ms; without this gate every one fires
// a fresh GET_DEVICES + DEVICE_LIST round-trip across SPI, exhausting
// the HAP session window and tipping both sides into retransmit storms.
// 1 s TTL is generous — DEVICE_LIST changes only on join/leave, and
// push events (`device.added/removed`) refresh the cache out-of-band.
static constexpr int64_t kDevListCacheMs = 1000;
static int64_t           s_devlist_last_ok_ms = 0;

extern "C" ApiStatus api_device_list(const char* /*body*/, size_t /*body_len*/,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    // Block up to slightly more than the hap_roundtrip budget (8 s) so
    // back-to-back callers serialise instead of all racing onto SPI.
    if (xSemaphoreTake(s_devlist_req_mutex, pdMS_TO_TICKS(8500)) != pdTRUE) {
        return API_INTERNAL_ERROR;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_devlist_last_ok_ms != 0 &&
        (now_ms - s_devlist_last_ok_ms) < kDevListCacheMs &&
        s_devlist_rsp_len > 0) {
        size_t n = s_devlist_rsp_len;
        if (n >= rsp_cap) n = rsp_cap - 1;
        memcpy(rsp_buf, s_devlist_rsp_json, n);
        rsp_buf[n] = '\0';
        xSemaphoreGive(s_devlist_req_mutex);
        if (rsp_len) *rsp_len = n;
        ESP_LOGD(TAG_API, "device.list cache hit age=%lldms",
                 (long long)(now_ms - s_devlist_last_ok_ms));
        return API_OK;
    }

    bool ok = hap_roundtrip(HapMsgType::GET_DEVICES, nullptr, 0,
                             s_devlist_rsp_sem);
    if (!ok) {
        xSemaphoreTake(s_devlist_rsp_sem, 0);
        xSemaphoreGive(s_devlist_req_mutex);
        return API_INTERNAL_ERROR;
    }

    size_t n = s_devlist_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_devlist_rsp_json, n);
    rsp_buf[n] = '\0';
    s_devlist_last_ok_ms = now_ms;
    xSemaphoreGive(s_devlist_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// GET /api/devices/{ieee}[/options] — expects args like
// {"ieee":"0x...","sub":"options"} (sub is optional).
extern "C" ApiStatus api_device_get(const char* body, size_t body_len,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    const char* sub = doc["sub"] | (const char*)nullptr;
    ESP_LOGI(TAG_API, "device.get ieee=%s sub=%s", ieee_str, sub ? sub : "(none)");

    if (sub && strcmp(sub, "options") == 0) {
        char nvs_key[16];
        snprintf(nvs_key, sizeof(nvs_key), "%014llx",
                 (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));
        char blob[256] = "{}";
        if (s_nvs_zhac_opt) {
            size_t sz = sizeof(blob) - 1;
            nvs_get_str(s_nvs_zhac_opt, nvs_key, blob, &sz);
        }
        size_t n = strlen(blob);
        if (n >= rsp_cap) n = rsp_cap - 1;
        memcpy(rsp_buf, blob, n);
        rsp_buf[n] = '\0';
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }

    if (xSemaphoreTake(s_devinfo_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;

    uint8_t req_buf[48];
    uint16_t req_len = 0;
    hap_json_encode_get_device_req(req_buf, sizeof(req_buf), &req_len, ieee);

    bool ok = hap_roundtrip(HapMsgType::GET_DEVICE_BY_ID, req_buf, req_len,
                             s_devinfo_rsp_sem);
    if (!ok) {
        xSemaphoreTake(s_devinfo_rsp_sem, 0);
        xSemaphoreGive(s_devinfo_req_mutex);
        return API_INTERNAL_ERROR;
    }

    size_t n = s_devinfo_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_devinfo_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_devinfo_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// POST /api/devices/{ieee}/{bind|unbind}
// args: original body merged with {"ieee":"..." , "unbind":bool}
extern "C" ApiStatus api_device_bind(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t src_ieee = parse_ieee(ieee_str);
    if (src_ieee == 0) return API_BAD_REQUEST;
    bool is_unbind = doc["unbind"] | false;

    HapBindReq bind{};
    bind.src_ieee = src_ieee;
    bind.src_ep   = doc["src_ep"]  | (uint8_t)0;
    bind.cluster  = doc["cluster"] | (uint16_t)0;
    bind.dst_ieee = parse_ieee(doc["dst_ieee"] | (const char*)nullptr);
    bind.dst_ep   = doc["dst_ep"]  | (uint8_t)0;
    bind.unbind   = is_unbind;

    uint8_t hap_buf[128];
    uint16_t hap_len = 0;
    if (!hap_json_encode_bind_req(hap_buf, sizeof(hap_buf), &hap_len, bind)) {
        return API_INTERNAL_ERROR;
    }

    if (xSemaphoreTake(s_bind_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;
    bool got_rsp = hap_roundtrip(HapMsgType::BIND_REQ, hap_buf, hap_len,
                                  s_bind_rsp_sem);
    bool ok = got_rsp && s_bind_rsp_ok;
    xSemaphoreGive(s_bind_req_mutex);

    const size_t n = ok ? api_write_ok(rsp_buf, rsp_cap)
                        : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// DELETE /api/devices/{ieee} — args: {"ieee":"...", "hard":bool}
extern "C" ApiStatus api_device_delete(const char* body, size_t body_len,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;
    bool hard = doc["hard"] | false;

    uint8_t hap_buf[64];
    uint16_t hap_len = 0;
    if (!hap_json_encode_device_delete(hap_buf, sizeof(hap_buf), &hap_len,
                                         ieee, hard)) {
        return API_INTERNAL_ERROR;
    }

    if (xSemaphoreTake(s_devdel_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;
    bool got_rsp = hap_roundtrip(HapMsgType::DEVICE_DELETE, hap_buf, hap_len,
                                  s_devdel_rsp_sem);
    bool ok = got_rsp && s_devdel_rsp_ok;
    xSemaphoreGive(s_devdel_req_mutex);

    if (ok) {
        char ws_msg[64];
        snprintf(ws_msg, sizeof(ws_msg),
                 "{\"event\":\"device_deleted\",\"ieee\":\"0x%016llX\"}",
                 (unsigned long long)ieee);
        ws_server_broadcast(ws_msg, strlen(ws_msg));
    }

    const size_t n = ok ? api_write_ok(rsp_buf, rsp_cap)
                        : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// Parse the IEEE out of the body and return the surviving doc.
// Returns API_BAD_REQUEST via `status_out` when anything is missing.
static ApiStatus parse_ieee_body(const char* body, size_t body_len,
                                  JsonDocument& doc, uint64_t& ieee_out) {
    if (body_len == 0) return API_BAD_REQUEST;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;
    ieee_out = ieee;
    return API_OK;
}

// PUT /api/devices/{ieee}        — WS `device.rename` — args {ieee, name}.
extern "C" ApiStatus api_device_rename(const char* body, size_t body_len,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;

    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;
    char name[30];
    strncpy(name, name_str, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    uint8_t hap_buf[80];
    uint16_t hap_len = 0;
    if (!hap_json_encode_device_set_name(hap_buf, sizeof(hap_buf), &hap_len,
                                           ieee, name)) {
        return API_INTERNAL_ERROR;
    }

    if (xSemaphoreTake(s_devinfo_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;
    bool ok = hap_roundtrip(HapMsgType::DEVICE_SET_NAME, hap_buf, hap_len,
                             s_devinfo_rsp_sem, 5000);
    if (!ok) {
        xSemaphoreTake(s_devinfo_rsp_sem, 0);
        xSemaphoreGive(s_devinfo_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t n = s_devinfo_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_devinfo_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_devinfo_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// WS `device.attr.set` — args {ieee, key, value, ep?, cluster?, attr?}.
// `value` may be bool / integer / string; boolean/integer round-trip
// through HapSetAttrReq.val directly. String values are numeric-parsed
// (enum-name strings need a follow-up — HapSetAttrReq has no str field
// today and the SET_ATTRIBUTE wire path is int-only). `val` is accepted
// as a legacy alias for `value` so existing REST callers keep working.
extern "C" ApiStatus api_device_attr_set(const char* body, size_t body_len,
                                          char* rsp_buf, size_t rsp_cap,
                                          size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;

    HapSetAttrReq attr{};
    attr.ieee = ieee;
    const char* key_str = doc["key"] | (const char*)nullptr;
    if (!key_str || key_str[0] == '\0') return API_BAD_REQUEST;
    if (strlen(key_str) >= sizeof(attr.key)) return API_BAD_REQUEST;
    strncpy(attr.key, key_str, sizeof(attr.key) - 1);

    // Accept `value` (SPA) or `val` (legacy REST) as aliases.
    JsonVariantConst vv = doc["value"];
    if (vv.isNull()) vv = doc["val"];
    if (vv.is<bool>())              attr.val = vv.as<bool>() ? 1 : 0;
    else if (vv.is<int>())          attr.val = vv.as<int32_t>();
    else if (vv.is<float>())        attr.val = (int32_t)vv.as<float>();
    else if (vv.is<const char*>())  attr.val = atoi(vv.as<const char*>());
    else                            attr.val = 0;

    attr.ep      = doc["ep"]      | (uint8_t)0;
    attr.cluster = doc["cluster"] | (uint16_t)0;
    attr.attr    = doc["attr"]    | (uint16_t)0;

    uint8_t hap_buf[160];
    uint16_t hap_len = 0;
    if (!hap_json_encode_set_attr(hap_buf, sizeof(hap_buf), &hap_len, attr)) {
        return API_INTERNAL_ERROR;
    }

    if (xSemaphoreTake(s_setattr_req_mutex, 0) != pdTRUE) {
        return API_INTERNAL_ERROR;
    }
    hap_send(HapMsgType::SET_ATTRIBUTE, hap_buf, hap_len, HAP_FLAG_NEEDS_ACK);
    bool rsp_ok = xSemaphoreTake(s_setattr_rsp_sem, pdMS_TO_TICKS(3000)) == pdTRUE;
    bool cmd_ok = s_setattr_rsp_ok;
    if (!rsp_ok) xSemaphoreTake(s_setattr_rsp_sem, 0);
    xSemaphoreGive(s_setattr_req_mutex);

    if (!rsp_ok) return API_INTERNAL_ERROR;
    if (cmd_ok) {
        const size_t n = api_write_ok(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    int n = snprintf(rsp_buf, rsp_cap, "%s",
                     "{\"ok\":false,\"err\":\"command failed\"}");
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

// WS `device.options.set` — args {ieee, occupancy_timeout?, debounce_ms?,
// flood_protection?}. Stores the body to NVS verbatim; P4 is notified
// via DEVICE_OPTIONS_SET only when one of the forwarded fields is set.
// The REST wrapper passes the raw original body under `__raw` so the
// stored NVS blob stays byte-identical to the pre-migration handler.
extern "C" ApiStatus api_device_options_set(const char* body, size_t body_len,
                                             char* rsp_buf, size_t rsp_cap,
                                             size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;
    if (body[0] != '{') return API_BAD_REQUEST;

    char nvs_key[16];
    snprintf(nvs_key, sizeof(nvs_key), "%014llx",
             (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));

    const char* raw_body = doc["__raw"] | (const char*)nullptr;
    const char* write_str = (raw_body && raw_body[0]) ? raw_body : body;

    bool nvs_ok = false;
    if (s_nvs_zhac_opt) {
        nvs_ok = (nvs_set_str(s_nvs_zhac_opt, nvs_key, write_str) == ESP_OK) &&
                 (nvs_commit(s_nvs_zhac_opt) == ESP_OK);
    }

    bool p4_ok     = true;
    bool forwarded = false;
    int32_t  occ_raw = -1;
    int32_t  deb_raw = -1;
    const int32_t* occ = nullptr;
    const int32_t* deb = nullptr;
    if (doc["occupancy_timeout"].is<int>()) {
        occ_raw = doc["occupancy_timeout"].as<int32_t>();
        occ = &occ_raw;
    }
    if (doc["debounce_ms"].is<int>()) {
        deb_raw = doc["debounce_ms"].as<int32_t>();
        deb = &deb_raw;
    } else if (doc["flood_protection"].is<bool>()) {
        deb_raw = doc["flood_protection"].as<bool>() ? 500 : 0;
        deb = &deb_raw;
    }

    if (occ || deb) {
        uint8_t hap_buf[96];
        uint16_t hap_len = 0;
        if (hap_json_encode_device_options_set(hap_buf, sizeof(hap_buf),
                                                &hap_len, ieee, occ, deb)) {
            if (xSemaphoreTake(s_devopt_req_mutex, 0) == pdTRUE) {
                bool got_rsp = hap_roundtrip(HapMsgType::DEVICE_OPTIONS_SET,
                                              hap_buf, hap_len,
                                              s_devopt_rsp_sem);
                p4_ok = got_rsp && s_devopt_rsp_ok;
                xSemaphoreGive(s_devopt_req_mutex);
                forwarded = true;
            } else {
                p4_ok = false;
            }
        }
    }

    const bool ok = nvs_ok && p4_ok;
    if (!ok) {
        const size_t n = api_write_err(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    if (!forwarded) {
        const size_t n = api_write_ok(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    int n = snprintf(rsp_buf, rsp_cap, "%s", "{\"ok\":true,\"applied\":true}");
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_device_reinterview(const char* body, size_t body_len,
                                             char* rsp_buf, size_t rsp_cap,
                                             size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;

    uint8_t hap_buf[48];
    uint16_t hap_len = 0;
    if (!hap_json_encode_interview_req(hap_buf, sizeof(hap_buf), &hap_len, ieee)) {
        int n = snprintf(rsp_buf, rsp_cap,
                         "{\"ok\":false,\"err\":\"encode failed\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }
    hap_send(HapMsgType::INTERVIEW_REQ, hap_buf, hap_len);
    ESP_LOGI(TAG_API, "interview re-triggered for 0x%016llx",
             (unsigned long long)ieee);
    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

