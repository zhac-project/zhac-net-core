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

// Per-caller rule response buffer. Heap-allocated (rather than stack)
// because HAP_MAX_PAYLOAD = 4 KB and REST handler stacks are tight.
static constexpr size_t kRuleRspCap = HAP_MAX_PAYLOAD;

// ── Rules ────────────────────────────────────────────────────────────────
extern "C" ApiStatus api_rule_list(const char* /*body*/, size_t /*body_len*/,
                                    char* rsp_buf, size_t rsp_cap,
                                    size_t* rsp_len) {
    uint8_t req_buf[4];
    uint16_t req_len = 0;
    hap_json_encode_rule_list_req(req_buf, sizeof(req_buf), &req_len);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRuleRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::RULE_LIST_REQ, req_buf, req_len,
                           rsp.get(), kRuleRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_rule_create(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* dsl  = doc["dsl"]  | (const char*)nullptr;
    const char* name = doc["name"] | "";
    if (!dsl || dsl[0] == '\0') return API_BAD_REQUEST;

    uint8_t hap_buf[512];
    uint16_t hap_len = 0;
    if (!hap_json_encode_rule_create(hap_buf, sizeof(hap_buf), &hap_len, name, dsl)) {
        return API_INTERNAL_ERROR;
    }

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRuleRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::RULE_CREATE, hap_buf, hap_len,
                           rsp.get(), kRuleRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }

    JsonDocument rd;
    bool parse_ok = true;
    if (deserializeJson(rd, rsp.get(), got) == DeserializationError::Ok) {
        parse_ok = rd["ok"] | false;
    }

    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    // Copy rule response verbatim either way; on parse failure caller
    // can extract the err message for the 400 reply.
    return parse_ok ? API_OK : API_BAD_REQUEST;
}

extern "C" ApiStatus api_rule_delete(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t rule_id = doc["id"] | (uint16_t)0;
    if (rule_id == 0) return API_BAD_REQUEST;

    uint8_t hap_buf[64];
    uint16_t hap_len = 0;
    if (!hap_json_encode_rule_delete(hap_buf, sizeof(hap_buf), &hap_len, rule_id)) {
        return API_INTERNAL_ERROR;
    }
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRuleRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::RULE_DELETE, hap_buf, hap_len,
                           rsp.get(), kRuleRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_rule_enable(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t rule_id = doc["id"] | (uint16_t)0;
    if (rule_id == 0) return API_BAD_REQUEST;
    bool enabled = doc["enabled"] | false;

    uint8_t hap_buf[64];
    uint16_t hap_len = 0;
    if (!hap_json_encode_rule_update(hap_buf, sizeof(hap_buf), &hap_len,
                                      rule_id, enabled)) {
        return API_INTERNAL_ERROR;
    }
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRuleRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::RULE_UPDATE, hap_buf, hap_len,
                           rsp.get(), kRuleRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// PUT /api/rules/{id} — args: {"id":N,"dsl":"...","name":"..."}
extern "C" ApiStatus api_rule_update(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t rule_id = doc["id"] | (uint16_t)0;
    if (rule_id == 0) return API_BAD_REQUEST;
    const char* dsl  = doc["dsl"]  | (const char*)nullptr;
    const char* name = doc["name"] | "";
    if (!dsl || dsl[0] == '\0') return API_BAD_REQUEST;

    uint8_t hap_buf[320];
    uint16_t hap_len = 0;
    if (!hap_json_encode_rule_update_dsl(hap_buf, sizeof(hap_buf), &hap_len,
                                           rule_id, name, dsl)) {
        return API_INTERNAL_ERROR;
    }
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRuleRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::RULE_UPDATE_DSL, hap_buf, hap_len,
                           rsp.get(), kRuleRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

