// SPDX-License-Identifier: AGPL-3.0-or-later
// rmk_store — RainMaker exposed-device set. Tracks which devices (by IEEE)
// are currently exposed as RainMaker nodes/params, so Tasks 16-18 can
// add/remove/reconcile membership without re-deriving it from the whole
// device list each time. Pure table half is host-tested; the NVS half
// ("zhac_rmk" namespace, single "set" blob) is a thin load/save wrapper,
// split the same way as dgm_store.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RMK_MAX_DEVS 10
typedef struct { uint64_t ieee[RMK_MAX_DEVS]; size_t count; } RmkTable;

// ── Pure table operations (no NVS, unit-tested) ──────────────────────────
// Ensure ieee is a member. Dedups (already-present -> true, no growth).
// Returns false only when the table is full and ieee is not already present.
bool   rmk_tbl_add(RmkTable* t, uint64_t ieee);
// Remove ieee. Returns true iff it was present. Implemented as swap-with-
// last (rmk_store.c) for O(1) removal without shifting the tail — survivor
// ORDER IS NOT PRESERVED across a removal. Callers (rmk_boot_restore's
// iteration, rmk_bridge_list's enumeration, etc.) must not assume the
// table's iteration order is stable or matches insertion order.
bool   rmk_tbl_remove(RmkTable* t, uint64_t ieee);
// Returns true iff ieee is currently a member.
bool   rmk_tbl_contains(const RmkTable* t, uint64_t ieee);
// Drop every member not present in live[0..nlive). Returns the number
// dropped.
size_t rmk_tbl_reconcile(RmkTable* t, const uint64_t* live, size_t nlive);

// ── NVS-backed store ("zhac_rmk" namespace, "set" blob) — IDF-only ───────
void rmk_store_init(void);
// Load the persisted table into *out (zeroed if absent/corrupt). Returns
// true iff a valid blob was found.
bool rmk_store_load(RmkTable* out);
// Persist *t. Returns true on success.
bool rmk_store_save(const RmkTable* t);

#ifdef __cplusplus
}
#endif
