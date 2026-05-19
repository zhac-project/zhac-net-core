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

// ── Groups ───────────────────────────────────────────────────────────────
static void grp_parse_members_from_body(const char* body, GrpRecord& r) {
    r.member_count = 0;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    JsonArray arr = doc["members"].as<JsonArray>();
    for (JsonObject obj : arr) {
        if (r.member_count >= GRP_MAX_MEMBERS) break;
        const char* ieee_str = obj["ieee"] | (const char*)nullptr;
        if (!ieee_str) continue;
        r.members[r.member_count++] = { parse_ieee(ieee_str),
                                          obj["ep"] | (uint8_t)1 };
    }
}

extern "C" ApiStatus api_group_list(const char* /*body*/, size_t /*body_len*/,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    static GrpRecord all[GRP_MAX_GROUPS];
    uint16_t cnt = grp_load_all(all, GRP_MAX_GROUPS);
    size_t pos = 0;
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "{\"groups\":[");
    for (uint16_t i = 0; i < cnt && pos < rsp_cap; i++) {
        if (i) rsp_buf[pos++] = ',';
        char tmp[256];
        size_t n = grp_to_json(all[i], tmp, sizeof(tmp));
        if (pos + n < rsp_cap) { memcpy(rsp_buf + pos, tmp, n); pos += n; }
    }
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "]}");
    if (rsp_len) *rsp_len = pos;
    return API_OK;
}

extern "C" ApiStatus api_group_create(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;

    GrpRecord r{};
    r.id = grp_next_id();
    strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    grp_parse_members_from_body(body, r);

    if (!grp_save(r)) return API_INTERNAL_ERROR;

    size_t n = grp_to_json(r, rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_group_get(const char* body, size_t body_len,
                                    char* rsp_buf, size_t rsp_cap,
                                    size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t id = doc["id"] | (uint16_t)0;

    GrpRecord r{};
    if (!grp_find(id, r)) return API_NOT_FOUND;

    size_t n = grp_to_json(r, rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_group_update(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t id = doc["id"] | (uint16_t)0;

    GrpRecord r{};
    if (!grp_find(id, r)) return API_NOT_FOUND;

    if (!doc["name"].isNull()) strncpy(r.name, doc["name"] | "", sizeof(r.name) - 1);
    if (!doc["members"].isNull()) grp_parse_members_from_body(body, r);

    if (!grp_save(r)) return API_INTERNAL_ERROR;
    size_t n = grp_to_json(r, rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_group_delete(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t id = doc["id"] | (uint16_t)0;
    bool ok = grp_delete(id);
    const size_t n = ok ? api_write_ok(rsp_buf, rsp_cap)
                        : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_group_cmd(const char* body, size_t body_len,
                                    char* rsp_buf, size_t rsp_cap,
                                    size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    uint16_t id = doc["id"] | (uint16_t)0;

    GrpRecord r{};
    if (!grp_find(id, r) || r.member_count == 0) return API_NOT_FOUND;

    char key_str[24] = "state";
    strncpy(key_str, doc["key"] | "state", sizeof(key_str) - 1);
    int val = doc["val"] | 0;

    uint8_t sent      = 0;   // members whose SET_ACK reported ok=true
    uint8_t failed    = 0;   // members whose SET_ACK reported ok=false
    uint8_t timed_out = 0;   // members whose SET_ACK never arrived
    for (uint8_t i = 0; i < r.member_count; i++) {
        HapSetAttrReq attr{};
        attr.ieee    = r.members[i].ieee;
        attr.ep      = r.members[i].ep;
        attr.cluster = 0;
        attr.attr    = 0;
        attr.val     = val;
        {
            const size_t cap = sizeof(attr.key) - 1;
            const size_t n   = strnlen(key_str, cap);
            memcpy(attr.key, key_str, n);
            attr.key[n] = '\0';
        }

        uint8_t hap_buf[128];
        uint16_t hap_len = 0;
        if (!hap_json_encode_set_attr(hap_buf, sizeof(hap_buf), &hap_len, attr)) {
            failed++;
            continue;
        }

        char ack_buf[64];
        size_t ack_len = 0;
        bool rsp_ok = hap_roundtrip_v2(HapMsgType::SET_ATTRIBUTE,
                                         hap_buf, hap_len,
                                         ack_buf, sizeof(ack_buf),
                                         &ack_len, 3000);
        if (!rsp_ok) {
            timed_out++;
            continue;
        }
        bool cmd_ok = false;
        if (ack_len > 0) {
            JsonDocument ad;
            if (deserializeJson(ad, ack_buf, ack_len) == DeserializationError::Ok) {
                cmd_ok = ad["ok"] | false;
            }
        }
        if (cmd_ok) sent++; else failed++;
    }

    const bool ok = (sent == r.member_count);
    int n = snprintf(rsp_buf, rsp_cap,
                     "{\"ok\":%s,\"sent\":%u,\"failed\":%u,\"timed_out\":%u}",
                     ok ? "true" : "false", sent, failed, timed_out);
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}
