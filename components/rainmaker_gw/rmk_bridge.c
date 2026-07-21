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
//
// ── Task 17 addendum — bridge IN (write path) ────────────────────────────
// Replaces rmk_bridge_write_stub_cb (log-only, ack-and-forget) with a real
// cloud -> device write: look up the target param by its SDK handle, run
// the OUT path's kMap-driven conversion backwards (app value -> zhac raw),
// call the injected device_attr_set_core adapter (dependency injection —
// see rmk_bridge.h; this file must not #include main/'s headers), and on
// success ack the app's OWN value (no re-conversion round-trip — the
// existing anti-oscillation rule). On failure, SNAP-BACK: S3 keeps no
// device-shadow store to read a ground-truth value from, so "truth" here
// is this registry's own last_reported (the last value the OUT direction
// is known to have reported), converted through the same zhac_to_rm_val()
// the OUT handler itself uses — see rmk_bridge_write_cb below for the lock
// discipline (registry lock is never held across the injected writer's
// blocking, up to 3 s HAP roundtrip).
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
#include "esp_rmaker_work_queue.h"       // Task 21 fix: defer the OUT
                                          // report off task_hap — see
                                          // rmk_bridge_on_attr_update's own
                                          // comment for why.
#include <new>                            // std::nothrow

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

// Injected cloud->device writer (Task 17 — bridge IN direction). Set once,
// synchronously, during app_main()'s single-threaded boot (main.cpp calls
// rmk_bridge_set_attr_writer() right after rainmaker_gw_init(), long before
// the RainMaker work-queue task could deliver a real write — the bridge
// isn't even claimed/associated yet at that point), then only ever read —
// no lock needed for this pointer, the same "pre-init single-threaded use"
// reasoning s_reg_mtx itself relies on below.
static rmk_attr_write_fn s_attr_writer = nullptr;

// Task 18 DI: boot-time restore hook, set once during app_main() (same
// "pre-init single-threaded use" reasoning as s_attr_writer above), then
// only ever read from rmk_bridge_attach() — see rmk_bridge.h's own comment.
static rmk_boot_restore_fn s_boot_restore = nullptr;

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

// Lock held by caller. Finds the plan entry within a specific device (`d`,
// itself obtained by the caller as the write-cb's priv_data — see
// rmk_bridge_expose_device's esp_rmaker_device_create() call) whose SDK
// param handle is `param`. Returns nullptr if `d`'s slot was reclaimed
// (in_use false — a concurrent unexpose/on_device_gone raced us; `d` itself
// is always safe to dereference, it's a slot inside the static, process-
// lifetime s_reg[] array, never freed) or no param in it matches. Callers
// treat both the same as "not writable" — mirrors the brief's own
// `if (!p || !p->writable) return ESP_ERR_INVALID_ARG;`.
static RmkBridgeParam* plan_by_handle(RmkBridgeDev* d, const esp_rmaker_param_t* param) {
    if (!d->in_use) return nullptr;
    for (size_t i = 0; i < d->param_count; i++)
        if (d->params[i].handle == param) return &d->params[i];
    return nullptr;
}

// device(zhac raw, shadow-style ×100-fixed-point-for-float convention) ->
// app(esp_rmaker) conversion. Single source of truth for both directions:
// the OUT handler (rmk_bridge_on_attr_update) and the write-cb's SNAP-BACK
// path (Task 17 — "converted back app-side the same way the OUT path
// does") both call this exact function so the two can never drift apart.
static esp_rmaker_param_val_t zhac_to_rm_val(const rmk_param_t* plan, int32_t val) {
    switch (plan->conv) {
    case RMK_CONV_BRI:     return esp_rmaker_int(rmk_bri_to_rm(val));
    case RMK_CONV_CCT:     return esp_rmaker_int(rmk_mired_to_kelvin(val));
    case RMK_CONV_DIV100:  return esp_rmaker_float((float)rmk_div100(val));
    case RMK_CONV_CONTACT: return esp_rmaker_bool(rmk_contact_to_rm(val != 0));
    default:
        return (plan->data_type == 'b') ? esp_rmaker_bool(val != 0)
                                         : esp_rmaker_int(val);
    }
}

