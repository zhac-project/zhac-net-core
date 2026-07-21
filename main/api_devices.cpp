// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared command handlers — domain split from former api_handlers.cpp.
// Handlers here are PURE business logic. Auth, rate-limiting, URI parsing,
// and transport framing live in rest_*.cpp (HTTP) and ws_bridge.cpp (WS).

#include "api_handlers.h"
#include "s3_internal.h"
#include "log_ring.h"
#include "groups_store.h"
#include "json_buf.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "wifi_mgr.h"
#include "rmk_bridge.h"   // rmk_bridge_on_device_renamed (Task 18)
#include "ArduinoJson.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <memory>
#include <atomic>

static const char* TAG_API = "api_devices";

// ── Deferred NVS commit for the device-options namespace (FINDINGS §6) ─────
// `api_device_options_set` is a moderately hot handler (per-device option
// write; can arrive in bursts when the SPA pushes several devices' options or
// a script bulk-applies). It used to `nvs_commit(s_nvs_zhac_opt)` on EVERY
// call — each commit erases+rewrites a flash page on the httpd task, so a
// burst meant flash wear plus per-call latency on the shared HTTP worker.
//
// `nvs_set_str` already stages the value in RAM and makes it visible to the
// next read on the SAME long-lived handle; only the `nvs_commit` need be
// durable. NVS serialises handle ops internally; only commit ORDERING matters
// here, not a data race (set/get run on the httpd task, commit on the esp_timer
// task). So we debounce the commit: each set re-arms a one-shot timer and
// the commit lands once the writes go quiet (DEBOUNCE_US), coalescing a burst
// into a single page write. `s_opt_dirty` guards against a needless commit.
// Mirrors the rule_store deferred-flush pattern (F-04) — including an explicit
// flush-now hook the reboot path calls so nothing in-flight is lost.
static esp_timer_handle_t s_opt_flush_timer = nullptr;
static std::atomic<bool>  s_opt_dirty{false};
static constexpr int64_t  kOptFlushDebounceUs = 2'000'000;  // 2 s quiet window

