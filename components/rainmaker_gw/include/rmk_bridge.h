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
// uint64_t/const char*/uint8_t/int32_t/rmk_expose_t/esp_err_t/bool/void.
//
// Owns the per-device registry of currently-exposed ZHAC devices and their
// RainMaker param handles. Task 18 (device-pool <-> exposed-set
// reconciliation) is the intended caller of the expose/unexpose/gone entry
// points below; this task (16) only fills in what they DO.
//
// REVISION (post-review fix): the original cut of this file subscribed to
// event_bus (EventType::ZCL_ATTR / SHADOW_OPTIMISTIC) for the OUT
// direction's input. That mechanism was correct and compiled, but code
// review proved it can never fire on this target: device_shadow is a
// linked-but-dead dependency in net-core (zero call sites for
// device_shadow_init/_process/_update_optimistic anywhere in this repo) —
// nothing ever publishes those event types on S3. The real S3-side fan-out
// for device state is main/hap_bridge.cpp's HapMsgType::BULK_STATE_UPDATE
// handler, which talks directly to ws_event_broadcast()/mqtt_gw_publish(),
// bypassing device_shadow/event_bus entirely. This header now exposes a
// direct-call entry point (rmk_bridge_on_attr_update) plus a cheap
// activity check (rmk_bridge_active) for hap_bridge.cpp to call inline —
// see rmk_bridge.c's file banner for the full analysis.
#pragma once
#include <stdbool.h>
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
// table) plus a standard Name param (enables in-app rename), assigns the
// plan's primary param, registers a log-only stub write callback (Task 17
// replaces it with a real command dispatch), adds the device to the node,
// and records it in the bridge's internal registry.
//
// The registry entry IS this bridge's exposed-set filter: the OUT handler
// (rmk_bridge_on_attr_update) only ever acts on a device that is currently
// registered here, so a device exposed via this call starts receiving
// state reports as soon as a matching attribute update arrives (see
// rmk_bridge_active / rmk_bridge_on_attr_update). Nothing in this file
// consults rmk_store's NVS-backed set at runtime — see the design note at
// the top of rmk_bridge.c for why that's sufficient rather than a second
// source of truth.
//
// `name` must be unique among currently-exposed devices (RainMaker
// requires unique device names within a node); this function does not
// check that — it's the caller's (Task 18's) responsibility.
//
// Returns ESP_ERR_INVALID_STATE if the flag is off or the bridge was never
// started (RMK_ST_DISABLED — see rainmaker_gw_state()); ESP_ERR_INVALID_ARG
// for a null name; ESP_ERR_INVALID_STATE if ieee is already exposed OR if
// the registry slot reserved for this call was reclaimed by a concurrent
// unexpose for the same ieee while the esp_rmaker_* setup was in flight
// (TOCTOU close — the built SDK objects are torn down, not leaked, in that
// case); ESP_ERR_NO_MEM if the registry (RMK_MAX_DEVS entries, rmk_store.h)
// is full; ESP_FAIL if esp_rmaker_device_create itself fails. A failure to
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
// "wasn't exposed anyway". No-op (including when the flag is off) if ieee
// isn't currently exposed. Called unconditionally from
// main/hap_bridge.cpp's HapMsgType::DEVICE_LEAVE handler (cheap no-op on a
// flag-off build via the stub below) — pulls the unpair cleanup forward
// from Task 18 rather than waiting for its own registry-reconciliation
// pass.
void rmk_bridge_on_device_gone(uint64_t ieee);

// Cheap activity check: true iff the bridge is RMK_ST_READY AND at least
// one device is currently exposed. Meant to be called unconditionally,
// every time a caller is *about* to do real work on the bridge's behalf
// (main/hap_bridge.cpp gates its per-attribute JSON parse behind this so a
// non-RainMaker / not-yet-associated build pays only this one cheap check,
// never the parse). Flag-off builds: always false, zero cost beyond the
// call itself (the stub is defined outside any #if so it links either
// way).
bool rmk_bridge_active(void);

