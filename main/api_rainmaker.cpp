// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RainMaker Bridge Phase 4 finale (Task 18): API ops + dynamic device
// exposure + uplink orchestration. Builds on Tasks 16/17 (bridge OUT/IN,
// components/rainmaker_gw) — this file is the user-drivable surface on top
// of that: `uplink.get/set`, `rainmaker.status`, `rainmaker.assoc.set`,
// `device.rainmaker.list/add/remove`, plus the boot-time restore of a
// persisted exposed-device set and a device.rename mirror hook (the latter
// wired from main/api_devices.cpp, not here).
//
// ── Layering: no esp_rmaker_* type/call in this file ─────────────────────
// components/rainmaker_gw's CMakeLists.txt exposes esp_rainmaker to itself
// ONLY (`idf_component_optional_requires(PRIVATE espressif__esp_rainmaker)`)
// — rmk_bridge.h's own file banner documents the invariant this file must
// not break: "no esp_rmaker_* type ever appears in rainmaker_gw.h". Every
// call this file makes into the bridge goes through rmk_bridge.h's plain
// uint64_t/const char*/bool/esp_err_t/rmk_expose_t surface. This includes
// the debounced node-config republish: esp_rmaker_report_node_details()
// itself is called from rmk_bridge_report_node_details_now() (component
// side); the 2 s debounce TIMER lives here instead, purely because this
// file already depends on esp_timer for unrelated reasons and the
// component's dependency footprint should not grow just to host a timer.
//
// ── Exposes-JSON shape assumption (READ BEFORE TOUCHING) ─────────────────
// GET_DEVICE_BY_ID's response is produced by zhac-main-core (P4) — a
// different repo, not present in this worktree, so the exact field names
// below could not be confirmed against source. This mirrors zigbee2mqtt's
// own expose-object convention (top-level CLAUDE.md: ZHC device
// definitions are generated from z2m), the strongest available signal:
//   - top-level "name"          : current friendly name (the SAME field
//                                  device.rename writes — api_devices.cpp)
//   - top-level "exposes"       : array of expose objects
//   - each expose's "property"  : shadow/state key (rmk_expose_t.name's
//                                  vocabulary — "state","brightness",...);
//                                  falls back to "name" if "property" is
//                                  absent
//   - each expose's "access"    : z2m bitmask (1=STATE,2=SET,4=GET) —
//                                  writable := (access & 2) != 0; falls
//                                  back to a plain boolean "writable" key
//                                  if "access" is absent
// Defaults to writable=false (fail closed) when neither is present. FLAG
// FOR HW-GATE REVIEW: verify against a live P4 GET_DEVICE_BY_ID response
// before shipping — see task-18-report.md's concerns section.
//
// ── Boot-reconcile choice ──────────────────────────────────────────────
// The brief's escape hatch (Task-18 environment facts item 4) is taken:
// boot restore does NOT run a full paged GET_DEVICES fetch to prune stale
// ieees first. It just re-exposes every persisted ieee; a stale/removed
// ieee's GET_DEVICE_BY_ID fetch fails harmlessly (logged, loop continues —
// see rmk_boot_restore below). Full reconcile-against-live-list is a
// follow-up, not implemented here.
//
// ── Serialization (Task 18 pinned must-verify #2) ────────────────────────
// See the comment at s_table_mtx below.
//
// ── UAF window (Task 18 pinned must-verify #3) ───────────────────────────
// See the comment at the rmk_bridge_unexpose_device() call site in
// api_device_rainmaker_remove below. No fix attempted — log-and-proceed,
// flagged for the Gate-B watchlist.
#include "sdkconfig.h"
#include "api_handlers.h"
#include "s3_internal.h"
#include "json_buf.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "mqtt_gw.h"
#include "nvs_helpers.h"
#include "zap_store.h"
#include "rainmaker_gw.h"
#include "rmk_bridge.h"
#include "rmk_store.h"
#include "ArduinoJson.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>
#include <memory>

static const char* TAG_RMK = "api_rainmaker";

