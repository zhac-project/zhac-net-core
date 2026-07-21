// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// rmk_bridge — shadow -> RainMaker param bridge (Task 16 of the RainMaker
// Bridge plan; bridge OUT direction). Fills the Task-11/13 stub.
//
// Compiled as C++ despite the .c name/extension — same category of reason
// as rainmaker_gw.c (see this component's CMakeLists.txt for the full
// rationale on that file): zcl_attribute.h (zap_common — needed here for
// the ValType enum: VAL_INT/VAL_BOOL/VAL_STR/VAL_FLOAT/VAL_NONE) is
// C++-only syntax (`#include <cstdint>` etc.), and this file's own code
// (bare `RmkBridgeDev`/`RmkBridgeParam` struct-as-type-name without a
// typedef, `nullptr`) is C++-only regardless. rmk_bridge.h's own
// `extern "C"` guard keeps this file's PUBLIC API at plain C linkage
// regardless, so nothing about the component's external contract changes.
//
// ── REVISION — post-review rehook (fix commit) ──────────────────────────
// The original cut of this file (Task 16 as first committed, `0300281`)
// subscribed to event_bus for EventType::ZCL_ATTR / SHADOW_OPTIMISTIC,
// following the brief's corrected-per-environment-facts mental model:
// "the S3-side device_shadow is updated by the HAP event stream from the
// P4... study how mqtt_gw_s3.cpp / remote_client learn about shadow
// updates and mirror that." That investigation *did* correctly establish
// that neither mqtt_gw_s3.cpp nor remote_client subscribes to event_bus
// (grep-verified across both repos — see the superseded banner in git
// history), and that device_shadow.cpp's publish_staged() is a real,
// correctly-implemented ZCL_ATTR/SHADOW_OPTIMISTIC producer... but never
// verified that anything on THIS target actually calls INTO device_shadow
// in the first place. It doesn't: grep across this entire net-core
// worktree for device_shadow_init / device_shadow_process /
// device_shadow_update_optimistic turns up zero call sites anywhere
// outside this file's own (now-corrected) comments. device_shadow is
// listed in main/idf_component.yml (so it links) but is never initialized
// or fed on S3 — a linked-but-dead dependency on this target. The
// event_bus subscription compiled and would have sat there forever,
// correctly waiting on a queue nothing ever posts to.
//
// The REAL S3-side fan-out for device state, read directly from
// main/hap_bridge.cpp's `case HapMsgType::BULK_STATE_UPDATE:` (~line 616):
// it deserializes the P4's JSON payload, extracts a top-level "ieee"
// field, and calls ws_event_broadcast("attr.bulk", ...) +
// mqtt_gw_publish("devices/<ieee>/state", ...) directly on the raw
// payload bytes — no device_shadow, no event_bus, anywhere in that path.
// The payload shape (per that handler's own comment, and confirmed
// against hap_json.cpp's hap_json_encode_device_attr_update() — the
// encoder whose output shape matches what the S3 handler actually reads,
// as opposed to the differently-shaped hap_json_encode_bulk() whose
// doc-comment claims the same 0x60 opcode but produces a top-level "devs"
// array the S3 handler never looks at) is:
//   {"type":"device_update","ieee":"0xXXXX...","attrs":{"<key>":<val>},
//    "lqi":<uint8>,"last_seen":<unix_ts>}
// where each `attrs` value is *already* JSON-natively-typed — a JSON bool
// for VAL_BOOL, an already-real (unscaled) JSON number for VAL_FLOAT
// (encode_device_attr_update: `attrs[key] = (float)int_val / 100.0f`), and
// a plain JSON integer for VAL_INT. No `val_type` tag travels on this
// wire at all — main/hap_bridge.cpp's new hook (this fix) has to infer it
// from the JSON variant's own type, and reconstructs this file's expected
// "raw shadow ×100" VAL_FLOAT convention before calling
// rmk_bridge_on_attr_update(). See that call site's own comment for a
// verified (not guessed — traced through the actual vendored
// components/arduinojson v7.4.3 parser/serializer source) residual
// ambiguity: a float-valued attribute that happens to be an exact whole
// number re-serializes without a decimal point and is indistinguishable,
// on the wire, from a genuine integer.
//
// This file no longer subscribes to anything. rmk_bridge_on_attr_update()
// is called directly, synchronously, from whatever task processes the
// BULK_STATE_UPDATE frame (task_hap) — no queue, no second task, so the
// original event_bus-drain-task busy-spin self-review finding and the
// "who drains the queue" investigation are both moot (the queue is gone).
// The registry mutex stays: expose/unexpose (Task 18, unknown thread) can
// still race this direct-call read path exactly as before.
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
#include "zcl_attribute.h"  // ValType (VAL_STR/VAL_NONE guard below).
                             // Reachable via zap_store's own public
                             // REQUIRES on zap_common (already in this
                             // component's CMakeLists.txt) — no new
                             // component dependency needed for this alone.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_params.h"  // ESP_RMAKER_DEF_NAME_PARAM,
                                          // esp_rmaker_name_param_create()

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
static size_t          s_reg_count = 0;   // count of in_use, fully-committed
                                           // entries — rmk_bridge_active()'s
                                           // cheap gate. Read without the
                                           // lock there (a benign, at-most-
                                           // one-event-stale relaxed read on
                                           // a hot per-frame fast path);
                                           // written only under reg_lock().
