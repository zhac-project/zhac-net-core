// SPDX-License-Identifier: AGPL-3.0-or-later
// No-op NVS host stub. grp_to_json (the H-01 target) touches no NVS; these
// exist so the groups_store.cpp TU compiles + links. Reads report NOT_FOUND so
// load paths yield empty; writes report OK.
#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* h) { if (h) *h = 1; return ESP_OK; }
static inline void      nvs_close(nvs_handle_t) {}
static inline esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*)       { return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t)        { return ESP_OK; }
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return ESP_OK; }
static inline esp_err_t nvs_erase_key(nvs_handle_t, const char*)                { return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t)                                { return ESP_OK; }
