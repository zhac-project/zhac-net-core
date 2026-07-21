// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// rmk_bridge — shadow -> RainMaker param bridge (Task 16 of the RainMaker
// Bridge plan; bridge OUT direction). Fills the Task-11/13 stub.
//
// Compiled as C++ despite the .c name/extension — same reason as
// rainmaker_gw.c (see this component's CMakeLists.txt for the full
// rationale on that file): this translation unit needs event_bus.h
// directly to subscribe to shadow-change events, and event_bus.h is
// C++-only (`enum class EventType`, `std::function`-typed EventHandler).
// rmk_bridge.h's own `extern "C"` guard keeps this file's PUBLIC API at
// plain C linkage regardless, so nothing about the component's external
// contract changes.
//
// ── Subscription mechanism — brief's Step 1, corrected ──────────────────
// The brief's original mental model ("mqtt_gw_s3.cpp subscribes to
// shadow/event-bus updates the same way remote_client does — mirror that")
// does not hold on this branch. Verified by grep across both
// zhac-components and this net-core worktree: neither
// zhac-components/components/mqtt_gw/mqtt_gw_s3.cpp nor
// components/remote_client/*.cpp calls event_bus_subscribe, references
// EventType, or touches ZclAttrEvent at all — mqtt_gw_s3.cpp is pure MQTT
// *transport* (broker connect/publish/subscribe), and remote_client relays
// WS command envelopes, neither one a shadow-event consumer. The only
// production event_bus_subscribe call sites anywhere in either repo are in
// zhac-components/components/simple_rules/simple_rules.cpp — and this
// net-core worktree's own main/idf_component.yml does not even list
// simple_rules as a dependency, so there is no shared dispatcher already
// running on S3 to piggyback on either.
//
// What IS true and load-bearing (read from
// zhac-components/components/device_shadow/device_shadow.cpp and
// components/event_bus/event_bus.cpp directly, not from stale docs — see
// below): device_shadow's publish_staged() emits EventType::ZCL_ATTR on
// every processed attribute (the device_shadow_process() path, which is
// what the S3-side HAP frame handler calls to mirror P4's bulk 0x60
// updates into the local shadow) and EventType::SHADOW_OPTIMISTIC on
// device_shadow_update_optimistic() (the no-report-device / optimistic
// relay path) — both carry the byte-identical ZclAttrEvent payload (ieee,
// val_type, key, int_val). That part of the brief's model was right; only
// the "where to copy the subscription from" pointer was stale.
//
// event_bus's OWN header/impl (components/event_bus/include/event_bus.h,
// event_bus.cpp — the event_bus/README.md is stale on this point, still
// describing a retired `event_bus_subscribe_queue`/`_drain_queue` pair
// that doesn't exist in the current header) make the actual contract
// unambiguous: event_bus_publish() only ever enqueues into each
// subscriber's private per-slot queue; the EventHandler passed to
// event_bus_subscribe() is invoked SOLELY inside drain_slot(), reached via
// event_bus_drain_handle() — "call from the subscriber's own task" per the
// header comment. Nothing on S3 currently drains anything. So, like
// simple_rules does for its own (unrelated, cron-driven) needs via its
// dedicated rule_cron task, this bridge must both subscribe AND run its
// own small drain task — there is no free ride available to mirror.
#include "sdkconfig.h"
#include "rmk_bridge.h"

#include "esp_log.h"

#if CONFIG_ZHAC_RAINMAKER_ENABLE
#include <cstring>

#include "rainmaker_gw.h"
#include "rmk_store.h"      // RMK_MAX_DEVS cap ONLY — see note above
                             // dev_by_ieee(): this bridge's own registry is
                             // the exposed-set filter; rmk_tbl_* (the NVS-
                             // backed set proper, owned by Task 18) is
                             // never called from this file.
#include "event_bus.h"
#include "zcl_attribute.h"  // ValType (VAL_STR/VAL_NONE guard below);
                             // pulled in transitively via event_bus.h too,
                             // included directly since it's used by name.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_rmaker_core.h"

static const char* TAG = "rmk_bridge";