// app(esp_rmaker) -> device(zhac raw) conversion for the write-cb — the
// brief's own 3-way switch (RMK_CONV_BRI / RMK_CONV_CCT / default).
// RMK_CONV_DIV100 and RMK_CONV_CONTACT never reach here: both only appear
// on sensor-reading params (temperature/humidity/contact, rmk_typemap.c's
// kMap) that come back from a real device as read-only, so
// rmk_bridge_write_cb's `plan.writable` check below rejects a write to one
// before this is ever called.
static int32_t rm_to_zhac_val(const rmk_param_t* plan, const esp_rmaker_param_val_t* val) {
    switch (plan->conv) {
    case RMK_CONV_BRI: return rmk_bri_from_rm(val->val.i);
    case RMK_CONV_CCT: return rmk_kelvin_to_mired(val->val.i);
    default:
        return (plan->data_type == 'b') ? (val->val.b ? 1 : 0) : val->val.i;
    }
}

// Real cloud -> device write callback (Task 17, replaces the Task 16
// log-only stub). Registered once per exposed device via
// esp_rmaker_device_add_cb() in rmk_bridge_expose_device(); `priv_data` is
// the RmkBridgeDev* the SDK was handed back at esp_rmaker_device_create()
// time (esp_rmaker_core.h: "Pointer to the private data passed while
// creating the device") — NOT esp_rmaker_device_add_cb()'s third argument,
// which is actually the (unused, nullptr) read_cb.
//
// Runs on the RainMaker SDK's own work-queue task
// (CONFIG_ESP_RMAKER_WORK_QUEUE_TASK_*) — blocking here for the injected
// writer's up-to-3000 ms HAP roundtrip (brief §4) is acceptable: that task
// exists to serialize exactly this kind of app-facing side effect and has
// no other latency-sensitive job.
static esp_err_t rmk_bridge_write_cb(const esp_rmaker_device_t* device,
                                     const esp_rmaker_param_t* param,
                                     const esp_rmaker_param_val_t val,
                                     void* priv_data,
                                     esp_rmaker_write_ctx_t* ctx) {
    (void)device;
    (void)ctx;
    RmkBridgeDev* d = (RmkBridgeDev*)priv_data;
    if (!d) return ESP_ERR_INVALID_ARG;            // shouldn't happen — expose_device always passes `d`
    if (!s_attr_writer) return ESP_ERR_INVALID_STATE;  // DI setter never called (see rmk_bridge.h)

    // Copy out what we need under the lock, then release it BEFORE the
    // (possibly blocking, up to 3 s) writer call below — never hold
    // reg_lock() across an esp_rmaker_*/HAP roundtrip call (brief §4).
    reg_lock();
    RmkBridgeParam* bp = plan_by_handle(d, param);
    if (!bp || !bp->plan.writable) {
        reg_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t    ieee = d->ieee;
    rmk_param_t plan = bp->plan;   // struct copy — zhac_key etc. point at
                                    // rmk_typemap.c's static string-literal
                                    // table (RmkBridgeParam's own comment),
                                    // so the copy never dangles even if this
                                    // slot is torn down while we're unlocked.
    reg_unlock();

    int32_t zv = rm_to_zhac_val(&plan, &val);

    // ep/cluster/attr: mirror api_device_attr_set's own SPA-omitted
    // defaults (main/api_devices.cpp: doc["ep"]|0, doc["cluster"]|0,
    // doc["attr"]|0) — 0/0/0 tells the P4 adapter to resolve the target by
    // `key` alone (device_attr_set_core's own header comment: "key-based
    // write; P4 adapter resolves by key"). The typemap's zhac_key strings
    // are static literals <=11 chars (rmk_typemap.c's kMap), well under
    // HapSetAttrReq::key's <=23-char precondition that device_attr_set_core
    // does NOT re-validate.
    bool cmd_ok = false;
    esp_err_t err = s_attr_writer(ieee, plan.zhac_key, zv,
                                  /*ep=*/0, /*cluster=*/0, /*attr=*/0,
                                  &cmd_ok);

    if (err == ESP_OK && cmd_ok) {
        // Anti-oscillation: ack the app's OWN value, never a re-converted
        // round-trip (Task 16/13 no-echo rule, brief line 35).
        esp_rmaker_param_update_and_report((esp_rmaker_param_t*)param, val);

        reg_lock();
        // Re-validate: up to 3 s elapsed since the copy above — don't write
        // through a stale pointer if this device/param was torn down
        // meanwhile (same TOCTOU discipline as rmk_bridge_expose_device's
        // own "reservation still ours" re-check).
        RmkBridgeParam* bp2 = plan_by_handle(d, param);
        if (bp2 && d->ieee == ieee) {
            bp2->last_reported = zv;
            bp2->has_last      = true;
        }
        reg_unlock();
        return ESP_OK;
    }

    // SNAP-BACK: S3 keeps no device-shadow store, so "current truth" is
    // this registry's last_reported — the last value the OUT direction is
    // known to have reported for this param — converted the SAME way the
    // OUT path does (zhac_to_rm_val, shared with rmk_bridge_on_attr_update).
    // If no prior report exists yet (has_last==false: e.g. the very first
    // write to a freshly-exposed param before any OUT-direction report has
    // landed), report nothing — the app keeps showing its own optimistic
    // value until a real report arrives. Acceptable per brief §3.
    reg_lock();
    RmkBridgeParam* bp3 = plan_by_handle(d, param);
    bool has_snap = bp3 && bp3->has_last;
    int32_t snap_raw = has_snap ? bp3->last_reported : 0;
    reg_unlock();

    if (has_snap) {
        esp_rmaker_param_val_t snap_v = zhac_to_rm_val(&plan, snap_raw);
        esp_rmaker_param_update_and_report((esp_rmaker_param_t*)param, snap_v);
    }

    ESP_LOGW(TAG, "write-cb ieee=0x%016llx key=%s FAILED (err=%s cmd_ok=%d)%s",
             (unsigned long long)ieee, plan.zhac_key, esp_err_to_name(err),
             (int)cmd_ok, has_snap ? " -- snapped back" : " -- no prior value, left as-is");
    return (err != ESP_OK) ? err : ESP_FAIL;
}

#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

// Task 17 DI setter — see rmk_bridge.h for the full rationale. Defined
// outside the top #if so main.cpp can call it unconditionally at boot.
void rmk_bridge_set_attr_writer(rmk_attr_write_fn fn) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    s_attr_writer = fn;
#else
    (void)fn;
#endif
}

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

    // priv_data = this registry slot: the SDK hands it back verbatim to
    // rmk_bridge_write_cb on every write for this device (esp_rmaker_core.h
    // esp_rmaker_device_create() doc: "Pointer to the private data passed
    // while creating the device") — NOT esp_rmaker_device_add_cb()'s third
    // argument below, which is the (unused) read_cb. `d` is a slot inside
    // the static, process-lifetime s_reg[] array, so this pointer stays
    // valid to dereference even after a concurrent unexpose zeroes the
    // slot's *contents* — see rmk_bridge_write_cb's own TOCTOU re-checks.
    esp_rmaker_device_t* dev = esp_rmaker_device_create(name, rmk_devtype_str(t), d);
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

    // Task 17: real write-cb (was Task 16's log-only stub). Third arg is
    // read_cb — always nullptr, the SDK never invokes one today (see
    // esp_rmaker_device_read_cb_t's own doc in esp_rmaker_core.h).
    esp_rmaker_device_add_cb(dev, rmk_bridge_write_cb, nullptr);

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
        // Safe to delete INLINE here — unlike rmk_bridge_unexpose_device's
        // own teardown below (Task 21 review fix; see that function's
        // comment for the use-after-free this distinction matters for).
        // This branch only runs when the reservation was reclaimed BEFORE
        // the commit a few lines down (`d->param_count = built_count`)
        // ever ran, so this ieee's registry param_count was 0 for this
        // whole call — rmk_bridge_on_attr_update's scan loop (bounded by
        // d->param_count) could never have found `dev`'s params to enqueue
        // a report against. No pending work-queue item can reference what
        // gets freed here, so there is nothing to order against.
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

