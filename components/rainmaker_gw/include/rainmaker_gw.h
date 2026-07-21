// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// RainMaker bridge lifecycle state. This Task-11 skeleton never advances
// past RMK_ST_DISABLED — the claim/connect state machine that actually
// drives these transitions lands in Task 13 (consumed further by Tasks 18,
// 19 per the RainMaker Bridge plan).
typedef enum {
    RMK_ST_DISABLED = 0,   // flag off, or flag on but uplink != RAINMAKER
    RMK_ST_INIT_CLAIM,     // claiming/provisioning with the RainMaker cloud
    RMK_ST_CONNECTING,     // claimed, establishing the cloud MQTT session
    RMK_ST_UNASSOCIATED,   // claimed but not yet associated with a user account
    RMK_ST_READY,          // connected and associated — bridge is live
    RMK_ST_BACKOFF,        // transient failure, retrying with backoff
    RMK_ST_CLAIM_FAILED,   // claim permanently failed (needs operator action)
} rmk_state_t;

// Entry point — call once from app_main(), after wifi_mgr_init() so the
// default event loop + netif already exist (P0 spike finding: RainMaker
// init before that runs against an uninitialized event loop). Safe to call
// unconditionally in both Kconfig states: no-op unless
// CONFIG_ZHAC_RAINMAKER_ENABLE is set AND the persisted uplink selector
// (zap_store: zhac_uplink_get()) is ZHAC_UPLINK_RAINMAKER.
void rainmaker_gw_init(void);

// Current lifecycle state.
rmk_state_t rainmaker_gw_state(void);

// RainMaker node ID once claimed. Returns "" (never NULL) until then.
const char* rainmaker_gw_node_id(void);

#ifdef __cplusplus
}
#endif
