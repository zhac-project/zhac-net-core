// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// RainMaker bridge lifecycle state, driven by the claim/connect state
// machine in rainmaker_gw.c (Task 13 of the RainMaker Bridge plan; consumed
// further by Tasks 18, 19).
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

// Start (or retry) the user-node mapping (association) workflow — the node
// side of pairing this device with a RainMaker user account. user_id/secret
// come from the phone app / esp-rainmaker-cli and are forwarded verbatim to
// the SDK's esp_rmaker_start_user_node_mapping(). Consumed by the
// `rainmaker.assoc.set` API op (Task 19).
//
// Safe to call unconditionally in both Kconfig states: returns
// ESP_ERR_INVALID_STATE when the flag is off, the uplink selector isn't
// RainMaker, or the RainMaker agent was never started / permanently failed
// to claim (RMK_ST_DISABLED / RMK_ST_CLAIM_FAILED) — there is no live agent
// to hand the mapping request to in either case. A successful return only
// means the workflow was triggered, not that the mapping itself succeeded;
// watch rainmaker_gw_state() for the RMK_ST_READY transition.
esp_err_t rainmaker_gw_assoc_start(const char* user_id, const char* secret);

#ifdef __cplusplus
}
#endif