// ── Registry ──────────────────────────────────────────────────────────
// One entry per exposed device; RMK_MAX_DEVS (rmk_store.h, currently 10)
// entries, matching rmk_store's own cap 1:1 since the two are meant to
// track the same exposed-set (Task 18's job to keep them in lockstep).
struct RmkBridgeParam {
    rmk_param_t          plan;          // copied by value — zhac_key/
                                         // rm_name/rm_type/rm_ui all point
                                         // at rmk_typemap.c's static
                                         // string-literal table, so the
                                         // copy never dangles.
    esp_rmaker_param_t*  handle;        // NULL if esp_rmaker_param_create
                                         // failed for this one param.
    int32_t              last_reported; // raw shadow int_val (pre-
                                         // conversion) as of the last
                                         // report — the skip-unchanged
                                         // throttle key.
    bool                 has_last;      // false until the first report.
};

struct RmkBridgeDev {
    bool                  in_use;
    uint64_t              ieee;
    esp_rmaker_device_t*  device;
    RmkBridgeParam        params[RMK_MAX_PARAMS];
    size_t                param_count;
};

static RmkBridgeDev    s_reg[RMK_MAX_DEVS];
static bool            s_attached = false;
static EventSubHandle  s_h_attr   = EVENT_SUB_INVALID;
static EventSubHandle  s_h_opt    = EVENT_SUB_INVALID;

// Guards s_reg[] against a concurrent Task-18 caller (expose/unexpose,
// presumably driven off DEVICE_JOIN/DEVICE_LEAVE, possibly on a different
// task than rmk_bridge_evt_task below) racing the OUT handler's read side.
// Lazy-created, null-guarded before first use — same accepted pattern
// event_bus.cpp itself uses for s_bus_mtx ("pre-init single-threaded use"):
// the only theoretical race is two callers both hitting a genuinely first
// call to this file at once, which in practice means Task 18 racing itself
// before it has even run once. Held only across registry scan/mutation;
// released before any esp_rmaker_* SDK call (some may block/log).
static SemaphoreHandle_t s_reg_mtx = nullptr;
static inline void reg_lock() {
    if (!s_reg_mtx) s_reg_mtx = xSemaphoreCreateMutex();
    if (s_reg_mtx) xSemaphoreTake(s_reg_mtx, portMAX_DELAY);
}
static inline void reg_unlock() {
    if (s_reg_mtx) xSemaphoreGive(s_reg_mtx);
}

// Lock held by caller.
static RmkBridgeDev* dev_by_ieee_locked(uint64_t ieee) {
    for (size_t i = 0; i < RMK_MAX_DEVS; i++)
        if (s_reg[i].in_use && s_reg[i].ieee == ieee) return &s_reg[i];
    return nullptr;
}
// Lock held by caller.
static RmkBridgeDev* free_slot_locked(void) {
    for (size_t i = 0; i < RMK_MAX_DEVS; i++)
        if (!s_reg[i].in_use) return &s_reg[i];
    return nullptr;
}

// Log-only STUB write callback (architecture cut, Task 16 brief item 3):
// Task 17 replaces this with a real zhc_adapter command dispatch. Mirrors
// Task 13's rmk_write_cb byte-for-byte — ack the value straight back so a
// phone-app toggle doesn't visually revert — but does NOT touch shadow or
// Zigbee state, since there is no command path wired yet.
static esp_err_t rmk_bridge_write_stub_cb(const esp_rmaker_device_t* device,
                                          const esp_rmaker_param_t* param,
                                          const esp_rmaker_param_val_t val,
                                          void* priv_data,
                                          esp_rmaker_write_ctx_t* ctx) {
    (void)priv_data;
    (void)ctx;
    ESP_LOGI(TAG, "WRITE-CB (stub, Task 17 pending) device=%s param=%s",
             esp_rmaker_device_get_name(device),
             esp_rmaker_param_get_name(param));
    esp_rmaker_param_update_and_report((esp_rmaker_param_t*)param, val);
    return ESP_OK;
}

