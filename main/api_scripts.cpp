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
#include <memory>
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

// Per-caller script response buffer. SCRIPT_READ_RSP can return the full
// Lua source so cap matches HAP_MAX_PAYLOAD; allocated on the heap so
// REST handler stacks stay tight.
static constexpr size_t kScriptRspCap = HAP_MAX_PAYLOAD;

// ── Scripts ──────────────────────────────────────────────────────────────
extern "C" ApiStatus api_script_list(const char* /*body*/, size_t /*body_len*/,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    uint8_t req_buf[4];
    uint16_t req_len = 0;
    hap_json_encode_rule_list_req(req_buf, sizeof(req_buf), &req_len);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kScriptRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::SCRIPT_LIST_REQ, req_buf, req_len,
                           rsp.get(), kScriptRspCap, &got, 5000)) {
        static const char empty[] = "{\"scripts\":[]}";
        ESP_LOGW(TAG_API, "SCRIPT_LIST_REQ timed out — returning empty list");
        size_t n = sizeof(empty) - 1;
        if (n >= rsp_cap) n = rsp_cap - 1;
        memcpy(rsp_buf, empty, n);
        rsp_buf[n] = '\0';
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
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

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    hap_json_encode_script_read_req(req_buf, sizeof(req_buf), &req_len, name_str);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kScriptRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::SCRIPT_READ_REQ, req_buf, req_len,
                           rsp.get(), kScriptRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
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

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    hap_json_encode_script_delete(req_buf, sizeof(req_buf), &req_len, name_str);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kScriptRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::SCRIPT_DELETE, req_buf, req_len,
                           rsp.get(), kScriptRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// POST /api/scripts/{name}/run — args: {"name":"..."}.
// Dispatches a SCRIPT_RUN_REQ to P4; reuses the script ACK round-trip.
// F-01-legacy: SCRIPT_RUN_REQ is not in expected_response_for table —
// it has no defined response type today (the P4 acks via SCRIPT_ACK but
// the request/response pairing isn't registered). Migration deferred
// until the protocol pins SCRIPT_RUN_REQ→SCRIPT_ACK explicitly; for now
// the handler still uses the legacy fire-and-forget hap_send. The body
// of /api/scripts/{name}/run is acknowledged with a synthetic ok.
extern "C" ApiStatus api_script_run(const char* body, size_t body_len,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;

    uint8_t req_buf[64];
    uint16_t req_len = 0;
    if (!hap_json_encode_script_run_req(req_buf, sizeof(req_buf), &req_len, name_str)) {
        return API_INTERNAL_ERROR;
    }
    hap_send(HapMsgType::SCRIPT_RUN_REQ, req_buf, req_len);
    const size_t n = api_write_ok(rsp_buf, rsp_cap);
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

    auto hap_buf = std::unique_ptr<uint8_t[]>(
        new (std::nothrow) uint8_t[HAP_SCRIPT_MAX_SRC + 128]);
    if (!hap_buf) return API_INTERNAL_ERROR;
    uint16_t hap_len = 0;
    if (!hap_json_encode_script_write(hap_buf.get(), HAP_SCRIPT_MAX_SRC + 128,
                                        &hap_len, name_str, src_str)) {
        return API_INTERNAL_ERROR;
    }
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kScriptRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::SCRIPT_WRITE, hap_buf.get(), hap_len,
                           rsp.get(), kScriptRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
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

    auto hap_buf = std::unique_ptr<uint8_t[]>(
        new (std::nothrow) uint8_t[HAP_SCRIPT_MAX_SRC + 128]);
    if (!hap_buf) return API_INTERNAL_ERROR;
    uint16_t hap_len = 0;
    if (!hap_json_encode_script_check_req(hap_buf.get(), HAP_SCRIPT_MAX_SRC + 128,
                                           &hap_len, name_str, src_str)) {
        return API_INTERNAL_ERROR;
    }
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kScriptRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::SCRIPT_CHECK_REQ, hap_buf.get(), hap_len,
                           rsp.get(), kScriptRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