// Deferred SDK teardown (Task 21 review fix — see rmk_bridge_unexpose_
// device's own comment for the full use-after-free rationale this closes).
// `priv` is the esp_rmaker_device_t* to detach and delete.
// esp_rmaker_node_remove_device()/esp_rmaker_device_delete() were verified
// from the vendored SDK source (esp_rmaker_node.c / esp_rmaker_device.c),
// not assumed: both are pure linked-list unlink + free() work, no internal
// locking, no work-queue calls of their own — safe to run from the work-
// queue task itself (cannot deadlock waiting on the task it's already
// running on), and since that queue's single consumer processes one item
// at a time, this can never execute concurrently with rmk_report_work_fn
// either — the two are strictly serialized by construction.
// Guarded exactly like the RmkReportWork block further down (and for the
// same reason, learned the hard way earlier in this same task): every
// esp_rmaker_* type is only ever declared inside CONFIG_ZHAC_RAINMAKER_
// ENABLE, and this function is referenced only from rmk_bridge_unexpose_
// device's own (already-guarded) body below.
#if CONFIG_ZHAC_RAINMAKER_ENABLE
static void rmk_teardown_work_fn(void* priv) {
    esp_rmaker_device_t* dev = static_cast<esp_rmaker_device_t*>(priv);
    const esp_rmaker_node_t* node = esp_rmaker_get_node();
    if (node) esp_rmaker_node_remove_device(node, dev);
    esp_rmaker_device_delete(dev);
}
#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