// ── uplink <-> string ──────────────────────────────────────────────────
static const char* uplink_to_str(zhac_uplink_t u) {
    switch (u) {
    case ZHAC_UPLINK_NONE:        return "none";
    case ZHAC_UPLINK_CUSTOM_MQTT: return "custom_mqtt";
    case ZHAC_UPLINK_RAINMAKER:   return "rainmaker";
    default:                      return "none";
    }
}
static bool str_to_uplink(const char* s, zhac_uplink_t* out) {
    if (!s) return false;
    if (!strcmp(s, "none"))        { *out = ZHAC_UPLINK_NONE;        return true; }
    if (!strcmp(s, "custom_mqtt")) { *out = ZHAC_UPLINK_CUSTOM_MQTT; return true; }
    if (!strcmp(s, "rainmaker"))   { *out = ZHAC_UPLINK_RAINMAKER;   return true; }
    return false;
}

// ── rmk_state_t -> string (brief's exact vocabulary) ─────────────────────
static const char* rmk_state_to_str(rmk_state_t s) {
    switch (s) {
    case RMK_ST_DISABLED:     return "disabled";
    case RMK_ST_INIT_CLAIM:   return "init_claim";
    case RMK_ST_CONNECTING:   return "connecting";
    case RMK_ST_UNASSOCIATED: return "unassociated";
    case RMK_ST_READY:        return "ready";
    case RMK_ST_BACKOFF:      return "backoff";
    case RMK_ST_CLAIM_FAILED: return "claim_failed";
    default:                  return "disabled";
    }
}

// ── Task 18 pinned must-verify #2: rmk_store serialization ───────────────
// rmk_store's load()/save() (rmk_store.c) each lock independently against
// EACH OTHER (its own recursive NVS mutex guards one call at a time) but do
// NOT serialize a whole load-mutate-save SEQUENCE against a concurrent
// second sequence — two WS/REST workers (esp_http_server's httpd task vs.
// the WS RX path, or a REST client racing a WS client) could each load the
// same starting table, mutate their own copy, and save, with the second
// save silently clobbering the first's change. g_table below is THE single
// in-memory copy — loaded once, lazily, on first use — that every entry
// point in this file (device.rainmaker.add/remove, rmk_boot_restore) reads
// and mutates. s_table_mtx is the ONE serialization point that makes a
// load-mutate-save sequence atomic with respect to every other sequence
// here.
//
// OWNERSHIP RULE: g_table may only be read or written while holding
// s_table_mtx. The one sanctioned exception is a short-lived stack COPY
// taken while holding the lock (rmk_boot_restore below) — the copy itself
// needs no lock afterward, only the read that produced it did. Symmetric to
// rmk_bridge.c's own reg_lock() discipline: NEVER hold s_table_mtx across a
// blocking call. The HAP GET_DEVICE_BY_ID roundtrip (up to 5 s) and the
// esp_rmaker_* work inside rmk_bridge_expose_device both run OUTSIDE this
// lock in rmk_expose_device_from_ieee() below — only the final
// rmk_tbl_add/_remove + rmk_store_save step is taken under it.
static RmkTable          g_table;
static bool              s_table_loaded = false;
static SemaphoreHandle_t s_table_mtx    = nullptr;

static void table_lock() {
    if (!s_table_mtx) s_table_mtx = xSemaphoreCreateMutex();
    if (s_table_mtx) xSemaphoreTake(s_table_mtx, portMAX_DELAY);
    if (!s_table_loaded) {
        rmk_store_load(&g_table);   // zeroed table if absent/corrupt — an empty set is fine
        s_table_loaded = true;
    }
}
static void table_unlock() {
    if (s_table_mtx) xSemaphoreGive(s_table_mtx);
}

// ── Debounced node-config republish ───────────────────────────────────────
// Mirrors main/api_devices.cpp's opt_stage_and_defer/s_opt_flush_timer
// pattern verbatim: each add/remove (or boot-restore entry) re-arms a 2 s
// one-shot timer, so a burst (the boot-restore loop, or a user adding
// several devices back to back) collapses into ONE
// esp_rmaker_report_node_details() call, 2 s after the last one — never the
// esp_rmaker_* call itself, only the timer plumbing (see file banner).
static esp_timer_handle_t s_republish_timer      = nullptr;
static constexpr int64_t  kRepublishDebounceUs   = 2'000'000;   // 2 s, per brief