// Bridge OUT direction, called directly (no event envelope) once per
// changed attribute: `key` is the shadow-style attribute name (e.g.
// "brightness", "temperature" — matches rmk_typemap.c's zhac_key column),
// `val_type` follows ValType semantics (zcl_attribute.h: VAL_INT=1,
// VAL_BOOL=2, VAL_STR=3, VAL_FLOAT=4) and `val` is the *raw shadow-style*
// value — for VAL_FLOAT this MUST already be the value×100 fixed-point
// convention (matches ShadowAttr/ZclAttrEvent elsewhere in this codebase;
// the typemap's RMK_CONV_DIV100 divides by 100 unconditionally). Callers
// parsing a wire format that carries an *already-real* float (this
// bridge's own hap_bridge.cpp caller does, see its own comment) must
// reconstruct that convention themselves before calling — do not pass an
// unscaled real value tagged VAL_FLOAT.
//
// No-op if ieee isn't currently exposed, if no exposed param matches
// `key`, if `val_type` is VAL_STR/VAL_NONE (no exposed param is ever
// string-typed), or if the bridge isn't RMK_ST_READY. Runs whatever thread
// the caller runs on (main/hap_bridge.cpp's task_hap) — allocation-free,
// same discipline as before: the one esp_rmaker_* SDK call happens outside
// this bridge's own registry lock.
//
// `force` distinguishes the two callers by intent, because they must be
// throttled differently against RainMaker's MQTT publish budget (1 token /
// 5 s refill — see rmk_bridge.c):
//   - force=false  REPORT path (hap_bridge.cpp's BULK_STATE_UPDATE hook,
//     i.e. real device reports). Skip-unchanged throttled against the last
//     value reported for this (ieee, key), AND — for continuous (non-bool)
//     params like temperature/humidity/brightness — rate-limited to at most
//     one report per CONFIG_ZHAC_RMK_MIN_REPORT_INTERVAL_S, so a chatty
//     sensor cannot drain the budget and starve command reports (or trip the
//     cloud's fair-use daily message cap).
//   - force=true   COMMAND path (api_device_attr_set, i.e. a SPA/cloud-UI/
//     REST write the user just made). Bypasses BOTH throttles: a user
//     command must always re-assert to the cloud, even when last_reported
//     has drifted from the cloud's actual state (a node reconnect resets the
//     cloud param to its default while this registry keeps the old value).
void rmk_bridge_on_attr_update(uint64_t ieee, const char* key,
                               uint8_t val_type, int32_t val, bool force);

// Idempotent liveness marker — logs once that the bridge OUT direction is
// live. Historically this subscribed to event_bus (see rmk_bridge.c's file
// banner for why that was removed); kept as a thin no-op-but-safe function
// so rainmaker_gw.c's existing call sites (both RMK_ST_READY transitions)
// don't need editing as part of this fix. A future cleanup could fold this
// into rmk_bridge_active() or drop it entirely along with those call
// sites. No-op if the flag is off.
void rmk_bridge_attach(void);

// ── Bridge IN: injected device-attr writer (Task 17) ─────────────────────
// rmk_bridge.c lives in this component and must not #include main/'s
// headers — but the real write path needs main/api_handlers.h's
// device_attr_set_core (Task 15). Wired by dependency injection instead:
// this typedef mirrors device_attr_set_core's REAL signature (which grew
// beyond the RainMaker-bridge plan's original 4-arg sketch to carry
// ep/cluster/attr plus an out-param for the ack "ok" bit) with exactly one
// substitution — the return type. device_attr_set_core returns `ApiStatus`,
// a main/-only enum this header cannot name without the same layering
// violation, so the injected function returns plain esp_err_t instead.
// main/main.cpp registers a thin adapter matching this exact typedef that
// calls device_attr_set_core() and maps its ApiStatus onto ESP_OK/ESP_FAIL;
// *cmd_ok_out is passed through untouched so the write-cb keeps the same
// "transport failed" vs "device NAK'd" distinction the JSON handler has.
typedef esp_err_t (*rmk_attr_write_fn)(uint64_t ieee, const char* key,
                                       int32_t val, uint8_t ep,
                                       uint16_t cluster, uint16_t attr,
                                       bool* cmd_ok_out);

// Register the real cloud -> device writer. Call unconditionally from
// app_main() right after rainmaker_gw_init() (main.cpp) — safe/no-op on a
// flag-off build: this setter is defined outside any #if so it always
// links, and storing the pointer when the flag is off is harmless since
// rmk_bridge_expose_device's own flag-off stub never registers a write-cb
// with the SDK in the first place, so nothing would ever call it back. A
// NULL fn (or never calling this setter) makes the write-cb fail closed
// with ESP_ERR_INVALID_STATE rather than silently no-op — see
// rmk_bridge.c.
void rmk_bridge_set_attr_writer(rmk_attr_write_fn fn);

// ── Task 18: API-op accessors + boot-restore / rename / republish hooks ──

