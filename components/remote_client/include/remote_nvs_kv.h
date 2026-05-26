// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Internal abstraction. remote_nvs.cpp calls these; the IDF build
// supplies an inline impl in remote_nvs.cpp that wraps real nvs_*
// calls. The host build links remote_nvs_stub.cpp instead.
//
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

bool kv_get_str(const char* ns, const char* key, char* out, size_t cap);
bool kv_set_str(const char* ns, const char* key, const char* val);
bool kv_get_u8 (const char* ns, const char* key, uint8_t* out);
bool kv_set_u8 (const char* ns, const char* key, uint8_t v);
bool kv_erase  (const char* ns, const char* key);
bool kv_commit (const char* ns);

// Host-test only — declared here so headers compile; not implemented
// in the IDF build.
void kv_reset_for_test(void);

#ifdef __cplusplus
}
#endif
