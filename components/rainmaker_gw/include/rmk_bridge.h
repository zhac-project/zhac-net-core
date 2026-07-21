// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// rmk_bridge — shadow -> RainMaker param bridge (Task 16 of the RainMaker
// Bridge plan; bridge OUT direction). Gets its own header rather than
// growing rainmaker_gw.h, matching this component's existing one-header-
// per-module convention (rmk_store.h <-> rmk_store.c, rmk_typemap.h <->
// rmk_typemap.c). Also keeps rainmaker_gw.h's documented invariant intact
// ("no esp_rmaker_* type ever appears in rainmaker_gw.h", CMakeLists.txt) —
// this header doesn't need one either: every signature below is plain
// uint64_t/const char*/rmk_expose_t/esp_err_t/void.
//
// Owns the per-device registry of currently-exposed ZHAC devices and their
// RainMaker param handles. Task 18 (device-pool <-> exposed-set
// reconciliation) is the intended caller of all four entry points below;
// this task (16) only fills in what they DO.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rmk_typemap.h"

#ifdef __cplusplus
extern "C" {
#endif

// Expose one device as a RainMaker node-device: classifies it
// (rmk_classify), builds its param plan (rmk_build_params), creates the
// esp_rmaker device + one esp_rmaker param per plan entry (UI type +
// bounds + read/write property flags as authored in rmk_typemap.c's
// table), assigns the plan's primary param, registers a log-only stub
// write callback (Task 17 replaces it with a real command dispatch), adds
// the device to the node, and records it in the bridge's internal
// registry.
//
// The registry entry IS this bridge's exposed-set filter: the OUT handler
// (shadow event -> param report) only ever acts on a device that is
// currently registered here, so a device exposed via this call starts
// receiving state reports as soon as a matching shadow event arrives (see
// rmk_bridge_attach). Nothing in this file consults rmk_store's NVS-backed
// set at runtime — see the design note at the top of rmk_bridge.c for why
// that's sufficient rather than a second source of truth.
//
// `name` must be unique among currently-exposed devices (RainMaker
// requires unique device names within a node); this function does not
// check that — it's the caller's (Task 18's) responsibility.
//
// Returns ESP_ERR_INVALID_STATE if the flag is off or the bridge was never
// started (RMK_ST_DISABLED — see rainmaker_gw_state()); ESP_ERR_INVALID_ARG
// for a null name; ESP_ERR_INVALID_STATE if ieee is already exposed;
// ESP_ERR_NO_MEM if the registry (RMK_MAX_DEVS entries, rmk_store.h) is
// full; ESP_FAIL if esp_rmaker_device_create itself fails. A failure to
// attach the device to the node (esp_rmaker_node_add_device) is logged but
// not fatal — the registry entry is kept so a later unexpose can still
// clean it up.
esp_err_t rmk_bridge_expose_device(uint64_t ieee, const char* name,
                                   const rmk_expose_t* ex, size_t n);

// Remove a device from the RainMaker node (esp_rmaker_node_remove_device +
// esp_rmaker_device_delete, in that order per the SDK's own note) and drop
// its registry entry. Returns ESP_ERR_NOT_FOUND if ieee isn't currently
// exposed; ESP_ERR_INVALID_STATE if the flag is off.
esp_err_t rmk_bridge_unexpose_device(uint64_t ieee);

// Device left the ZHAC pool entirely (DEVICE_LEAVE / hard delete). Same
// cleanup as rmk_bridge_unexpose_device but void-returning and silent on
// "wasn't exposed anyway" — matches the shape Task 18's pool-change
// handler needs (a DEVICE_LEAVE consumer with nothing useful to do with an
// esp_err_t). No-op (including when the flag is off) if ieee isn't
// currently exposed.
void rmk_bridge_on_device_gone(uint64_t ieee);

// Subscribe to the shadow event stream (EventType::ZCL_ATTR +
// EventType::SHADOW_OPTIMISTIC — see rmk_bridge.c's design note for why
// both, and why a dedicated task is required) and start the bridge's
// small dispatch task. Call once, from rainmaker_gw.c, at every place the
// lifecycle state machine transitions into RMK_ST_READY (there are two on
// this branch: first mapping completion, and a reconnect that lands
// straight on an already-persisted mapping) — idempotent, so calling it
// more than once is a harmless no-op (logged at WARN). No-op if the flag
// is off.
void rmk_bridge_attach(void);

#ifdef __cplusplus
}
#endif