static void republish_timer_cb(void*) {
    rmk_bridge_report_node_details_now();
}

static void schedule_republish() {
    if (!s_republish_timer) {
        const esp_timer_create_args_t args = {
            .callback = &republish_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "rmk_republish",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_republish_timer) != ESP_OK) {
            s_republish_timer = nullptr;
        }
    }
    if (s_republish_timer) {
        esp_timer_stop(s_republish_timer);              // restart the debounce window
        esp_timer_start_once(s_republish_timer, kRepublishDebounceUs);
    } else {
        // No timer available — report immediately rather than silently
        // dropping the republish.
        rmk_bridge_report_node_details_now();
    }
}

// ── Shared {"devices":[{"ieee":..,"type":..}]} reply builder ─────────────
// Used by device.rainmaker.list AND as the reply for a successful add/remove
// (brief: "Reply = updated list").
static ApiStatus rmk_write_device_list(char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    rmk_bridge_dev_info_t infos[RMK_MAX_DEVS];
    size_t n = rmk_bridge_list(infos, RMK_MAX_DEVS);

    JsonWriter w(rsp_buf, rsp_cap);
    w.raw("{\"devices\":[");
    for (size_t i = 0; i < n; i++) {
        if (i) w.ch(',');
        w.raw("{\"ieee\":\"0x");
        w.fmt("%016llX", (unsigned long long)infos[i].ieee);
        w.raw("\",\"type\":\"");
        w.raw(infos[i].type ? infos[i].type : "esp.device.other");
        w.raw("\"}");
    }
    w.raw("]}");
    size_t len = w.finish();
    if (rsp_len) *rsp_len = len;
    return len ? API_OK : API_INTERNAL_ERROR;
}

// Raw exposes array cap — generous headroom over rmk_typemap.h's
// RMK_MAX_PARAMS(8): a device can (and usually does) carry exposes
// rmk_typemap.c's kMap doesn't map (linkquality, battery, ...); rmk_classify/
// rmk_build_params need the FULL set to classify correctly, and simply skip
// anything they don't recognize.
static constexpr size_t kRmkRawExposeCap = 32;
static constexpr size_t kRmkDevRspCap    = HAP_MAX_PAYLOAD;