// ── OUT handler: shadow event -> converted, throttled param report ──────
// Runs on rmk_bridge_evt_task (below) — the event-bus/dispatcher thread
// for this subscription. Allocation-free: fixed-size registry scan, plain
// integer/float math in rmk_typemap's conversions, no heap/string work.
static void rmk_bridge_on_shadow_event(const Event& ev) {
    // Lifecycle guard (brief's requirement). rmk_bridge_attach() is only
    // called on entry to RMK_ST_READY, but rainmaker_gw.c can later drop
    // to RMK_ST_BACKOFF on an MQTT disconnect without ever unsubscribing
    // (Task 13 has no detach path) — so re-check live on every event
    // rather than only gating the initial subscribe.
    if (rainmaker_gw_state() != RMK_ST_READY) return;

    const ZclAttrEvent* a = reinterpret_cast<const ZclAttrEvent*>(ev.data);
    // VAL_STR aliases int_val with str_val in the same union — reading
    // int_val for a string-valued attribute would be garbage. VAL_NONE
    // carries nothing meaningful either. Every rmk_param_t data_type is
    // 'b'/'i'/'f' (rmk_typemap.h) — no exposed param is ever string-typed
    // — so both are unconditional skips, not just "unhandled conv".
    if (a->val_type == VAL_STR || a->val_type == VAL_NONE) return;

    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(a->ieee);
    if (!d) { reg_unlock(); return; }   // not exposed — registry IS the filter

    esp_rmaker_param_t* handle = nullptr;
    esp_rmaker_param_val_t v{};
    bool changed = false;

    for (size_t i = 0; i < d->param_count; i++) {
        RmkBridgeParam* bp = &d->params[i];
        if (strcmp(bp->plan.zhac_key, a->key) != 0) continue;
        if (!bp->handle) break;   // this param's create() failed earlier

        // Skip-unchanged throttle on the RAW shadow value, checked before
        // paying for the conversion below.
        if (bp->has_last && bp->last_reported == a->int_val) break;
        bp->last_reported = a->int_val;
        bp->has_last = true;

        switch (bp->plan.conv) {
        case RMK_CONV_BRI:     v = esp_rmaker_int(rmk_bri_to_rm(a->int_val)); break;
        case RMK_CONV_CCT:     v = esp_rmaker_int(rmk_mired_to_kelvin(a->int_val)); break;
        case RMK_CONV_DIV100:  v = esp_rmaker_float((float)rmk_div100(a->int_val)); break;
        case RMK_CONV_CONTACT: v = esp_rmaker_bool(rmk_contact_to_rm(a->int_val != 0)); break;
        default:
            v = (bp->plan.data_type == 'b') ? esp_rmaker_bool(a->int_val != 0)
                                             : esp_rmaker_int(a->int_val);
            break;
        }
        handle  = bp->handle;
        changed = true;
        break;   // zhac_key is unique within one device's param plan
    }
    reg_unlock();

    // esp_rmaker_param_update_and_report queues internally (brief's own
    // note) — called outside the lock so the SDK's internal work (mutex/
    // queue push into the RainMaker work-queue task) can never block a
    // future registry access from another task.
    if (changed && handle) esp_rmaker_param_update_and_report(handle, v);
}

// ── Dispatch task ─────────────────────────────────────────────────────
// event_bus's own header comment on event_bus_drain_handle: "call from
// the subscriber's own task." Nothing on S3 currently runs a shared
// event-bus pump (see file banner above), so this bridge owns a small
// dedicated one — exactly as simple_rules owns rule_cron for its own,
// unrelated per-second timer need, not because a shared task pattern
// doesn't exist elsewhere.
//
// Stack size (4096) is a plain local constant, not registered in
// zhac-components/components/zap_common/include/task_stacks.h's kTable —
// that file lives in a different repo (zhac-components, out of this
// task's edit scope: the controller's file list for this task covers only
// components/rainmaker_gw/* and rainmaker_gw.c) and is explicitly a
// stack-monitor lookup table, not a hard requirement for a task to run.
// The stack monitor just won't show "rmk_bridge_evt" by name until a
// follow-up task adds it there.
static void rmk_bridge_evt_task(void*) {
    for (;;) {
        // Block up to 200 ms for a ZCL_ATTR event (the common case — every
        // HAP-relayed bulk attribute update); drains any backlog quickly
        // once woken (drain_slot only blocks waiting for the FIRST event).
        event_bus_drain_handle(s_h_attr, 200);
        // Short poll of the optimistic-shadow queue so a no-report Tuya
        // command relay (the whole point of SHADOW_OPTIMISTIC) isn't
        // starved behind a long idle block on the ZCL_ATTR queue above.
        event_bus_drain_handle(s_h_opt, 50);
    }
}