static bool             s_attached = false;

// Guards s_reg[] against a concurrent Task-18 caller (expose/unexpose,
// presumably driven off DEVICE_JOIN/DEVICE_LEAVE, possibly on a different
// task than task_hap, which is what now drives rmk_bridge_on_attr_update
// directly) racing the OUT handler's read side. Lazy-created, null-guarded
// before first use — same accepted pattern event_bus.cpp itself uses for
// s_bus_mtx ("pre-init single-threaded use"): the only theoretical race is
// two callers both hitting a genuinely first call to this file at once,
// which in practice means Task 18 racing itself before it has even run
// once. Held only across registry scan/mutation; released before any
// esp_rmaker_* SDK call (some may block/log).
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
        // Only release the reservation if it's still ours (same defensive
        // check as the TOCTOU close below — cheap, and correct either way:
        // if it was already reclaimed there is nothing of ours left to
        // release).
        if (d->in_use && d->ieee == ieee && !d->device) memset(d, 0, sizeof(*d));
        reg_unlock();
        return ESP_FAIL;
    }

    // Standard Name param (RainMaker's own examples: "should be added to
    // all devices for which you want a user customisable name" —
    // enables in-app rename). Not tracked in d->params[]/built[] below:
    // it has no zhac_key / shadow mapping, so it isn't something the OUT
    // handler ever updates — it's a device-identity param, set once here.
    esp_rmaker_device_add_param(dev, esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, name));

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

    bool added_to_node = false;
    const esp_rmaker_node_t* node = esp_rmaker_get_node();
    if (node) {
        esp_err_t aerr = esp_rmaker_node_add_device(node, dev);
        if (aerr == ESP_OK) {
            added_to_node = true;
        } else {
            ESP_LOGE(TAG, "esp_rmaker_node_add_device failed for %s: %s",
                     name, esp_err_to_name(aerr));
            // Non-fatal: device + params exist and are recorded below so a
            // later unexpose can still clean them up; it just isn't part
            // of the node's reported config until that happens.
        }
    } else {
        ESP_LOGW(TAG, "esp_rmaker_get_node() returned NULL — %s created but not attached to a node", name);
    }

    // Code-review fix: re-assert the reservation is still ours before
    // committing. Between the reservation above and here, a concurrent
    // rmk_bridge_unexpose_device(ieee)/rmk_bridge_on_device_gone(ieee) for
    // this SAME ieee (racing on a different task — Task 18's threading
    // model is unknown) could have torn the slot down and even let it be
    // reserved again by a third, unrelated expose call. Blindly writing
    // through `d` in that case would resurrect a torn-down entry and/or
    // corrupt an unrelated device's in-flight registration. Detect it and
    // discard what was built instead of leaking the SDK objects.
    reg_lock();
    if (!d->in_use || d->ieee != ieee) {
        reg_unlock();
        ESP_LOGW(TAG, "expose_device ieee=0x%016llx: registry slot reclaimed during setup — discarding built device",
                 (unsigned long long)ieee);
        if (added_to_node) {
            const esp_rmaker_node_t* node2 = esp_rmaker_get_node();
            if (node2) esp_rmaker_node_remove_device(node2, dev);
        }
        esp_rmaker_device_delete(dev);   // takes the Name param + every
                                         // esp_rmaker_device_add_param'd
                                         // param down with it — no separate
                                         // per-param delete needed/offered
                                         // by the SDK for this case.
        return ESP_ERR_INVALID_STATE;
    }
    d->device = dev;
    for (size_t i = 0; i < built_count; i++) d->params[i] = built[i];
    d->param_count = built_count;
    s_reg_count++;
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
    if (s_reg_count > 0) s_reg_count--;
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