esp_err_t rmk_bridge_unexpose_device(uint64_t ieee) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(ieee);
    if (!d) { reg_unlock(); return ESP_ERR_NOT_FOUND; }
    esp_rmaker_device_t* dev = d->device;
    memset(d, 0, sizeof(*d));   // registry entry gone even if the SDK
                                // calls below fail — never leave a
                                // half-torn-down device reachable by ieee.
                                // Clearing this FIRST, under the lock, is
                                // also what makes the deferred teardown
                                // below race-free: rmk_bridge_on_attr_
                                // update's own dev_by_ieee_locked lookup
                                // will miss this ieee from this point on,
                                // so no NEW report can be enqueued
                                // referencing dev's params after this line.
    if (s_reg_count > 0) s_reg_count--;
    reg_unlock();

    if (dev) {
        // Task 21 review fix — CRITICAL use-after-free, closed by MUTUAL
        // EXCLUSION on reg_lock (both here and in rmk_bridge_on_attr_
        // update) PLUS the work queue's FIFO ordering. Read rmk_bridge_
        // on_attr_update's own comment for the full two-part argument —
        // this comment covers this function's half of it.
        //
        // This used to call esp_rmaker_node_remove_device + esp_rmaker_
        // device_delete inline, right here. But rmk_bridge_on_attr_update
        // can enqueue a RmkReportWork referencing one of THIS device's
        // param handles onto the shared work queue, and that queued item
        // might not run until well after this function returns —
        // deleting the params inline could free them out from under an
        // already-queued report (esp_rmaker_device_delete walks every
        // param and free()s it — esp_rmaker_param.c's esp_rmaker_param_
        // delete). Unlike the existing, accepted rmk_bridge_write_cb UAF
        // window (bounded ~3000 ms, documented at that function), this
        // window would be UNBOUNDED — however long the shared 8-slot
        // queue takes to drain, each item up to ~10 s.
        //
        // An EARLIER cut of this fix deferred the teardown (as this one
        // still does) but claimed FIFO ordering alone closed the race —
        // wrong: rmk_bridge_on_attr_update also had a gap, between
        // reading a param's handle and enqueueing its report, where it
        // held no lock at all. A concurrent unexpose call from a
        // DIFFERENT TASK (this function is reachable not only from
        // task_hap's DEVICE_LEAVE path, which can never race itself, but
        // also from main/api_rainmaker.cpp's api_device_rainmaker_remove
        // on the WS/REST task — a real cross-task race on this dual-core
        // part) could land its teardown enqueue in that gap, ahead of the
        // report. The actual fix lives in rmk_bridge_on_attr_update: it
        // now holds reg_lock across its ENTIRE scan-through-enqueue
        // sequence, so the registry clear below (also under reg_lock)
        // and that function's check-and-enqueue can never interleave —
        // exactly one of the two runs first, in full:
        //   - If the clear here runs first: rmk_bridge_on_attr_update's
        //     own lookup then fails and no report is ever created.
        //   - If rmk_bridge_on_attr_update's locked section runs first:
        //     its report is fully enqueued before this function can even
        //     acquire the lock to clear the registry, let alone reach the
        //     teardown enqueue below — so the report is unconditionally
        //     ahead of the teardown in the FIFO queue.
        // FIFO ordering is still essential (it is what carries the
        // mutual-exclusion decision through to the two queue items'
        // actual run order) but is not sufficient BY ITSELF — mutual
        // exclusion on reg_lock is what decides which one gets enqueued
        // first, which is the part the earlier cut of this comment got
        // wrong. The teardown enqueue below itself does not need to be
        // under reg_lock for this argument to hold: by the time it runs,
        // the registry clear above (what a racing rmk_bridge_on_attr_
        // update actually contends with) has already unconditionally
        // completed.
        esp_err_t qerr = esp_rmaker_work_queue_add_task(rmk_teardown_work_fn, dev);
        if (qerr != ESP_OK) {
            // Queue full (8 slots): do NOT fall back to an inline delete
            // — that would recreate exactly the race this fix closes.
            // Each occurrence leaks this one device's SDK objects rather
            // than risk a use-after-free; logged at ERROR so it is
            // visible rather than silently accepted. This bound (<=
            // RMK_MAX_DEVS=10 devices' worth of esp_rmaker_device_t/
            // param objects) is PER OCCURRENCE, bounded by the live
            // device count at the moment it happens — NOT a cumulative
            // cap over the firmware's uptime. If this condition recurs
            // across multiple unexpose calls over a long-running
            // session, each occurrence adds to the total leaked, with no
            // lifetime ceiling. Expected to be rare (8 queue slots, one
            // fast consumer) but not impossible under sustained network
            // degradation backing up several slow (~10 s) report items
            // ahead of a teardown request.
            ESP_LOGE(TAG, "unexpose ieee=0x%016llx: work-queue full — leaking SDK "
                          "device/param objects to avoid a UAF race with a pending report",
                     (unsigned long long)ieee);
        }
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

// Deferred-report payload for the work-queue hop in rmk_bridge_on_attr_
// update below (Task 21 fix). Small, self-contained, freed by the work
// function itself once it runs. Guarded like every other post-registry
// declaration in this file (the shared #if block above closed before
// rmk_bridge_set_attr_writer) — esp_rmaker_param_t/esp_rmaker_param_val_t
// are only ever declared inside CONFIG_ZHAC_RAINMAKER_ENABLE, and both this
// struct and rmk_report_work_fn are referenced only from
// rmk_bridge_on_attr_update's own (already-guarded) body below.
#if CONFIG_ZHAC_RAINMAKER_ENABLE
struct RmkReportWork {
    esp_rmaker_param_t*    handle;
    esp_rmaker_param_val_t val;
};

static void rmk_report_work_fn(void* priv) {
    RmkReportWork* w = static_cast<RmkReportWork*>(priv);
    esp_rmaker_param_update_and_report(w->handle, w->val);
    delete w;
}
#endif  // CONFIG_ZHAC_RAINMAKER_ENABLE

// ── OUT handler: attr update -> converted, throttled param report ───────
// Called directly (no event envelope, no queue, no dispatch task) from
// main/hap_bridge.cpp's BULK_STATE_UPDATE handler — see this file's top
// banner for why the event_bus path this used to run on never fired on
// this target. The registry scan/throttle/conversion below stays
// allocation-free: fixed-size scan, plain integer/float math, no heap/
// string work — see the Task 21 fix note near the bottom of this function
// for the one part of the old behavior that changed.
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

    // Task 21 review fix (2nd round) — the registry scan, the conversion,
    // the work-queue enqueue, AND the throttle bookkeeping commit ALL run
    // inside this ONE critical section now (reg_lock held top to bottom).
    // See the comment below the scan loop for why the previous cut of
    // this function — which scanned under reg_lock but enqueued AFTER
    // releasing it — left a real, adversarially-reachable use-after-free
    // open against a concurrent rmk_bridge_unexpose_device running on a
    // different task.
    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(ieee);
    if (!d) { reg_unlock(); return; }   // not exposed — registry IS the filter

    RmkBridgeParam* target = nullptr;
    esp_rmaker_param_val_t v{};

    for (size_t i = 0; i < d->param_count; i++) {
        RmkBridgeParam* bp = &d->params[i];
        if (strcmp(bp->plan.zhac_key, key) != 0) continue;
        if (!bp->handle) break;   // this param's create() failed earlier

        // Skip-unchanged throttle on the RAW shadow value, checked before
        // paying for the conversion below.
        if (bp->has_last && bp->last_reported == val) break;

        // Task 17 factored this conversion out to zhac_to_rm_val() so the
        // write-cb's SNAP-BACK path (same conversion, opposite direction's
        // report) shares the exact logic instead of a hand-copied
        // duplicate that could silently drift from this one.
        v = zhac_to_rm_val(&bp->plan, val);
        target = bp;
        break;   // zhac_key is unique within one device's param plan
    }

    // Task 21 review fix (2nd round) — CRITICAL: this is what actually
    // closes the use-after-free (read rmk_bridge_unexpose_device's own
    // comment too — the two together are one argument).
    //
    // The FIRST cut of this fix scanned/converted under reg_lock,
    // correctly identified the target param, then called reg_unlock()
    // BEFORE constructing and enqueueing the RmkReportWork. That gap was
    // real and adversarially reachable: rmk_bridge_unexpose_device is
    // reachable not only from task_hap (the DEVICE_LEAVE path, which can
    // never race ITSELF — no task runs concurrently with its own thread
    // of execution) but also from main/api_rainmaker.cpp's api_device_
    // rainmaker_remove, which runs on the WS/REST task — a genuinely
    // different, concurrently-schedulable task on this dual-core (SMP)
    // part. If that concurrent unexpose ran its whole reg_lock -> memset
    // -> reg_unlock -> work_queue_add_task(teardown) sequence inside the
    // gap between this function's unlock and its (until now, separately
    // unlocked) enqueue, and its teardown enqueue happened to land in the
    // shared FIFO before this function's report enqueue, the queue would
    // drain teardown-then-report: the consumer frees the params, then the
    // report dereferences freed memory. FIFO ordering alone — the ONLY
    // guarantee the first cut relied on — cannot prevent that: FIFO only
    // orders items that are ALREADY enqueued, it says nothing about which
    // of two RACING enqueue calls, from two different tasks, lands first.
    //
    // Fix: hold reg_lock across the enqueue too. esp_rmaker_work_queue_
    // add_task is a 0-tick, non-blocking xQueueSend (verified — see
    // rmk_bridge_unexpose_device's own comment), so this costs nothing
    // and cannot stall anything; the one call that actually blocks
    // (esp_mqtt_client_publish, inside rmk_report_work_fn) still runs
    // entirely outside any lock, on the work-queue task — untouched by
    // this change, and still the whole point of the deferral.
    //
    // This turns the guarantee from "FIFO ordering alone" into "mutual
    // exclusion decides who goes first, then FIFO ordering carries that
    // decision through": reg_lock is the ONLY place either side (this
    // function's scan-through-enqueue, or rmk_bridge_unexpose_device's
    // registry clear) touches the registry, so exactly one of the two
    // critical sections runs first, in full, with no interleaving —
    //   - If a concurrent unexpose's clear-under-lock runs first: this
    //     function's own dev_by_ieee_locked lookup above then fails,
    //     `target` stays null, and no report is ever created — nothing
    //     to race the teardown with, regardless of when that teardown's
    //     own (separately unlocked) enqueue call actually happens.
    //   - If this function's whole locked section runs first: the report
    //     is fully constructed AND enqueued before a concurrent unexpose
    //     can even acquire the lock to begin clearing the registry, let
    //     alone reach its own (later, unlocked) teardown enqueue — so the
    //     report is unconditionally ahead of the teardown in the FIFO
    //     queue.
    // Either way, a teardown can never be enqueued ahead of a report that
    // references the device it tears down.
    //
    // This also removes the previous cut's post-enqueue re-lock/re-
    // validate dance for the throttle bookkeeping, and the "duplicate
    // report" trade-off that came with it: with the lock never dropped
    // between finding `target` and committing to it, there is no TOCTOU
    // window left to re-validate against, and no concurrent update to
    // the same param can interleave either (task_hap only ever calls
    // this function from its own single-threaded frame dispatch, so it
    // cannot race a second call to itself).
    if (target) {
        RmkReportWork* w = new (std::nothrow) RmkReportWork{target->handle, v};
        if (!w) {
            ESP_LOGW(TAG, "OOM deferring RainMaker report for key=%s — dropped", key);
        } else {
            esp_err_t qerr = esp_rmaker_work_queue_add_task(rmk_report_work_fn, w);
            if (qerr == ESP_OK) {
                target->last_reported = val;
                target->has_last      = true;
            } else {
                ESP_LOGW(TAG, "work-queue full, dropping RainMaker report for key=%s (%s)",
                         key, esp_err_to_name(qerr));
                delete w;
            }
        }
    }
    reg_unlock();
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

    // Task 18: fire the boot-time persisted-device restore exactly once, on
    // the FIRST READY transition reached this boot — the s_attached guard
    // above (this function's own pre-existing idempotency) is what makes
    // "exactly once" hold across rainmaker_gw.c's two RMK_ST_READY call
    // sites (initial mapping-done vs. a later reconnect). NULL-checked so a
    // flag-on build that never wired the setter degrades to "no persisted
    // devices come back" (logged on the main/ side) instead of crashing.
    if (s_boot_restore) s_boot_restore();
#endif
}

