// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Caps (matching spec §5.4)
#define REMOTE_NVS_URL_MAX     128
#define REMOTE_NVS_TOKEN_MAX    96
#define REMOTE_NVS_DEVID_MAX    32

// Read the config blob. Returns true on success (all fields populated
// with stored or default value). False on persistent error.
//
// Empty url or empty token after the read means "not configured —
// stay DISABLED".
bool remote_nvs_load(bool* enabled_out,
                     char* url_out,    size_t url_cap,
                     char* token_out,  size_t token_cap,
                     char* devid_out,  size_t devid_cap);

// Write the config. Pass empty string for url/token/devid to KEEP the
// stored value (no-op for that field). Pass non-empty to overwrite.
bool remote_nvs_save(bool enabled,
                     const char* url,
                     const char* token,
                     const char* devid);

// Erase url+token (keep `enabled` flag).
bool remote_nvs_forget_creds(void);

#ifdef __cplusplus
}
#endif
