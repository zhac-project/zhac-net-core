// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// In-process NVS stub for host tests. Implements the kv_* abstraction
// declared in remote_nvs_kv.h using an unordered_map. Only linked
// into host-test binaries.

#include "remote_nvs_kv.h"
#include <unordered_map>
#include <string>
#include <cstring>

namespace {
std::unordered_map<std::string, std::string> g_store;
}

extern "C" bool kv_get_str(const char* ns, const char* key,
                           char* out, size_t cap) {
    std::string k = std::string(ns) + ":" + key;
    auto it = g_store.find(k);
    if (it == g_store.end()) return false;
    if (it->second.size() + 1 > cap) return false;
    std::memcpy(out, it->second.c_str(), it->second.size() + 1);
    return true;
}

extern "C" bool kv_set_str(const char* ns, const char* key, const char* val) {
    g_store[std::string(ns) + ":" + key] = val ? val : "";
    return true;
}

extern "C" bool kv_get_u8(const char* ns, const char* key, uint8_t* out) {
    std::string k = std::string(ns) + ":" + key;
    auto it = g_store.find(k);
    if (it == g_store.end()) return false;
    *out = (uint8_t)(it->second.empty() ? 0 : (uint8_t)it->second[0]);
    return true;
}

extern "C" bool kv_set_u8(const char* ns, const char* key, uint8_t v) {
    g_store[std::string(ns) + ":" + key] = std::string(1, (char)v);
    return true;
}

extern "C" bool kv_erase(const char* ns, const char* key) {
    g_store.erase(std::string(ns) + ":" + key);
    return true;
}

extern "C" bool kv_commit(const char* /*ns*/) { return true; }

extern "C" void kv_reset_for_test(void) { g_store.clear(); }