#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

esp_err_t rmk_bridge_expose_device(uint64_t ieee, const char* name,
                                   const rmk_expose_t* ex, size_t n) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (rainmaker_gw_state() == RMK_ST_DISABLED) return ESP_ERR_INVALID_STATE;
    if (!name || (n > 0 && !ex)) return ESP_ERR_INVALID_ARG;

    reg_lock();
    if (dev_by_ieee_locked(ieee)) { reg_unlock(); return ESP_ERR_INVALID_STATE; }
    RmkBridgeDev* d = free_slot_locked();
    if (!d) {
        reg_unlock();
        ESP_LOGW(TAG, "registry full (%d) — cannot expose ieee=0x%016llx",
                 RMK_MAX_DEVS, (unsigned long long)ieee);
        return ESP_ERR_NO_MEM;
    }
    // Reserve the slot up front (in_use=true) so a concurrent expose for a
    // different ieee can't also claim it while we're outside the lock
    // below doing the (non-trivial) esp_rmaker_* setup calls.
    memset(d, 0, sizeof(*d));
    d->in_use = true;
    d->ieee   = ieee;
    reg_unlock();

    rmk_devtype_t t = rmk_classify(ex, n);
    rmk_param_t plan[RMK_MAX_PARAMS];
    size_t pc = rmk_build_params(t, ex, n, plan, RMK_MAX_PARAMS);

    esp_rmaker_device_t* dev = esp_rmaker_device_create(name, rmk_devtype_str(t), nullptr);
    if (!dev) {
        ESP_LOGE(TAG, "esp_rmaker_device_create failed for %s", name);
        reg_lock();
        memset(d, 0, sizeof(*d));   // release the reserved slot
        reg_unlock();
        return ESP_FAIL;
    }

    esp_rmaker_param_t* primary = nullptr;
    RmkBridgeParam built[RMK_MAX_PARAMS];
    size_t built_count = 0;

    for (size_t i = 0; i < pc; i++) {
        const rmk_param_t* p = &plan[i];
        esp_rmaker_param_val_t initv = (p->data_type == 'b') ? esp_rmaker_bool(false)
                                      : (p->data_type == 'f') ? esp_rmaker_float(0.0f)
                                                               : esp_rmaker_int(0);
        uint8_t props = (uint8_t)(PROP_FLAG_READ | (p->writable ? PROP_FLAG_WRITE : 0));
        esp_rmaker_param_t* rp = esp_rmaker_param_create(p->rm_name, p->rm_type, initv, props);
        if (!rp) {
            ESP_LOGW(TAG, "esp_rmaker_param_create failed: %s/%s", name, p->rm_name);
            continue;
        }
        if (p->rm_ui) esp_rmaker_param_add_ui_type(rp, p->rm_ui);
        if (p->min != 0 || p->max != 0 || p->step != 0) {
            esp_rmaker_param_val_t mn, mx, st;
            if (p->data_type == 'f') {
                mn = esp_rmaker_float((float)p->min);
                mx = esp_rmaker_float((float)p->max);
                st = esp_rmaker_float((float)p->step);
            } else {
                mn = esp_rmaker_int(p->min);
                mx = esp_rmaker_int(p->max);
                st = esp_rmaker_int(p->step);
            }
            esp_rmaker_param_add_bounds(rp, mn, mx, st);
        }
        esp_rmaker_device_add_param(dev, rp);
        if (p->primary) primary = rp;

        built[built_count].plan          = *p;
        built[built_count].handle        = rp;
        built[built_count].last_reported = 0;
        built[built_count].has_last      = false;
        built_count++;
    }
    if (primary) esp_rmaker_device_assign_primary_param(dev, primary);

    esp_rmaker_device_add_cb(dev, rmk_bridge_write_stub_cb, nullptr);

    const esp_rmaker_node_t* node = esp_rmaker_get_node();
    if (node) {
        esp_err_t aerr = esp_rmaker_node_add_device(node, dev);
        if (aerr != ESP_OK) {
            ESP_LOGE(TAG, "esp_rmaker_node_add_device failed for %s: %s",
                     name, esp_err_to_name(aerr));
            // Non-fatal: device + params exist and are recorded below so a
            // later unexpose can still clean them up; it just isn't part
            // of the node's reported config until that happens.
        }
    } else {
        ESP_LOGW(TAG, "esp_rmaker_get_node() returned NULL — %s created but not attached to a node", name);
    }

    reg_lock();
    d->device = dev;
    for (size_t i = 0; i < built_count; i++) d->params[i] = built[i];
    d->param_count = built_count;
    reg_unlock();

    ESP_LOGI(TAG, "exposed ieee=0x%016llx name=%s type=%s params=%u",
             (unsigned long long)ieee, name, rmk_devtype_str(t), (unsigned)built_count);
    return ESP_OK;