// ── Shared helper: fetch a device's live exposes + expose it to RainMaker
// ── (device.rainmaker.add AND rmk_boot_restore both call this) ───────────
// Fetches via the SAME mechanism api_device_get uses (HAP GET_DEVICE_BY_ID
// — main/api_devices.cpp), builds an rmk_expose_t[] from the response,
// derives the display name (collision-checked against currently-exposed
// names), calls rmk_bridge_expose_device, and on success records the ieee
// in the persisted table + schedules a debounced republish. Does NOT build
// any reply JSON — callers do that.
static esp_err_t rmk_expose_device_from_ieee(uint64_t ieee) {
    uint8_t req_buf[48];
    uint16_t req_len = 0;
    hap_json_encode_get_device_req(req_buf, sizeof(req_buf), &req_len, ieee);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kRmkDevRspCap]);
    if (!rsp) return ESP_ERR_NO_MEM;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::GET_DEVICE_BY_ID, req_buf, req_len,
                           rsp.get(), kRmkDevRspCap, &got, 5000)) {
        return ESP_ERR_TIMEOUT;
    }

    JsonDocument doc;
    if (deserializeJson(doc, rsp.get(), got)) return ESP_FAIL;

    // See file banner: exposes-shape assumption, not verified against P4 source.
    rmk_expose_t ex[kRmkRawExposeCap];
    size_t n = 0;
    JsonArrayConst exposes = doc["exposes"].as<JsonArrayConst>();
    for (JsonObjectConst o : exposes) {
        if (n >= kRmkRawExposeCap) break;
        const char* prop = o["property"] | (const char*)nullptr;
        if (!prop || !prop[0]) prop = o["name"] | (const char*)nullptr;
        if (!prop || !prop[0]) continue;
        bool writable;
        if (o["access"].is<int>()) {
            writable = (o["access"].as<int>() & 0x2) != 0;   // z2m ACCESS_SET bit
        } else {
            writable = o["writable"] | false;
        }
        utf8_safe_copy(ex[n].name, sizeof(ex[n].name), prop);
        ex[n].writable = writable;
        n++;
    }

    // Friendly name: ZHAC's own current name if present, else a synthetic
    // "ZHAC-<last4hex>" fallback. Collision-checked against every
    // currently-exposed RainMaker device name (RainMaker requires unique
    // names within a node — rmk_bridge_expose_device's own doc says this
    // check is the caller's job). utf8_safe_copy throughout: this name
    // becomes a value the phone app renders and, via rmk_bridge_
    // on_device_renamed's mirror, something ZHAC's own rename path also
    // touches — this codebase has twice fixed a poisoned-UTF-8-name class
    // of bug (WS text-frame killer), so sanitizing at the point of
    // introduction is deliberate, not decoration.
    char display[30];
    const char* fname = doc["name"] | (const char*)nullptr;
    if (fname && fname[0]) {
        utf8_safe_copy(display, sizeof(display), fname);
    } else {
        snprintf(display, sizeof(display), "ZHAC-%04llX",
                 (unsigned long long)(ieee & 0xFFFFULL));
    }

    rmk_bridge_dev_info_t infos[RMK_MAX_DEVS];
    size_t ninfo = rmk_bridge_list(infos, RMK_MAX_DEVS);
    bool collide = false;
    for (size_t i = 0; i < ninfo; i++) {
        if (infos[i].name && strcmp(infos[i].name, display) == 0) { collide = true; break; }
    }
    if (collide) {
        char base[30];
        utf8_safe_copy(base, sizeof(base) - 5, display);   // leave room for "-XXXX"
        char suffixed[30];
        // Precision on %s (24 == sizeof(suffixed) - 1('-') - 4(hex) - 1(NUL))
        // makes the worst-case output length provably <= sizeof(suffixed) to
        // the compiler — utf8_safe_copy's own cap above already keeps `base`
        // within 24 bytes at runtime, but -Wformat-truncation can't reason
        // through a non-printf-family function call, only through the
        // format string itself (GCC error seen in CI: format-truncation on
        // the un-precisioned "%s-%04llX").
        snprintf(suffixed, sizeof(suffixed), "%.24s-%04llX", base,
                 (unsigned long long)(ieee & 0xFFFFULL));
        utf8_safe_copy(display, sizeof(display), suffixed);
    }

    esp_err_t err = rmk_bridge_expose_device(ieee, display, ex, n);
    if (err != ESP_OK) return err;

    table_lock();
    rmk_tbl_add(&g_table, ieee);
    rmk_store_save(&g_table);
    table_unlock();

    schedule_republish();
    return ESP_OK;
}