// Task 18 DI setter — defined outside the top #if (same pattern as
// rmk_bridge_set_attr_writer) so main.cpp can call it unconditionally.
void rmk_bridge_set_boot_restore(rmk_boot_restore_fn fn) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    s_boot_restore = fn;
#else
    (void)fn;
#endif
}

size_t rmk_bridge_device_count(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    // Relaxed read — same "at most one event stale" tolerance already
    // documented for s_reg_count / rmk_bridge_active() above. rainmaker.status
    // is a point-in-time status read, not a correctness-critical decision.
    return s_reg_count;
#else
    return 0;
#endif
}

size_t rmk_bridge_list(rmk_bridge_dev_info_t* out, size_t cap) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (!out || cap == 0) return 0;

    uint64_t              ieees[RMK_MAX_DEVS];
    esp_rmaker_device_t*  devs[RMK_MAX_DEVS];
    size_t                n = 0;

    reg_lock();
    for (size_t i = 0; i < RMK_MAX_DEVS && n < cap; i++) {
        if (!s_reg[i].in_use) continue;
        ieees[n] = s_reg[i].ieee;
        devs[n]  = s_reg[i].device;
        n++;
    }
    reg_unlock();

    // esp_rmaker_device_get_type/_get_name are simple accessors, but this
    // file's own convention (write-cb, rmk_bridge_on_attr_update) is to
    // never call an esp_rmaker_* function while holding reg_lock() — kept
    // here too rather than assuming today's trivial implementation stays
    // that way forever.
    for (size_t i = 0; i < n; i++) {
        out[i].ieee = ieees[i];
        out[i].type = devs[i] ? esp_rmaker_device_get_type(devs[i]) : "esp.device.other";
        out[i].name = devs[i] ? esp_rmaker_device_get_name(devs[i]) : "";
    }
    return n;
