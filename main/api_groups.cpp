// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared command handlers — domain split from former api_handlers.cpp.
// Handlers here are PURE business logic. Auth, rate-limiting, URI parsing,
// and transport framing live in rest_*.cpp (HTTP) and ws_bridge.cpp (WS).

#include "api_handlers.h"
#include "s3_internal.h"
#include "log_ring.h"
#include "groups_store.h"
#include "json_buf.h"
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
#include "esp_attr.h"
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
    // PSRAM: ~9.5 KB of group records, refilled from NVS on every call —
    // cold path, would otherwise be the single largest dram0 .bss symbol.
    //
    // P4-T29 (FINDINGS §6): `group.list` is remote-allow-listed, so this runs
    // on BOTH the httpd task and task_remote. The `all[]` scratch is file-
    // static and SHARED between those contexts — a concurrent second
    // group.list would refill it mid-serialisation here (torn read). Hold the
    // store lock across the load AND the read-back loop so the snapshot we
    // serialise is the one we loaded. (Recursive mutex — grp_load_all re-takes
    // it internally; create/update/delete from the other task block until we
    // release, matching the wifi-scan-snapshot pattern P2-T16 used.)
    EXT_RAM_BSS_ATTR static GrpRecord all[GRP_MAX_GROUPS];
    grp_store_lock();
    uint16_t cnt = grp_load_all(all, GRP_MAX_GROUPS);
    // CODEX H-01: every append goes through JsonWriter (never advances past
    // rsp_cap). Each group is serialised into scratch and appended only if it
    // fits WITH room reserved for a separating comma and the closing "]}", so
    // the response is always valid JSON even when it fills and later groups are
    // dropped rather than silently corrupting the buffer.
    JsonWriter w(rsp_buf, rsp_cap);
    w.raw("{\"groups\":[");
    uint16_t emitted = 0, dropped = 0;
    for (uint16_t i = 0; i < cnt; i++) {
        // Sized for a maximum group (16 members + name) INCLUDING the worst
        // case where a legacy poisoned name expands to � escapes (31
        // bytes × 6 ≈ 186 B for the name alone, ~830 B total) — at 768 such a
        // group failed closed and silently vanished from the list.
        char tmp[1024];
        size_t n = grp_to_json(all[i], tmp, sizeof(tmp));
        if (n == 0) { dropped++; continue; }
        if (w.len() + (emitted ? 1u : 0u) + n + 2 > rsp_cap) { dropped++; continue; }
        if (emitted) w.ch(',');
        w.raw(tmp, n);   // length-bounded: tmp is not guaranteed NUL-terminated
        emitted++;
    }
    w.raw("]}");
    grp_store_unlock();
    if (dropped) {
        ESP_LOGW("api_groups", "group.list: %u group(s) dropped — response cap %zu too small",
                 (unsigned)dropped, rsp_cap);
    }
    size_t len = w.finish();
    if (rsp_len) *rsp_len = len;
    return len ? API_OK : API_INTERNAL_ERROR;
}

extern "C" ApiStatus api_group_create(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;

    GrpRecord r{};
    // UTF-8-safe: byte-blind strncpy split a multibyte char at the name[32]
    // bound, persisting invalid UTF-8 — every later group.list WS text frame
    // then closed the socket (Chrome + Tomcat validate UTF-8 per RFC 6455).
    utf8_safe_copy(r.name, sizeof(r.name), doc["name"] | "");
    grp_parse_members_from_body(body, r);

    // P4-T29: atomic id-alloc + persist (was next_id()+save() with a TOCTOU
    // window between them — two concurrent creators could grab the same slot).
    if (!grp_create(r)) return API_INTERNAL_ERROR;

    size_t n = grp_to_json(r, rsp_buf, rsp_cap);
    if (n == 0) return API_INTERNAL_ERROR;   // don't emit `"data":` with an empty body
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
    if (n == 0) return API_INTERNAL_ERROR;   // don't emit `"data":` with an empty body
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

    if (!doc["name"].isNull()) utf8_safe_copy(r.name, sizeof(r.name), doc["name"] | "");
    if (!doc["members"].isNull()) grp_parse_members_from_body(body, r);

    if (!grp_save(r)) return API_INTERNAL_ERROR;
    size_t n = grp_to_json(r, rsp_buf, rsp_cap);
    if (n == 0) return API_INTERNAL_ERROR;   // don't emit `"data":` with an empty body
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

    // Command mode: the SPA sends {cluster, cmd} for a ZCL command (On/Off/
    // Toggle/Identify) to fan out to each member. Legacy callers send {key, val}
    // for an attribute set. Disambiguated by the presence of `cmd`. (Previously
    // this handler only read key/val, so the SPA's cluster/cmd were dropped and
    // every button defaulted to key="state", val=0 → all buttons sent Off.)
    const bool     cmd_mode    = !doc["cmd"].isNull();
    const uint16_t zcl_cluster = doc["cluster"] | (uint16_t)0;
    const uint8_t  zcl_cmd     = (uint8_t)(doc["cmd"] | 0);
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
        attr.val     = val;
        if (cmd_mode) {
            // Raw ZCL command: cluster + cmd (carried in attr.attr), flagged by
            // the reserved "__zclcmd__" key so the P4 sends the command rather
            // than doing an attribute set.
            attr.cluster = zcl_cluster;
            attr.attr    = zcl_cmd;
            memcpy(attr.key, "__zclcmd__", sizeof("__zclcmd__"));
        } else {
            attr.cluster = 0;
            attr.attr    = 0;
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