// ── Boot-time restore (Task 18 dynamic-exposure half) ────────────────────
// Registered with components/rainmaker_gw via rmk_bridge_set_boot_restore()
// (main.cpp, right after rmk_bridge_set_attr_writer) — called from
// rmk_bridge_attach(), which rainmaker_gw.c invokes on the FIRST READY
// transition each boot. See file banner for the boot-reconcile choice (no
// paged GET_DEVICES prune; a stale ieee just fails its own fetch, logged,
// loop continues).
extern "C" void rmk_boot_restore(void) {
    RmkTable snapshot;
    table_lock();
    snapshot = g_table;   // lazy-load already ran inside table_lock() above
    table_unlock();

    size_t restored = 0;
    for (size_t i = 0; i < snapshot.count; i++) {
        esp_err_t err = rmk_expose_device_from_ieee(snapshot.ieee[i]);
        if (err == ESP_OK) {
            restored++;
        } else {
            ESP_LOGW(TAG_RMK, "boot restore: ieee=0x%016llX failed (%s) — skipping, continuing",
                     (unsigned long long)snapshot.ieee[i], esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG_RMK, "boot restore: %u/%u persisted device(s) re-exposed",
             (unsigned)restored, (unsigned)snapshot.count);
}

// ── uplink.get ─────────────────────────────────────────────────────────
extern "C" ApiStatus api_uplink_get(const char* /*body*/, size_t /*body_len*/,
                                     char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    int n = snprintf(rsp_buf, rsp_cap, "{\"uplink\":\"%s\"}", uplink_to_str(zhac_uplink_get()));
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

// ── uplink.set {mode} ──────────────────────────────────────────────────
// Orchestration matrix (design decision already made — SDK node cannot be
// de-initialized once esp_rmaker_start() has run):
//   mode:"rainmaker" on a flag-off build -> rejected with an "err" field
//                                        BEFORE any side effect (no persist,
//                                        no mqtt_gw_stop()) — see the
//                                        #if !CONFIG_ZHAC_RAINMAKER_ENABLE
//                                        block just below
//   same mode                        -> no-op, reply current
//   none/custom_mqtt -> custom_mqtt  -> start mqtt_gw
//   any -> rainmaker (flag-on only)  -> mqtt_gw_stop() [explicit, pinned
//                                        review item] then rainmaker_gw_init()
//                                        (late-init no-op if already run this
//                                        boot — rainmaker_gw.c's Task 18
//                                        reentrancy guard)
//   rainmaker -> none/custom_mqtt    -> persist + reboot_required:true,
//                                        agent is NOT touched (cannot be
//                                        stopped)
//   custom_mqtt -> none              -> mqtt_gw_stop()
// zhac_uplink_set() always runs FIRST, before any of the above (other than
// the flag-off rainmaker rejection, which runs before even that), for every
// real (non-no-op) transition.
extern "C" ApiStatus api_uplink_set(const char* body, size_t body_len,
                                     char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* mode_str = doc["mode"] | (const char*)nullptr;
    zhac_uplink_t new_mode;
    if (!str_to_uplink(mode_str, &new_mode)) return API_BAD_REQUEST;

#if !CONFIG_ZHAC_RAINMAKER_ENABLE
    // Reject BEFORE any side effect on a flag-off build (post-merge review
    // finding). The original cut of this function called zhac_uplink_set()
    // and then mqtt_gw_stop() unconditionally, reaching the flag-off/
    // flag-on split only afterward, inside the new_mode==RAINMAKER branch
    // below — so a flag-off uplink.set{mode:"rainmaker"} tore down a
    // WORKING custom_mqtt connection, started nothing (rainmaker_gw_init()
    // is compiled out), and persisted a non-functional selector, stranding
    // the device with no uplink at all until a manual recovery. This early
    // return runs before zhac_uplink_get()/_set() or mqtt_gw_stop() are
    // ever touched, so the existing uplink (persisted selector + live
    // mqtt_gw state) is left exactly as it was. Compile-time guard, not a
    // runtime rainmaker_gw_state()==RMK_ST_DISABLED check, for the same
    // reason this used to be a compile-time split further down: DISABLED is
    // ALSO the legitimate state on a flag-ON build before any claim
    // attempt, so it is not a reliable "not compiled in" signal on its
    // own — #if is unambiguous and cannot silently start lying if this
    // function's call order ever changes.
    if (new_mode == ZHAC_UPLINK_RAINMAKER) {
        int n = snprintf(rsp_buf, rsp_cap,
                         "{\"uplink\":\"rainmaker\",\"err\":\"rainmaker not compiled in\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }
#endif

    zhac_uplink_t old_mode = zhac_uplink_get();
    if (new_mode == old_mode) {
        int n = snprintf(rsp_buf, rsp_cap, "{\"uplink\":\"%s\"}", uplink_to_str(new_mode));
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }

    // Persist FIRST (design decision) — every branch below assumes
    // zhac_uplink_get() already reflects new_mode. Never reached for
    // new_mode==RAINMAKER on a flag-off build (rejected above, before any
    // of this runs).
    zhac_uplink_set(new_mode);

    if (old_mode == ZHAC_UPLINK_RAINMAKER) {
        // Switching AWAY from rainmaker (to none or custom_mqtt). The SDK
        // node cannot be de-initialized/stopped once started — there is no
        // esp_rmaker_stop()/deinit in this SDK version, confirmed against
        // esp_rmaker_core.h. Persist + tell the client a reboot is
        // required; the new mode takes effect next boot, when
        // rainmaker_gw_init()'s own uplink-selector check early-returns
        // instead. Do NOT attempt to stop the agent here.
        int n = snprintf(rsp_buf, rsp_cap,
                         "{\"uplink\":\"%s\",\"reboot_required\":true}",
                         uplink_to_str(new_mode));
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }

    if (new_mode == ZHAC_UPLINK_RAINMAKER) {
        // Only reachable on a flag-on build now — the flag-off case was
        // rejected and returned above, before old_mode was even read, so
        // mqtt_gw_stop() below never runs as a dead-end teardown with
        // nothing to replace it.
        //
        // MUST be the explicit call (pinned review item) — mqtt_gw_stop()
        // is cheap/idempotent even when mqtt wasn't running (old_mode ==
        // NONE); called unconditionally rather than trying to be clever
        // about old_mode.
        mqtt_gw_stop();
        // Late init is safe: rainmaker_gw_init() was designed to run
        // post-wifi (long true by this point in boot) and is now
        // idempotent within one boot (rainmaker_gw.c's Task 18 reentrancy
        // guard on s_state) — if the agent already ran once this boot
        // (persisted uplink was already RAINMAKER at app_main() time),
        // this call is a logged no-op, not a double-init.
        rainmaker_gw_init();
        int n = snprintf(rsp_buf, rsp_cap, "{\"uplink\":\"rainmaker\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }

    if (new_mode == ZHAC_UPLINK_CUSTOM_MQTT) {
        // old_mode == NONE here (RAINMAKER handled above; CUSTOM_MQTT->
        // CUSTOM_MQTT is the no-op case already returned). Mirrors
        // main/api_system.cpp's api_settings_set mqtt_enabled=true live-
        // apply branch verbatim (the existing "start mqtt_gw" entry point).
        char url[128] = {}, root[32] = {}, cid[32] = {};
        nvs_read_mqtt_cfg(nullptr, url, sizeof(url), root, sizeof(root), cid, sizeof(cid));
        if (root[0]) mqtt_gw_set_root_topic(root);
        if (cid[0])  mqtt_gw_set_client_id(cid);
        if (url[0])  mqtt_gw_set_broker_url(url);
        else         mqtt_gw_start();
    } else {
        // new_mode == ZHAC_UPLINK_NONE, old_mode == CUSTOM_MQTT (RAINMAKER
        // and the no-op case are both handled above). "Also stop mqtt_gw
        // when switching to none" — brief.
        mqtt_gw_stop();
    }

    int n = snprintf(rsp_buf, rsp_cap, "{\"uplink\":\"%s\"}", uplink_to_str(new_mode));
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

// ── rainmaker.status ───────────────────────────────────────────────────
extern "C" ApiStatus api_rainmaker_status(const char* /*body*/, size_t /*body_len*/,
                                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    const char* node_id = rainmaker_gw_node_id();
    JsonWriter w(rsp_buf, rsp_cap);
    w.raw("{\"state\":\"");
    w.raw(rmk_state_to_str(rainmaker_gw_state()));
    w.raw("\",\"node_id\":\"");
    w.str(node_id, strlen(node_id));
    w.raw("\",\"devices\":");
    w.fmt("%u", (unsigned)rmk_bridge_device_count());
    w.raw("}");
    size_t len = w.finish();
    if (rsp_len) *rsp_len = len;
    return len ? API_OK : API_INTERNAL_ERROR;
}

// ── rainmaker.assoc.set {user_id,secret} ──────────────────────────────
extern "C" ApiStatus api_rainmaker_assoc_set(const char* body, size_t body_len,
                                              char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* user_id = doc["user_id"] | (const char*)nullptr;
    const char* secret  = doc["secret"]  | (const char*)nullptr;
    if (!user_id || !user_id[0] || !secret || !secret[0]) return API_BAD_REQUEST;

    esp_err_t err = rainmaker_gw_assoc_start(user_id, secret);
    const size_t n = (err == ESP_OK) ? api_write_ok(rsp_buf, rsp_cap)
                                      : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// ── device.rainmaker.list ──────────────────────────────────────────────
extern "C" ApiStatus api_device_rainmaker_list(const char* /*body*/, size_t /*body_len*/,
                                                char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    return rmk_write_device_list(rsp_buf, rsp_cap, rsp_len);
}

// ── device.rainmaker.add {ieee} ────────────────────────────────────────
extern "C" ApiStatus api_device_rainmaker_add(const char* body, size_t body_len,
                                               char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || !ieee_str[0]) return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;

    // Cap check FIRST, before the (up to 5 s) HAP fetch below — brief §3.
    table_lock();
    bool already = rmk_tbl_contains(&g_table, ieee);
    bool full    = !already && g_table.count >= RMK_MAX_DEVS;
    table_unlock();

    if (full) {
        int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":false,\"err\":\"cap reached (10)\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }

    if (!already) {
        esp_err_t err = rmk_expose_device_from_ieee(ieee);
        if (err != ESP_OK) {
            int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":false,\"err\":\"%s\"}",
                             esp_err_to_name(err));
            if (rsp_len) *rsp_len = (size_t)n;
            return API_OK;
        }
    }
    // Already-exposed ieee: idempotent success, falls straight through to
    // the current list below (rmk_tbl_add's own dedup semantics).
    return rmk_write_device_list(rsp_buf, rsp_cap, rsp_len);
}

// ── device.rainmaker.remove {ieee} ─────────────────────────────────────
extern "C" ApiStatus api_device_rainmaker_remove(const char* body, size_t body_len,
                                                  char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || !ieee_str[0]) return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;

    // Task 18 pinned must-verify #3 (UAF window, Task-17 review finding):
    // rmk_bridge_unexpose_device races an in-flight write-cb
    // (rmk_bridge.c's rmk_bridge_write_cb) for up to ~3000 ms — the write-cb
    // holds this device/param's SDK pointers across the injected
    // attr-writer's blocking HAP roundtrip, and esp_rmaker_device_delete
    // (called from unexpose, inside rmk_bridge.c) would free exactly those
    // pointers out from under it. No fix attempted here — a refcount/
    // quiesce redesign is out of scope for Task 18 (see rmk_bridge.h's own
    // doc on rmk_bridge_unexpose_device). Log-and-proceed is the accepted
    // mitigation; flagged again in task-18-report.md's Gate-B watchlist.
    esp_err_t uerr = rmk_bridge_unexpose_device(ieee);
    if (uerr != ESP_OK && uerr != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG_RMK, "device.rainmaker.remove ieee=0x%016llX: unexpose returned %s",
                 (unsigned long long)ieee, esp_err_to_name(uerr));
    }

    table_lock();
    rmk_tbl_remove(&g_table, ieee);
    rmk_store_save(&g_table);
    table_unlock();

    schedule_republish();
    return rmk_write_device_list(rsp_buf, rsp_cap, rsp_len);
}

// ── DEVICE_LEAVE unpair hook (Task 21 audit fix) ─────────────────────────
// Called unconditionally from main/hap_bridge.cpp's HapMsgType::DEVICE_LEAVE
// case, right alongside the existing rmk_bridge_on_device_gone() (Task 16 —
// that one only cleans up components/rainmaker_gw's in-memory registry; it
// cannot touch g_table, which lives in this file, per this component's own
// "no main/ headers" layering rule — see rmk_bridge.h's file banner).
// Without this, a device unpaired from the Zigbee network entirely while
// still exposed to RainMaker stayed in the persisted "zhac_rmk" NVS set
// forever: every future boot's rmk_boot_restore() would re-attempt exposing
// it, fail its GET_DEVICE_BY_ID fetch (harmlessly logged, per that
// function's own "per-device failure, loop continues" contract — spec
// section 6's stale-ieee row), and just burn a wasted ~5 s HAP timeout on
// every single boot from then on, silently, forever.
//
// Mirrors api_device_rainmaker_remove's own persisted-set-drop + debounced
// republish above, conditioned on the ieee having actually been a member:
// DEVICE_LEAVE fires for EVERY departing device, exposed to RainMaker or
// not, so an unconditional save+republish here would mean every ordinary
// Zigbee unpair (the overwhelming common case, nothing to do with
// RainMaker) pays an NVS write and a debounced cloud republish for no
// reason — the same "cheap no-op unless there's real work" discipline
// rmk_bridge_active()/rmk_bridge_on_device_gone() already follow.
extern "C" void rmk_on_device_gone(uint64_t ieee) {
    table_lock();
    bool had = rmk_tbl_remove(&g_table, ieee);
    if (had) rmk_store_save(&g_table);
    table_unlock();
    if (had) schedule_republish();
}
