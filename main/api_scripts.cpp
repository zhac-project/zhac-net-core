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

static const char* TAG_API = "api_scripts";

// Shared script roundtrip — 5 s timeout into s_script_rsp_sem.
static bool script_hap_roundtrip(HapMsgType type,
                                  const uint8_t* payload, uint16_t payload_len) {
    return hap_roundtrip(type, payload, payload_len, s_script_rsp_sem, 5000);
}

// ── Scripts ──────────────────────────────────────────────────────────────
extern "C" ApiStatus api_script_list(const char* /*body*/, size_t /*body_len*/,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;

    uint8_t req_buf[4];
    uint16_t req_len = 0;
    hap_json_encode_rule_list_req(req_buf, sizeof(req_buf), &req_len);

    bool ok = script_hap_roundtrip(HapMsgType::SCRIPT_LIST_REQ, req_buf, req_len);
    xSemaphoreGive(s_script_req_mutex);

    if (ok) {
        size_t n = s_script_rsp_len;
        if (n >= rsp_cap) n = rsp_cap - 1;
        memcpy(rsp_buf, s_script_rsp_json, n);
        rsp_buf[n] = '\0';
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    static const char empty[] = "{\"scripts\":[]}";
    ESP_LOGW(TAG_API, "SCRIPT_LIST_REQ timed out — returning empty list");
    size_t n = sizeof(empty) - 1;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, empty, n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_script_read(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;

    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    hap_json_encode_script_read_req(req_buf, sizeof(req_buf), &req_len, name_str);

    bool ok = script_hap_roundtrip(HapMsgType::SCRIPT_READ_REQ, req_buf, req_len);
    if (!ok) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t n = s_script_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_script_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_script_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_script_delete(const char* body, size_t body_len,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;

    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    hap_json_encode_script_delete(req_buf, sizeof(req_buf), &req_len, name_str);

    bool ok = script_hap_roundtrip(HapMsgType::SCRIPT_DELETE, req_buf, req_len);
    if (!ok) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t n = s_script_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_script_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_script_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// POST /api/scripts/{name}/run — args: {"name":"..."}.
// Dispatches a SCRIPT_RUN_REQ to P4; reuses the script ACK round-trip.
extern "C" ApiStatus api_script_run(const char* body, size_t body_len,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;

    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) return API_INTERNAL_ERROR;

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    if (!hap_json_encode_script_run_req(req_buf, sizeof(req_buf), &req_len, name_str)) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }

    bool ok = script_hap_roundtrip(HapMsgType::SCRIPT_RUN_REQ, req_buf, req_len);
    if (!ok) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t n = s_script_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_script_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_script_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// POST /api/scripts/{name} — args: {"name":"...","src":"..."}
extern "C" ApiStatus api_script_write(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | (const char*)nullptr;
    const char* src_str  = doc["src"]  | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;
    if (!src_str  || src_str[0]  == '\0') return API_BAD_REQUEST;

    uint8_t* hap_buf = static_cast<uint8_t*>(malloc(HAP_SCRIPT_MAX_SRC + 128));
    if (!hap_buf) return API_INTERNAL_ERROR;
    uint16_t hap_len = 0;
    if (!hap_json_encode_script_write(hap_buf, HAP_SCRIPT_MAX_SRC + 128,
                                        &hap_len, name_str, src_str)) {
        free(hap_buf);
        return API_INTERNAL_ERROR;
    }
    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) {
        free(hap_buf);
        return API_INTERNAL_ERROR;
    }
    bool ok = script_hap_roundtrip(HapMsgType::SCRIPT_WRITE, hap_buf, hap_len);
    free(hap_buf);
    if (!ok) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t n = s_script_rsp_len;
    if (n >= rsp_cap) n = rsp_cap - 1;
    memcpy(rsp_buf, s_script_rsp_json, n);
    rsp_buf[n] = '\0';
    xSemaphoreGive(s_script_req_mutex);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// WS script.check — args {"name":"<label>","src":"<lua source>"}.
// Returns the P4 Lua parser verdict: {"ok":bool,"err":"…","line":N}.
// Used by the CM6 linter in the SPA to show inline parse errors.
extern "C" ApiStatus api_script_check(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | "check";
    const char* src_str  = doc["src"]  | (const char*)nullptr;
    if (!src_str || src_str[0] == '\0') return API_BAD_REQUEST;

    uint8_t* hap_buf = static_cast<uint8_t*>(malloc(HAP_SCRIPT_MAX_SRC + 128));
    if (!hap_buf) return API_INTERNAL_ERROR;
    uint16_t hap_len = 0;
    if (!hap_json_encode_script_check_req(hap_buf, HAP_SCRIPT_MAX_SRC + 128,
                                           &hap_len, name_str, src_str)) {
        free(hap_buf);
        return API_INTERNAL_ERROR;
    }
    if (xSemaphoreTake(s_script_req_mutex, 0) != pdTRUE) {
        free(hap_buf);
        return API_INTERNAL_ERROR;
    }
    bool ok_rt = script_hap_roundtrip(HapMsgType::SCRIPT_CHECK_REQ, hap_buf, hap_len);
    free(hap_buf);
    if (!ok_rt) {
        xSemaphoreGive(s_script_req_mutex);
        return API_INTERNAL_ERROR;
    }
    size_t cn = s_script_rsp_len;
    if (cn >= rsp_cap) cn = rsp_cap - 1;
    memcpy(rsp_buf, s_script_rsp_json, cn);
    rsp_buf[cn] = '\0';
    xSemaphoreGive(s_script_req_mutex);
    if (rsp_len) *rsp_len = cn;
    return API_OK;
}