bool rmk_bridge_active(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    return rainmaker_gw_state() == RMK_ST_READY && s_reg_count > 0;
#else
    return false;
#endif
}

// ── OUT handler: attr update -> converted, throttled param report ───────
// Called directly (no event envelope, no queue, no dispatch task) from
// main/hap_bridge.cpp's BULK_STATE_UPDATE handler — see this file's top
// banner for why the event_bus path this used to run on never fired on
// this target. Allocation-free: fixed-size registry scan, plain
// integer/float math in rmk_typemap's conversions, no heap/string work.
void rmk_bridge_on_attr_update(uint64_t ieee, const char* key,
                               uint8_t val_type, int32_t val) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    // Lifecycle guard (brief's original requirement, still applies).
    if (rainmaker_gw_state() != RMK_ST_READY) return;
    if (!key) return;
    // VAL_STR aliases int_val with str_val in the wire's ZclAttrEvent
    // convention this function's `val` parameter follows — a string value
    // has no sensible int32_t here. VAL_NONE carries nothing meaningful
    // either. Every rmk_param_t data_type is 'b'/'i'/'f' (rmk_typemap.h) —
    // no exposed param is ever string-typed — so both are unconditional
    // skips, not just "unhandled conv".
    if (val_type == VAL_STR || val_type == VAL_NONE) return;

    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(ieee);
    if (!d) { reg_unlock(); return; }   // not exposed — registry IS the filter

    esp_rmaker_param_t* handle = nullptr;
    esp_rmaker_param_val_t v{};
    bool changed = false;

    for (size_t i = 0; i < d->param_count; i++) {
        RmkBridgeParam* bp = &d->params[i];
        if (strcmp(bp->plan.zhac_key, key) != 0) continue;
        if (!bp->handle) break;   // this param's create() failed earlier

        // Skip-unchanged throttle on the RAW shadow value, checked before
        // paying for the conversion below.
        if (bp->has_last && bp->last_reported == val) break;
        bp->last_reported = val;
        bp->has_last = true;

        switch (bp->plan.conv) {
        case RMK_CONV_BRI:     v = esp_rmaker_int(rmk_bri_to_rm(val)); break;
        case RMK_CONV_CCT:     v = esp_rmaker_int(rmk_mired_to_kelvin(val)); break;
        case RMK_CONV_DIV100:  v = esp_rmaker_float((float)rmk_div100(val)); break;
        case RMK_CONV_CONTACT: v = esp_rmaker_bool(rmk_contact_to_rm(val != 0)); break;
        default:
            v = (bp->plan.data_type == 'b') ? esp_rmaker_bool(val != 0)
                                             : esp_rmaker_int(val);
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
#else
    (void)ieee; (void)key; (void)val_type; (void)val;
#endif
}

void rmk_bridge_attach(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    // Historically subscribed to event_bus here (superseded — see file
    // banner). Kept as a thin idempotent marker so rainmaker_gw.c's two
    // existing RMK_ST_READY call sites don't need editing as part of this
    // fix; the OUT direction is live purely by virtue of
    // rmk_bridge_active()/rmk_bridge_on_attr_update() being safe to call
    // unconditionally the moment a device is exposed and the state machine
    // reaches READY — no setup step is actually required any more.
    if (s_attached) return;
    s_attached = true;
    ESP_LOGI(TAG, "bridge OUT direction live (direct-call, main/hap_bridge.cpp BULK_STATE_UPDATE hook)");
#endif
}