// Count of currently-exposed devices (registry, not rmk_store's persisted
// set — see rmk_bridge_expose_device's own doc for why the registry is the
// single live source of truth). Backs `rainmaker.status`'s "devices" field.
// 0 on a flag-off build.
size_t rmk_bridge_device_count(void);

// One row per currently-exposed device: `type` and `name` are OWNED
// copies (Task 21 review fix, residual #1). They used to be borrowed
// pointers into the SDK's own live device object — but rmk_bridge_
// unexpose_device's teardown is now deferred onto the work queue (Task 21
// row-1 fix), so the window during which a concurrent teardown could free
// that object out from under a caller still holding a borrowed pointer
// widened from near-immediate to however long the queue takes to drain.
// A pointer copy alone would not have fixed this — it would just move the
// same race to wherever the caller eventually dereferences it (the
// original problem this whole review thread is about, one level up).
// rmk_bridge_list() now copies the actual string DATA into these
// caller-owned buffers while still holding its own lock, so the result is
// safe to read (and retain) after the call returns, unlike the old
// contract. Backs `device.rainmaker.list` / `.add` / `.remove`'s reply.
// Returns the number of rows written (<= cap). 0 on a flag-off build.
typedef struct {
    uint64_t ieee;
    char     type[32];   // e.g. "esp.device.lightbulb" (rmk_devtype_str()
                          // — longest is "esp.device.temperature-sensor",
                          // 30 chars + NUL)
    char     name[30];   // current RainMaker display name — matches the
                          // char[30] buffers this codebase already uses
                          // for this exact string everywhere else
                          // (main/api_rainmaker.cpp's `display`, main/
                          // api_devices.cpp's `name`)
} rmk_bridge_dev_info_t;
size_t rmk_bridge_list(rmk_bridge_dev_info_t* out, size_t cap);

// Immediate (non-debounced) node-config republish — thin wrapper around
// esp_rmaker_report_node_details(). esp_rmaker_* types/calls must not leave
// this component (see this header's own file banner + CMakeLists.txt's
// PRIVATE optional_requires on espressif__esp_rainmaker), so the DEBOUNCE
// itself lives in main/api_rainmaker.cpp (which already depends on
// esp_timer for unrelated reasons) — that file's 2 s one-shot timer calls
// this function, never esp_rmaker_report_node_details() directly. No-op if
// the flag is off or the bridge isn't RMK_ST_READY (nothing meaningful to
// report yet/anymore).
void rmk_bridge_report_node_details_now(void);

// Boot-time persisted-device restore hook (Task 18's dynamic-exposure half).
// rmk_bridge_attach() calls this exactly once, on the FIRST READY transition
// reached this boot (guarded by the same s_attached idempotency this
// function already relies on) — see rainmaker_gw.c's two RMK_ST_READY call
// sites. The restore loop itself (rmk_store_load + per-ieee re-expose via a
// live HAP GET_DEVICE_BY_ID fetch) needs main/api_rainmaker.cpp's
// rmk_expose_device_from_ieee() helper, which needs hap_protocol/hap_json —
// this component's CMakeLists.txt deliberately has no REQUIRES on any of
// those (same "stay HAP-agnostic" layering rule rmk_attr_write_fn documents
// above), so the capability is injected here the same way: main.cpp
// registers main/api_rainmaker.cpp's rmk_boot_restore() via this setter
// right after rmk_bridge_set_attr_writer(). Safe/no-op on a flag-off build
// (setter defined outside any #if); a flag-on build that never wires the
// setter just means no persisted devices come back (logged, not fatal).
typedef void (*rmk_boot_restore_fn)(void);
void rmk_bridge_set_boot_restore(rmk_boot_restore_fn fn);

// ZHAC-side rename (main/api_devices.cpp's device.rename success path)
// mirrored onto an exposed device's RainMaker "Name" param, so the phone
// app reflects the new name without waiting for a reboot/re-expose. No-op
// (including when the flag is off) if ieee isn't currently exposed.
// `new_name` should already be UTF-8-sanitized by the caller (api_devices.cpp
// already runs device.rename's name through utf8_safe_copy before this
// point — see the WS UTF-8 poison history this codebase has twice fixed).
// This is the rename-only counterpart to rmk_bridge_on_device_gone() above
// — DEVICE_LEAVE cleanup already exists (Task 16) and is not touched here.
void rmk_bridge_on_device_renamed(uint64_t ieee, const char* new_name);

#ifdef __cplusplus
}
#endif
