// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the WiFi manager.
 *
 * - Calls esp_netif_init() and esp_event_loop_create_default().
 * - Reads NVS "wifi_cfg" for stored SSID/password.
 *   - If credentials exist: starts STA mode (falls back to AP after 15 failures).
 *   - If no credentials: starts AP mode immediately.
 * - Spawns the GPIO-reset polling task (5 s long-press erases creds + reboots).
 * - Spawns the DNS captive-portal task when in AP mode.
 *
 * Must be called from app_main() BEFORE creating other network-dependent tasks.
 */
void wifi_mgr_init(void);

/** Returns true when the device is currently in AP (soft-AP) mode. */
bool wifi_mgr_is_ap_mode(void);

/**
 * Copy the current IP address (STA or AP) into @p buf as a dotted-quad string.
 * Returns @p buf on success, or an empty string on failure.
 */
void wifi_mgr_get_ip_str(char* buf, size_t len);

/**
 * Return the AP-mode SSID string ("ZHAC-XXXX").
 * The pointer is valid for the lifetime of the process.
 */
const char* wifi_mgr_get_ap_ssid(void);

/**
 * Perform a blocking WiFi scan and return the number of APs found.
 * After this call, use wifi_mgr_get_scan_results() to access the records.
 * Returns 0 on error.
 */
uint16_t wifi_mgr_scan(void);

/**
 * Copy a snapshot of the most recent scan results into a caller-owned buffer.
 *
 * The internal results array is shared between the local httpd task and the
 * remote-client task (wifi.scan is remote-allow-listed). Returning a pointer
 * to the live static let a concurrent scan overwrite the array mid-read; this
 * snapshot API copies under the scan mutex so the caller gets a stable copy.
 *
 * @param out  caller-owned buffer to receive the records (may be nullptr only
 *             if @p max is 0).
 * @param max  capacity of @p out in records.
 * @return number of records actually copied (min of stored count and @p max).
 */
uint16_t wifi_mgr_get_scan_results(wifi_ap_record_t* out, uint16_t max);

/**
 * Erase stored WiFi credentials from NVS and reboot.
 * Does not return.
 */
void wifi_mgr_forget_and_reboot(void);

#ifdef __cplusplus
}
#endif