static void opt_commit_now() {
    // Single-shot the staged page write. Cheap no-op when nothing is dirty.
    if (s_nvs_zhac_opt && s_opt_dirty.exchange(false)) {
        esp_err_t err = nvs_commit(s_nvs_zhac_opt);
        if (err != ESP_OK) {
            // Re-mark so a later set / explicit flush retries; the staged
            // values are still in the handle, just not yet durable.
            s_opt_dirty.store(true);
            ESP_LOGW(TAG_API, "deferred zhac_opt commit failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static void opt_flush_timer_cb(void*) { opt_commit_now(); }

// Called from the reboot path (main.cpp) alongside rule_store_flush_now() so a
// pending device-options write is made durable before restart.
extern "C" void api_device_opt_flush_now() {
    if (s_opt_flush_timer) esp_timer_stop(s_opt_flush_timer);  // cancel pending
    opt_commit_now();
}

// Stage `write_str` under `nvs_key` and (re)arm the debounced commit. Returns
// the nvs_set_str result; commit durability is handled by the timer.
static esp_err_t opt_stage_and_defer(const char* nvs_key, const char* write_str) {
    esp_err_t err = nvs_set_str(s_nvs_zhac_opt, nvs_key, write_str);
    if (err != ESP_OK) return err;
    s_opt_dirty.store(true);
    if (!s_opt_flush_timer) {
        const esp_timer_create_args_t args = {
            .callback = &opt_flush_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "opt_flush",
            .skip_unhandled_events = true,
        };
        // Best-effort: if timer creation fails, fall back to an immediate
        // commit below so the write is never silently left non-durable.
        if (esp_timer_create(&args, &s_opt_flush_timer) != ESP_OK) {
            s_opt_flush_timer = nullptr;
        }
    }
    if (s_opt_flush_timer) {
        esp_timer_stop(s_opt_flush_timer);                 // restart the window
        esp_timer_start_once(s_opt_flush_timer, kOptFlushDebounceUs);
    } else {
        opt_commit_now();   // no timer available — commit synchronously
    }
    return ESP_OK;
}

// Each DEVICE_LIST chunk the P4 sends is one SPI frame (<= HAP_MAX_PAYLOAD).
static constexpr size_t kDevListChunkCap = HAP_MAX_PAYLOAD;
static constexpr size_t kDevInfoRspCap   = HAP_MAX_PAYLOAD;

// PAGING (HOTFIX): the P4 cannot fit a full fleet in one 4096-byte frame
// (~16 realistic devices overflow it), so GET_DEVICES is now paged — each
// roundtrip returns one chunk `{"next":N,"devices":[...]}` and we follow the
// `next` cursor until done, accumulating every device into ONE reassembled
// `{"devices":[...]}`. Worst case ZAP_MAX_DEVICES(200) × ~320 B ≈ 64 KB, so
// the accumulator lives in PSRAM (internal DRAM is the tight heap on S3).
static constexpr size_t kDevListAccumCap = 64 * 1024;
// Loop bound — never spin even if a buggy peer fails to advance the cursor.
static constexpr int     kDevListMaxPages = ZAP_MAX_DEVICES + 8;

// ── Devices ──────────────────────────────────────────────────────────────

// Coalesce burst calls. The SPA's auto-refresh + manual click can queue a
// dozen `device.list` cmds in <100 ms; without this gate every one fires
// a fresh (now multi-page) GET_DEVICES round-trip across SPI, exhausting
// the HAP session window and tipping both sides into retransmit storms.
// 1 s TTL is generous — DEVICE_LIST changes only on join/leave, and
// push events (`device.added/removed`) refresh the cache out-of-band.
static constexpr int64_t kDevListCacheMs = 1000;
static SemaphoreHandle_t s_devlist_cache_mutex = nullptr;
static int64_t           s_devlist_last_ok_ms  = 0;
static char*             s_devlist_cache_buf   = nullptr;   // holds the FULL list
static size_t            s_devlist_cache_len   = 0;

static void devlist_cache_init() {
    if (s_devlist_cache_mutex) return;
    s_devlist_cache_mutex = xSemaphoreCreateMutex();
    // Cache the reassembled list (not a single chunk), so size it to the
    // accumulator, not HAP_MAX_PAYLOAD.
    s_devlist_cache_buf   = static_cast<char*>(
        heap_caps_malloc(kDevListAccumCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(s_devlist_cache_mutex && s_devlist_cache_buf);
}

// Fetch every device page from the P4 and reassemble into `out` as a single
// `{"devices":[...]}` document. Returns the assembled length (excl. NUL) on
// success, or 0 on failure (timeout / parse error / accumulator overflow).
// `out` must be at least kDevListAccumCap bytes.
//
// CURSOR IS A RAW ARRAY INDEX, not a stable device key. If the pool gains or
// loses a device between pages (multi-roundtrip paging is NOT a single atomic
// snapshot — the pool lock is held only per-chunk on P4), indices shift and this
// reassembly may drop or duplicate ONE device in THIS response only. Self-heals
// on the next refresh (1s cache) + device.added/removed events. Proper fix: a
// pool-generation token in the envelope, restart-on-mismatch — deferred (exceeds
// hotfix scope). See FINDINGS follow-up.
static size_t devlist_fetch_all(char* out, size_t out_cap) {
    // Per-chunk receive buffer (one SPI frame). PSRAM via rest_big_alloc so a
    // burst of concurrent callers doesn't churn the tight internal heap.
    char* chunk = static_cast<char*>(rest_big_alloc(kDevListChunkCap));
    if (!chunk) return 0;

    // Build the reassembled array by appending each chunk's elements. We open
    // the envelope once, splice every device object across all pages, close
    // once. Re-serialising each element keeps the element shape byte-exact.
    size_t pos = 0;
    auto put = [&](const char* s, size_t n) -> bool {
        if (pos + n + 1 > out_cap) return false;   // +1 for trailing NUL
        memcpy(out + pos, s, n);
        pos += n;
        return true;
    };
    if (!put("{\"devices\":[", 12)) { free(chunk); return 0; }

    bool first_elem = true;
    uint16_t start   = 0;
    bool ok_done     = false;

    // P4-T29 (FINDINGS §5): this loop runs on the SINGLE httpd task and the
    // DEVICE_LIST paging hotfix turned device.list into up to kDevListMaxPages
    // (ZAP_MAX_DEVICES+8 = 208) sequential 5 s hap_roundtrip_v2 calls. A dead
    // or marginal P4 link (every page times out) would otherwise pin the httpd
    // task — and therefore ALL REST + WS + queued async sends — for up to
    // 208 × 5 s ≈ 17 minutes. Bound the CUMULATIVE wall-clock the paged loop
    // may consume; once exceeded we abort and report a failed fetch (the
    // caller then serves the 1 s stale cache or a clean error), rather than
    // emitting a half-fleet that LOOKS complete. Worst-case stall is now
    // kDevListBudgetUs + one in-flight roundtrip (≤5 s) ≈ 13 s. The healthy
    // path (fleet paged in tens of ms) never trips this. NOTE: the proper fix
    // is to move multi-roundtrip commands off the httpd task onto a worker —
    // recommended follow-up; this budget is the minimal stall containment.
    static constexpr int64_t kDevListBudgetUs = 8'000'000;   // 8 s total
    const int64_t deadline_us = esp_timer_get_time() + kDevListBudgetUs;

    for (int page = 0; page < kDevListMaxPages; page++) {
        if (esp_timer_get_time() > deadline_us) {
            ESP_LOGE(TAG_API,
                     "device.list exceeded %lld ms budget at page %d (start=%u) — aborting",
                     (long long)(kDevListBudgetUs / 1000), page, (unsigned)start);
            free(chunk);
            return 0;   // treat as failed fetch — caller falls back to cache/error
        }
        uint8_t req[2] = { static_cast<uint8_t>(start & 0xFF),
                           static_cast<uint8_t>((start >> 8) & 0xFF) };
        size_t got = 0;
        // One retry per page, deadline-gated: with NEEDS_ACK requests a single
        // lost frame self-heals via session retransmit, but a double loss can
        // still burn one 5 s timeout — don't fail the whole list (and the SPA
        // login gate behind it) over one bad page. The deadline check keeps
        // the documented worst-case stall (budget + one in-flight roundtrip)
        // unchanged.
        bool page_ok = false;
        for (int attempt = 0; attempt < 2 && !page_ok; attempt++) {
            if (attempt > 0) {
                if (esp_timer_get_time() > deadline_us) break;
                ESP_LOGW(TAG_API, "device.list page %d retry (start=%u)",
                         page, (unsigned)start);
            }
            page_ok = hap_roundtrip_v2(HapMsgType::GET_DEVICES, req, sizeof(req),
                                       chunk, kDevListChunkCap, &got, 5000);
        }
        if (!page_ok) {
            ESP_LOGE(TAG_API, "device.list page %d roundtrip failed (start=%u)",
                     page, (unsigned)start);
            free(chunk);
            return 0;
        }

        JsonDocument doc;
        DeserializationError de = deserializeJson(doc, chunk, got);
        if (de) {
            ESP_LOGE(TAG_API, "device.list page %d JSON parse: %s",
                     page, de.c_str());
            free(chunk);
            return 0;
        }

        JsonArrayConst devs = doc["devices"].as<JsonArrayConst>();
        for (JsonObjectConst o : devs) {
            if (!first_elem && !put(",", 1)) { free(chunk); return 0; }
            // Re-serialise this element straight into the accumulator tail.
            size_t avail = out_cap - pos - 1;            // reserve NUL
            size_t w = serializeJson(o, out + pos, avail);
            if (w == 0 || w >= avail) {                  // element didn't fit
                ESP_LOGE(TAG_API, "device.list accumulator overflow at page %d",
                         page);
                free(chunk);
                return 0;
            }
            pos += w;
            first_elem = false;
        }

        // `next` cursor: absent / >= would-not-advance means done. The P4
        // sets next == device count when the final page is reached, and
        // guarantees forward progress (next > start) otherwise.
        // NOTE: the terminal DATA page reports next==count (>start), so S3 always spends one final empty GET_DEVICES roundtrip to observe next==start. Bounded + cached 1s; acceptable.
        uint16_t next = doc["next"] | static_cast<uint16_t>(0);
        if (!doc["next"].is<uint16_t>() || next <= start) {
            // Either no cursor (legacy single-frame peer) or the terminal
            // page — we are done after consuming this chunk.
            ok_done = true;
            break;
        }
        start = next;
    }

    free(chunk);
    if (!ok_done) {
        ESP_LOGE(TAG_API, "device.list paging hit max %d pages without done",
                 kDevListMaxPages);
        return 0;
    }
    if (!put("]}", 2)) return 0;
    out[pos] = '\0';
    return pos;
}

extern "C" ApiStatus api_device_list(const char* /*body*/, size_t /*body_len*/,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    devlist_cache_init();

    // Cache lookup under the cache mutex. The cache exists ONLY to absorb
    // SPA refresh bursts; v2 already permits concurrent callers, so this
    // is no longer the only serialisation point.
    {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_devlist_cache_mutex, portMAX_DELAY);
        if (s_devlist_last_ok_ms != 0 &&
            (now_ms - s_devlist_last_ok_ms) < kDevListCacheMs &&
            s_devlist_cache_len > 0) {
            size_t n = s_devlist_cache_len;
            if (n >= rsp_cap) n = rsp_cap - 1;
            memcpy(rsp_buf, s_devlist_cache_buf, n);
            rsp_buf[n] = '\0';
            xSemaphoreGive(s_devlist_cache_mutex);
            if (rsp_len) *rsp_len = n;
            ESP_LOGD(TAG_API, "device.list cache hit age=%lldms",
                     (long long)(now_ms - s_devlist_last_ok_ms));
            return API_OK;
        }
        xSemaphoreGive(s_devlist_cache_mutex);
    }

    // Reassemble all pages into a PSRAM accumulator.
    char* accum = static_cast<char*>(rest_big_alloc(kDevListAccumCap));
    if (!accum) return API_INTERNAL_ERROR;
    size_t total = devlist_fetch_all(accum, kDevListAccumCap);
    if (total == 0) { free(accum); return API_INTERNAL_ERROR; }

    // Refresh cache + reply. Cache write is mutex-guarded so concurrent
    // callers see a consistent buffer.
    xSemaphoreTake(s_devlist_cache_mutex, portMAX_DELAY);
    const size_t cache_n = (total < kDevListAccumCap) ? total : kDevListAccumCap - 1;
    memcpy(s_devlist_cache_buf, accum, cache_n);
    s_devlist_cache_buf[cache_n] = '\0';
    s_devlist_cache_len  = cache_n;
    s_devlist_last_ok_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_devlist_cache_mutex);

    size_t n = (total >= rsp_cap) ? rsp_cap - 1 : total;
    if (total >= rsp_cap) {
        ESP_LOGW(TAG_API, "device.list %u B exceeds caller cap %u — truncated",
                 (unsigned)total, (unsigned)rsp_cap);
    }
    memcpy(rsp_buf, accum, n);
    rsp_buf[n] = '\0';
    free(accum);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// GET /api/devices/{ieee}[/options] — expects args like
// {"ieee":"0x...","sub":"options"} (sub is optional).
extern "C" ApiStatus api_device_get(const char* body, size_t body_len,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    const char* sub = doc["sub"] | (const char*)nullptr;
    ESP_LOGI(TAG_API, "device.get ieee=%s sub=%s", ieee_str, sub ? sub : "(none)");

    if (sub && strcmp(sub, "options") == 0) {
        char nvs_key[16];
        snprintf(nvs_key, sizeof(nvs_key), "%014llx",
                 (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));
        char blob[256] = "{}";
        if (s_nvs_zhac_opt) {
            size_t sz = sizeof(blob) - 1;
            nvs_get_str(s_nvs_zhac_opt, nvs_key, blob, &sz);
        }
        size_t n = strlen(blob);
        if (n >= rsp_cap) n = rsp_cap - 1;
        memcpy(rsp_buf, blob, n);
        rsp_buf[n] = '\0';
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }

    uint8_t req_buf[48];
    uint16_t req_len = 0;
    hap_json_encode_get_device_req(req_buf, sizeof(req_buf), &req_len, ieee);

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kDevInfoRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::GET_DEVICE_BY_ID, req_buf, req_len,
                           rsp.get(), kDevInfoRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }
    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// POST /api/devices/{ieee}/{bind|unbind}
// args: original body merged with {"ieee":"..." , "unbind":bool}
extern "C" ApiStatus api_device_bind(const char* body, size_t body_len,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t src_ieee = parse_ieee(ieee_str);
    if (src_ieee == 0) return API_BAD_REQUEST;
    bool is_unbind = doc["unbind"] | false;

    HapBindReq bind{};
    bind.src_ieee = src_ieee;
    bind.src_ep   = doc["src_ep"]  | (uint8_t)0;
    bind.cluster  = doc["cluster"] | (uint16_t)0;
    bind.dst_ieee = parse_ieee(doc["dst_ieee"] | (const char*)nullptr);
    bind.dst_ep   = doc["dst_ep"]  | (uint8_t)0;
    bind.unbind   = is_unbind;

    uint8_t hap_buf[128];
    uint16_t hap_len = 0;
    if (!hap_json_encode_bind_req(hap_buf, sizeof(hap_buf), &hap_len, bind)) {
        return API_INTERNAL_ERROR;
    }

    char ack_buf[64];
    size_t ack_len = 0;
    bool got_rsp = hap_roundtrip_v2(HapMsgType::BIND_REQ, hap_buf, hap_len,
                                     ack_buf, sizeof(ack_buf), &ack_len, 5000);
    bool cmd_ok = false;
    if (got_rsp && ack_len > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, ack_buf, ack_len) == DeserializationError::Ok) {
            cmd_ok = doc["ok"] | false;
        }
    }
    const bool ok = got_rsp && cmd_ok;

    const size_t n = ok ? api_write_ok(rsp_buf, rsp_cap)
                        : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// DELETE /api/devices/{ieee} — args: {"ieee":"...", "hard":bool}
extern "C" ApiStatus api_device_delete(const char* body, size_t body_len,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;
    bool hard = doc["hard"] | false;

    uint8_t hap_buf[64];
    uint16_t hap_len = 0;
    if (!hap_json_encode_device_delete(hap_buf, sizeof(hap_buf), &hap_len,
                                         ieee, hard)) {
        return API_INTERNAL_ERROR;
    }

    char ack_buf[64];
    size_t ack_len = 0;
    bool got_rsp = hap_roundtrip_v2(HapMsgType::DEVICE_DELETE, hap_buf, hap_len,
                                     ack_buf, sizeof(ack_buf), &ack_len, 5000);
    bool cmd_ok = false;
    if (got_rsp && ack_len > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, ack_buf, ack_len) == DeserializationError::Ok) {
            cmd_ok = doc["ok"] | false;
        }
    }
    bool ok = got_rsp && cmd_ok;

    if (ok) {
        char ws_msg[64];
        snprintf(ws_msg, sizeof(ws_msg),
                 "{\"event\":\"device_deleted\",\"ieee\":\"0x%016llX\"}",
                 (unsigned long long)ieee);
        // Via the WS-TX worker queue — ws_server_broadcast is reserved for
        // the worker itself (single-caller invariant, see s3_internal.h).
        ws_bridge_broadcast_enqueue(ws_msg, strlen(ws_msg));
    }

    const size_t n = ok ? api_write_ok(rsp_buf, rsp_cap)
                        : api_write_err(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// Parse the IEEE out of the body and return the surviving doc.
// Returns API_BAD_REQUEST via `status_out` when anything is missing.
static ApiStatus parse_ieee_body(const char* body, size_t body_len,
                                  JsonDocument& doc, uint64_t& ieee_out) {
    if (body_len == 0) return API_BAD_REQUEST;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;
    ieee_out = ieee;
    return API_OK;
}

// PUT /api/devices/{ieee}        — WS `device.rename` — args {ieee, name}.
extern "C" ApiStatus api_device_rename(const char* body, size_t body_len,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;

    const char* name_str = doc["name"] | (const char*)nullptr;
    if (!name_str || name_str[0] == '\0') return API_BAD_REQUEST;
    char name[30];
    // UTF-8-safe: a byte-blind strncpy could split a multibyte char at this
    // bound; the poisoned name would persist on the P4 and turn every
    // device.list page into an invalid-UTF-8 WS text frame (socket-killer,
    // same failure as the group.list name bug).
    utf8_safe_copy(name, sizeof(name), name_str);

    uint8_t hap_buf[80];
    uint16_t hap_len = 0;
    if (!hap_json_encode_device_set_name(hap_buf, sizeof(hap_buf), &hap_len,
                                           ieee, name)) {
        return API_INTERNAL_ERROR;
    }

    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kDevInfoRspCap]);
    if (!rsp) return API_INTERNAL_ERROR;
    size_t got = 0;
    if (!hap_roundtrip_v2(HapMsgType::DEVICE_SET_NAME, hap_buf, hap_len,
                           rsp.get(), kDevInfoRspCap, &got, 5000)) {
        return API_INTERNAL_ERROR;
    }

    // Task 18: keep an exposed RainMaker device's in-app display name in
    // sync with a ZHAC-side rename, so the phone app doesn't show a stale
    // name until the next reboot/re-expose. Cheap + safe to call
    // unconditionally (no-op if ieee isn't exposed or the flag is off) —
    // same "always call, flag-off stub is a no-op" convention as
    // rmk_bridge_on_device_gone(), already wired at the DEVICE_LEAVE site
    // (Task 16) and NOT duplicated here. `name` is already UTF-8-sanitized
    // above (utf8_safe_copy).
    rmk_bridge_on_device_renamed(ieee, name);

    size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
    memcpy(rsp_buf, rsp.get(), n);
    rsp_buf[n] = '\0';
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// Core of `device.attr.set` — extracted from api_device_attr_set (Phase 4
// refactor, Task 15) so a non-JSON caller (Task 17's RainMaker
// param-write path) can drive the identical SET_ATTRIBUTE→P4 pipeline.
// Builds the exact same HapSetAttrReq{ieee,ep,cluster,attr,val,key} the
// JSON handler used to build inline, runs the same 3000 ms
// SET_ATTRIBUTE roundtrip, and decodes the ack's "ok" field the same way.
//
// Does NOT touch a reply buffer — encoding {"ok":true} /
// {"ok":false,"err":"..."} stays the caller's job; before this
// extraction that encoding was simply the tail of api_device_attr_set
// itself.
//
// Returns API_INTERNAL_ERROR, with *cmd_ok_out left untouched, if the
// request failed to encode or the P4 roundtrip failed/timed out — this
// mirrors api_device_attr_set's original early-return paths, which
// produced no reply body either. Returns API_OK once an ack was
// received; *cmd_ok_out then reports whether the device accepted the
// command (false covers both an explicit ack "ok":false and an
// unparsable/empty ack — same fallback the original code used).
//
// `key` must already be validated to fit HapSetAttrReq::key (<=23 chars,
// NUL-terminated); this function does not re-check — the JSON handler's
// parse/validate step still owns that bound check.
extern "C" ApiStatus device_attr_set_core(uint64_t ieee, const char* key,
                                           int32_t val, uint8_t ep,
                                           uint16_t cluster, uint16_t attr,
                                           bool* cmd_ok_out) {
    HapSetAttrReq req{};
    req.ieee    = ieee;
    req.ep      = ep;
    req.cluster = cluster;
    req.attr    = attr;
    req.val     = val;
    strncpy(req.key, key, sizeof(req.key) - 1);

    uint8_t hap_buf[160];
    uint16_t hap_len = 0;
    if (!hap_json_encode_set_attr(hap_buf, sizeof(hap_buf), &hap_len, req)) {
        return API_INTERNAL_ERROR;
    }

    char ack_buf[64];
    size_t ack_len = 0;
    bool got_rsp = hap_roundtrip_v2(HapMsgType::SET_ATTRIBUTE,
                                     hap_buf, hap_len,
                                     ack_buf, sizeof(ack_buf), &ack_len, 3000);
    if (!got_rsp) return API_INTERNAL_ERROR;
    bool cmd_ok = false;
    if (ack_len > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, ack_buf, ack_len) == DeserializationError::Ok) {
            cmd_ok = doc["ok"] | false;
        }
    }
    *cmd_ok_out = cmd_ok;
    return API_OK;
}

// WS `device.attr.set` — args {ieee, key, value, ep?, cluster?, attr?}.
// `value` may be bool / integer / string; boolean/integer round-trip
// through HapSetAttrReq.val directly. String values are numeric-parsed
// (enum-name strings need a follow-up — HapSetAttrReq has no str field
// today and the SET_ATTRIBUTE wire path is int-only). `val` is accepted
// as a legacy alias for `value` so existing REST callers keep working.
//
// Post-parse behavior (build SET_ATTRIBUTE, HAP roundtrip, ack decode)
// lives in device_attr_set_core (Phase 4 extraction, Task 15) — this
// handler now only parses/validates the body and encodes the reply.
extern "C" ApiStatus api_device_attr_set(const char* body, size_t body_len,
                                          char* rsp_buf, size_t rsp_cap,
                                          size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;

    HapSetAttrReq attr{};
    attr.ieee = ieee;
    const char* key_str = doc["key"] | (const char*)nullptr;
    if (!key_str || key_str[0] == '\0') return API_BAD_REQUEST;
    if (strlen(key_str) >= sizeof(attr.key)) return API_BAD_REQUEST;
    strncpy(attr.key, key_str, sizeof(attr.key) - 1);

    // Accept `value` (SPA) or `val` (legacy REST) as aliases.
    JsonVariantConst vv = doc["value"];
    if (vv.isNull()) vv = doc["val"];
    if (vv.is<bool>())              attr.val = vv.as<bool>() ? 1 : 0;
    else if (vv.is<int>())          attr.val = vv.as<int32_t>();
    else if (vv.is<float>())        attr.val = (int32_t)vv.as<float>();
    else if (vv.is<const char*>())  attr.val = atoi(vv.as<const char*>());
    else                            attr.val = 0;

    attr.ep      = doc["ep"]      | (uint8_t)0;
    attr.cluster = doc["cluster"] | (uint16_t)0;
    attr.attr    = doc["attr"]    | (uint16_t)0;

    bool cmd_ok = false;
    ApiStatus core_st = device_attr_set_core(attr.ieee, attr.key, attr.val,
                                              attr.ep, attr.cluster, attr.attr,
                                              &cmd_ok);
    if (core_st != API_OK) return core_st;

    if (cmd_ok) {
        const size_t n = api_write_ok(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    int n = snprintf(rsp_buf, rsp_cap, "%s",
                     "{\"ok\":false,\"err\":\"command failed\"}");
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

// WS `device.options.set` — args {ieee, occupancy_timeout?, debounce_ms?,
// flood_protection?, throttle_ms?}. Stores the body to NVS verbatim; P4 is notified
// via DEVICE_OPTIONS_SET only when one of the forwarded fields is set.
// The REST wrapper passes the raw original body under `__raw` so the
// stored NVS blob stays byte-identical to the pre-migration handler.
extern "C" ApiStatus api_device_options_set(const char* body, size_t body_len,
                                             char* rsp_buf, size_t rsp_cap,
                                             size_t* rsp_len) {
    JsonDocument doc;
    uint64_t ieee = 0;
    ApiStatus st = parse_ieee_body(body, body_len, doc, ieee);
    if (st != API_OK) return st;
    if (body[0] != '{') return API_BAD_REQUEST;

    char nvs_key[16];
    snprintf(nvs_key, sizeof(nvs_key), "%014llx",
             (unsigned long long)(ieee & 0x00FFFFFFFFFFFFFFULL));

    const char* raw_body = doc["__raw"] | (const char*)nullptr;
    const char* write_str = (raw_body && raw_body[0]) ? raw_body : body;

    // Stage the write and defer the flash commit (debounced) — see
    // opt_stage_and_defer. The value is immediately visible to the next read
    // on this cached handle; durability follows on the quiet-window timer or
    // the reboot flush hook. `nvs_ok` reflects the stage succeeding.
    bool nvs_ok = false;
    if (s_nvs_zhac_opt) {
        nvs_ok = (opt_stage_and_defer(nvs_key, write_str) == ESP_OK);
    }

    bool p4_ok     = true;
    bool forwarded = false;
    int32_t  occ_raw = -1;
    int32_t  deb_raw = -1;
    int32_t  thr_raw = -1;
    const int32_t* occ = nullptr;
    const int32_t* deb = nullptr;
    const int32_t* thr = nullptr;
    if (doc["occupancy_timeout"].is<int>()) {
        occ_raw = doc["occupancy_timeout"].as<int32_t>();
        occ = &occ_raw;
    }
    if (doc["debounce_ms"].is<int>()) {
        deb_raw = doc["debounce_ms"].as<int32_t>();
        deb = &deb_raw;
    } else if (doc["flood_protection"].is<bool>()) {
        deb_raw = doc["flood_protection"].as<bool>() ? 500 : 0;
        deb = &deb_raw;
    }
    if (doc["throttle_ms"].is<int>()) {
        thr_raw = doc["throttle_ms"].as<int32_t>();
        thr = &thr_raw;
    }

    if (occ || deb || thr) {
        uint8_t hap_buf[160];   // 3 optional fields + ieee; matches SET_ATTRIBUTE
        uint16_t hap_len = 0;
        if (hap_json_encode_device_options_set(hap_buf, sizeof(hap_buf),
                                                &hap_len, ieee, occ, deb, thr)) {
            char ack_buf[64];
            size_t ack_len = 0;
            bool got_rsp = hap_roundtrip_v2(HapMsgType::DEVICE_OPTIONS_SET,
                                             hap_buf, hap_len,
                                             ack_buf, sizeof(ack_buf),
                                             &ack_len, 5000);
            bool cmd_ok = false;
            if (got_rsp && ack_len > 0) {
                JsonDocument ad;
                if (deserializeJson(ad, ack_buf, ack_len) == DeserializationError::Ok) {
                    cmd_ok = ad["ok"] | false;
                }
            }
            p4_ok     = got_rsp && cmd_ok;
            forwarded = true;
        } else {
            // Encode failure (e.g. payload would overflow hap_buf) must not
            // masquerade as success — fail the request rather than fall
            // through to the !forwarded "ok" path below.
            p4_ok = false;
        }
    }

    const bool ok = nvs_ok && p4_ok;
    if (!ok) {
        const size_t n = api_write_err(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    if (!forwarded) {
        const size_t n = api_write_ok(rsp_buf, rsp_cap);
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    int n = snprintf(rsp_buf, rsp_cap, "%s", "{\"ok\":true,\"applied\":true}");
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_device_reinterview(const char* body, size_t body_len,
                                             char* rsp_buf, size_t rsp_cap,
                                             size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;

    uint8_t hap_buf[48];
    uint16_t hap_len = 0;
    if (!hap_json_encode_interview_req(hap_buf, sizeof(hap_buf), &hap_len, ieee)) {
        int n = snprintf(rsp_buf, rsp_cap,
                         "{\"ok\":false,\"err\":\"encode failed\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }
    hap_send(HapMsgType::INTERVIEW_REQ, hap_buf, hap_len);
    ESP_LOGI(TAG_API, "interview re-triggered for 0x%016llx",
             (unsigned long long)ieee);
    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// CONFIGURE_REQ — fire-and-forget. Payload identical to INTERVIEW_REQ
// (just IEEE), so reuse the same hap_json encoder; only the HAP msg
// type differs. P4 handler re-runs `zhac_adapter_configure` against
// the cached (model_id, manufacturer_name) and skips the full
// interview. Use after firmware adds new `reports[]` / `config_steps[]`
// to a def for a device that's already paired (ZG-204Z's read-on-join
// for sensitivity / keep_time is the original motivating case).
extern "C" ApiStatus api_device_configure(const char* body, size_t body_len,
                                           char* rsp_buf, size_t rsp_cap,
                                           size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ieee_str = doc["ieee"] | (const char*)nullptr;
    if (!ieee_str || ieee_str[0] == '\0') return API_BAD_REQUEST;
    uint64_t ieee = parse_ieee(ieee_str);
    if (ieee == 0) return API_BAD_REQUEST;

    uint8_t hap_buf[48];
    uint16_t hap_len = 0;
    if (!hap_json_encode_interview_req(hap_buf, sizeof(hap_buf), &hap_len, ieee)) {
        int n = snprintf(rsp_buf, rsp_cap,
                         "{\"ok\":false,\"err\":\"encode failed\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }
    hap_send(HapMsgType::CONFIGURE_REQ, hap_buf, hap_len);
    ESP_LOGI(TAG_API, "configure re-fired for 0x%016llx",
             (unsigned long long)ieee);
    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

