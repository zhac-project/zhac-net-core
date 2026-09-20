// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ha_glue.cpp — Home Assistant discovery on the dual-chip S3. The devices live
// on the P4, so ha_bridge's data callbacks go through the same HAP-backed API
// handlers the web UI uses (device.list / device.get / device.attr.set). Each
// blocks for a HAP round trip; ha_bridge only calls them from its own task.
#include "ha_glue.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api_handlers.h"
#include "ha_bridge.h"
#include "s3_internal.h"   // rest_big_alloc

namespace {

constexpr size_t kListCap = 64 * 1024;   // matches api_devices.cpp's accumulator
constexpr size_t kDevCap  = 8 * 1024;
constexpr size_t kJsonCap = 4 * 1024;

bool with_device(uint64_t ieee, HaDeviceCb cb, void* ctx) {
    char body[48];
    snprintf(body, sizeof(body), "{\"ieee\":\"0x%016" PRIX64 "\"}", ieee);
    char* rsp = static_cast<char*>(rest_big_alloc(kDevCap));
    char* exposes = static_cast<char*>(rest_big_alloc(kJsonCap));
    char* attrs = static_cast<char*>(rest_big_alloc(kJsonCap));
    bool ok = false;
    size_t n = 0;
    if (rsp && exposes && attrs &&
        api_device_get(body, strlen(body), rsp, kDevCap, &n) == API_OK && n > 0) {
        JsonDocument d;
        if (!deserializeJson(d, rsp, n) && d.is<JsonObjectConst>()) {
            if (d["exposes"].isNull() || serializeJson(d["exposes"], exposes, kJsonCap) >= kJsonCap)
                snprintf(exposes, kJsonCap, "[]");
            if (d["attrs"].isNull() || serializeJson(d["attrs"], attrs, kJsonCap) >= kJsonCap)
                snprintf(attrs, kJsonCap, "{}");
            const HaDeviceSnapshot snap{ieee, d["name"] | "", d["vendor"] | "", d["model"] | "",
                                        exposes, attrs};
            cb(snap, ctx);
            ok = true;
        }
    }
    free(rsp);
    free(exposes);
    free(attrs);
    return ok;
}

void for_each_device(HaDeviceCb cb, void* ctx) {
    char* rsp = static_cast<char*>(rest_big_alloc(kListCap));
    if (!rsp) return;
    size_t n = 0;
    uint64_t* ieees = nullptr;
    size_t count = 0;
    if (api_device_list("", 0, rsp, kListCap, &n) == API_OK && n > 0) {
        JsonDocument d;
        if (!deserializeJson(d, rsp, n)) {
            JsonArrayConst arr = d.is<JsonArrayConst>() ? d.as<JsonArrayConst>()
                                                         : d["devices"].as<JsonArrayConst>();
            ieees = static_cast<uint64_t*>(rest_big_alloc(sizeof(uint64_t) * (arr.size() + 1)));
            for (JsonObjectConst row : arr) {
                const char* s = row["ieee"] | (const char*)nullptr;
                if (ieees && s) ieees[count++] = strtoull(s, nullptr, 16);
            }
        }
    }
    free(rsp);   // before the per-device round trips: 64 KB is a lot to hold
    for (size_t i = 0; i < count; i++) {
        if (ieees[i]) with_device(ieees[i], cb, ctx);
    }
    free(ieees);
}

bool set_attr(uint64_t ieee, const char* key, const char* value_json) {
    // key: snake_case, checked by ha_bridge; value_json: a JSON scalar.
    char body[192];
    const int bn = snprintf(body, sizeof(body), "{\"ieee\":\"0x%016" PRIX64 "\",\"key\":\"%s\",\"value\":%s}",
                            ieee, key, value_json);
    if (bn <= 0 || (size_t)bn >= sizeof(body)) return false;
    char rsp[96] = {};
    size_t rn = 0;
    return api_device_attr_set(body, (size_t)bn, rsp, sizeof(rsp), &rn) == API_OK &&
           strstr(rsp, "\"ok\":false") == nullptr;
}

const HaBridgePlatform kPlatform = {
    "ZHAC dual-chip (ESP32-S3 + ESP32-P4)",
    for_each_device,
    with_device,
    set_attr,
};

}  // namespace

void ha_glue_start() { ha_bridge_init(&kPlatform); }

void ha_glue_publish_attrs(uint64_t ieee, JsonObjectConst attrs) {
    if (!ha_bridge_enabled() || attrs.isNull()) return;
    for (JsonPairConst kv : attrs) {
        char v[64];
        const size_t n = serializeJson(kv.value(), v, sizeof(v));
        if (n > 0 && n < sizeof(v)) ha_bridge_publish_state(ieee, kv.key().c_str(), v);
    }
}