#else
    (void)ieee; (void)name; (void)ex; (void)n;
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t rmk_bridge_unexpose_device(uint64_t ieee) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(ieee);
    if (!d) { reg_unlock(); return ESP_ERR_NOT_FOUND; }
    esp_rmaker_device_t* dev = d->device;
    memset(d, 0, sizeof(*d));   // registry entry gone even if the SDK
                                // calls below fail — never leave a
                                // half-torn-down device reachable by ieee.
    reg_unlock();

    if (dev) {
        const esp_rmaker_node_t* node = esp_rmaker_get_node();
        // esp_rmaker_device_delete's own doc: remove from the node first.
        if (node) esp_rmaker_node_remove_device(node, dev);
        esp_rmaker_device_delete(dev);
    }

    ESP_LOGI(TAG, "unexposed ieee=0x%016llx", (unsigned long long)ieee);
    return ESP_OK;
#else
    (void)ieee;
    return ESP_ERR_INVALID_STATE;
#endif
}

void rmk_bridge_on_device_gone(uint64_t ieee) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    esp_err_t err = rmk_bridge_unexpose_device(ieee);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "on_device_gone(ieee=0x%016llx): cleanup returned %s",
                 (unsigned long long)ieee, esp_err_to_name(err));
    }
#else
    (void)ieee;
#endif
}

void rmk_bridge_attach(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (s_attached) {
        ESP_LOGW(TAG, "rmk_bridge_attach: already attached — ignoring");
        return;
    }
    s_h_attr = event_bus_subscribe(EventType::ZCL_ATTR,          rmk_bridge_on_shadow_event);
    s_h_opt  = event_bus_subscribe(EventType::SHADOW_OPTIMISTIC, rmk_bridge_on_shadow_event);
    if (s_h_attr == EVENT_SUB_INVALID || s_h_opt == EVENT_SUB_INVALID) {
        ESP_LOGE(TAG, "event_bus_subscribe failed (attr_ok=%d opt_ok=%d) — "
                      "bridge OUT direction will miss events of the failed type(s)",
                 s_h_attr != EVENT_SUB_INVALID, s_h_opt != EVENT_SUB_INVALID);
    }
    // event_bus_drain_handle(EVENT_SUB_INVALID, ...) returns 0 immediately
    // instead of blocking for the timeout (event_bus.cpp: guarded at the
    // top of the function) — if BOTH subscribes failed, the task loop
    // below would busy-spin at 100% CPU instead of idling. Only start the
    // task when at least one handle is real; a fully-failed attach (both
    // EVENT_SUB_INVALID — the subscriber table for both types completely
    // full, never observed in practice since this bridge is the first S3
    // subscriber to either type) leaves the OUT direction inactive, which
    // is already what the ERROR log above documents.
    if (s_h_attr != EVENT_SUB_INVALID || s_h_opt != EVENT_SUB_INVALID) {
        xTaskCreate(rmk_bridge_evt_task, "rmk_bridge_evt", 4096, nullptr, 2, nullptr);
    } else {
        ESP_LOGE(TAG, "both subscriptions failed — dispatch task NOT started, OUT direction fully inactive");
    }
    s_attached = true;
    ESP_LOGI(TAG, "attached to shadow event stream (ZCL_ATTR + SHADOW_OPTIMISTIC)");
#endif
}