#else
    (void)out; (void)cap;
    return 0;
#endif
}

void rmk_bridge_report_node_details_now(void) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (rainmaker_gw_state() != RMK_ST_READY) return;   // nothing to report yet/anymore
    esp_err_t err = esp_rmaker_report_node_details();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report_node_details failed: %s", esp_err_to_name(err));
    }
#endif
}

void rmk_bridge_on_device_renamed(uint64_t ieee, const char* new_name) {
#if CONFIG_ZHAC_RAINMAKER_ENABLE
    if (!new_name) return;

    reg_lock();
    RmkBridgeDev* d = dev_by_ieee_locked(ieee);
    esp_rmaker_device_t* dev = d ? d->device : nullptr;
    reg_unlock();
    if (!dev) return;   // not exposed, or expose_device hasn't attached the SDK device yet

    // Same "never call esp_rmaker_* while holding reg_lock()" discipline as
    // the rest of this file — dev is a pointer into the SDK's own live
    // object, safe to use unlocked here (worst case: a concurrent unexpose
    // deletes it a moment later, and this update either lands just before
    // that or is a harmless no-op against an object about to be torn down —
    // the SDK does not document param updates against a mid-delete device as
    // unsafe, and this path is not on any latency- or correctness-critical
    // trigger).
    esp_rmaker_param_t* name_param =
        esp_rmaker_device_get_param_by_name(dev, ESP_RMAKER_DEF_NAME_PARAM);
    if (!name_param) return;
    esp_rmaker_param_update_and_report(name_param, esp_rmaker_str(new_name));
#else
    (void)ieee; (void)new_name;
#endif
}
