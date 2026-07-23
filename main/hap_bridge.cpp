// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "s3_internal.h"
#include "esp_random.h"
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // unlink (drop partial alerts.bin on write failure)
#include <algorithm>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"   // esp_app_get_description()->version (S3 FW version)
#include "hap_master.h"
#include "hap_session.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ArduinoJson.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "log_ring.h"
#include "api_handlers.h"
#include "metrics/metrics_macros.h"
#include "task_stacks.h"
#include "rmk_bridge.h"   // Task 16 fix: BULK_STATE_UPDATE is the real S3
                          // fan-out for device state (device_shadow/
                          // event_bus are never fed on this target — see
                          // rmk_bridge.c's file banner). rainmaker_gw
                          // is already a REQUIRES of this "main" component
                          // (main.cpp includes rainmaker_gw.h), so
                          // rmk_bridge.h — same component, same
                          // INCLUDE_DIRS — needs no new CMake dependency.
#include <cmath>          // lroundf() — VAL_FLOAT reconstruction below

static const char* TAG = "hap_bridge";

// Telegram gateway handler forward declarations
extern "C" void tg_gw_handle_settoken(const uint8_t*, uint16_t);
extern "C" void tg_gw_handle_setchat (const uint8_t*, uint16_t);
extern "C" void tg_gw_handle_send    (const uint8_t*, uint16_t);

// Task 21 fix: rmk_bridge_on_device_gone() (called from DEVICE_LEAVE below)
// only cleans up components/rainmaker_gw's in-memory registry — it cannot
// touch main/api_rainmaker.cpp's persisted NVS exposed-set (g_table), which
// lives in a different translation unit that the component layer must not
// depend on. Without this, a device unpaired while exposed to RainMaker
// stayed in the persisted set forever. Same forward-declare-at-call-site
// pattern main.cpp already uses for rmk_boot_restore().
extern "C" void rmk_on_device_gone(uint64_t ieee);   // main/api_rainmaker.cpp


// Heartbeat liveness tracking. Updated by the on_frame HEARTBEAT branch,
// polled by task_hap. Three consecutive missed intervals (15 s by default)
// flag the P4 as unresponsive. Flag clears on next received heartbeat.
static std::atomic<TickType_t> s_last_p4_hb_tick{0};
static std::atomic<bool>       s_p4_unresponsive{false};
bool hap_bridge_is_p4_unresponsive() {
    return s_p4_unresponsive.load(std::memory_order_relaxed);
}

// Cached P4 Prometheus snapshot (served via HapMsgType::METRICS_RSP).
// Fetched asynchronously from the bridge loop every
// HAP_METRICS_REFRESH_MS. Readers (rest_ops /metrics handler) copy
// under a mutex; writers (HAP frame handler) do the same.
static constexpr TickType_t HAP_METRICS_REFRESH_MS = 30000;
EXT_RAM_BSS_ATTR static char s_p4_metrics_buf[HAP_MAX_PAYLOAD];
static uint16_t          s_p4_metrics_len = 0;
static SemaphoreHandle_t s_p4_metrics_mutex = nullptr;

size_t hap_bridge_copy_p4_metrics(char* out, size_t max) {
    if (!s_p4_metrics_mutex || !out || max == 0) return 0;
    xSemaphoreTake(s_p4_metrics_mutex, portMAX_DELAY);
    const size_t n = (s_p4_metrics_len < max - 1) ? s_p4_metrics_len : max - 1;
    memcpy(out, s_p4_metrics_buf, n);
    out[n] = '\0';
    xSemaphoreGive(s_p4_metrics_mutex);
    return n;
}

void hap_bridge_copy_p4_fw_ver(char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!s_p4_metrics_mutex) return;
    xSemaphoreTake(s_p4_metrics_mutex, portMAX_DELAY);
    const size_t n = strnlen(s_p4_fw_ver, sizeof(s_p4_fw_ver));
    const size_t copy = (n < cap - 1) ? n : cap - 1;
    memcpy(out, s_p4_fw_ver, copy);
    out[copy] = '\0';
    xSemaphoreGive(s_p4_metrics_mutex);
}

static void task_alert_persist(void*) {
    // Heap-allocate snapshot buffer — stack can't hold 32 × ~100 B safely
    auto* snap = static_cast<AlertLogEntry*>(heap_caps_malloc(sizeof(AlertLogEntry) * ALERT_LOG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    configASSERT(snap);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Snapshot under mutex, then write outside the critical section
        uint8_t head, count;
        xSemaphoreTake(s_alert_log_mutex, portMAX_DELAY);
        memcpy(snap, s_alert_log, sizeof(AlertLogEntry) * ALERT_LOG_MAX);
        head  = s_alert_log_head;
        count = s_alert_log_count;
        xSemaphoreGive(s_alert_log_mutex);

        FILE* f = fopen("/spiffs/alerts.bin", "wb");
        if (!f) { ESP_LOGW(TAG, "alert_log_persist: fopen failed"); continue; }
        // Check every write + the close: a short write (SPIFFS full) or a
        // close-time flush error would otherwise leave a truncated alerts.bin
        // that alert_log_load() reads back as a corrupt ring. On any failure
        // unlink the partial file so the next boot just starts empty.
        bool ok = fwrite(&head,  1, 1, f) == 1
               && fwrite(&count, 1, 1, f) == 1
               && fwrite(snap, sizeof(AlertLogEntry), ALERT_LOG_MAX, f) == ALERT_LOG_MAX;
        if (fclose(f) != 0) ok = false;
        if (!ok) {
            ESP_LOGW(TAG, "alert_log_persist: write/close failed — dropping partial file");
            unlink("/spiffs/alerts.bin");
            continue;
        }
        ESP_LOGD(TAG, "alert_log persisted count=%u", count);
    }
}

void alert_log_schedule_persist() {
    TaskHandle_t h = xTaskGetHandle("TaskAlertPrst");
    if (h) xTaskNotifyGive(h);
}

void alert_log_load() {
    FILE* f = fopen("/spiffs/alerts.bin", "rb");
    if (!f) return;  // first boot, nothing to load

    uint8_t head = 0, count = 0;
    // Read directly into s_alert_log (BSS) — local array would overflow main task stack (3584 B)
    xSemaphoreTake(s_alert_log_mutex, portMAX_DELAY);
    bool ok = fread(&head,        1, 1,             f) == 1 &&
              fread(&count,       1, 1,             f) == 1 &&
              fread(s_alert_log,  sizeof(AlertLogEntry), ALERT_LOG_MAX, f) == ALERT_LOG_MAX;
    if (ok) {
        s_alert_log_head  = head;
        s_alert_log_count = count;
    } else {
        memset(s_alert_log, 0, sizeof(s_alert_log));
    }
    xSemaphoreGive(s_alert_log_mutex);
    fclose(f);

    if (!ok) { ESP_LOGW(TAG, "alert_log_load: corrupt file"); return; }
    ESP_LOGI(TAG, "alert_log loaded: %u entr%s", count, count == 1 ? "y" : "ies");
}

void alert_persist_task_init() {
    xTaskCreate(task_alert_persist, "TaskAlertPrst", zhac::stack::kAlertPersist, nullptr, 1, nullptr);
}

void hap_send(HapMsgType type, const uint8_t* payload, uint16_t payload_len,
              uint8_t flags) {
    HapFrame f{};
    f.type        = type;
    f.seq         = hap_session_next_seq();
    f.flags       = flags;
    f.payload     = payload;
    f.payload_len = payload_len;
    hap_session_send(f);
}

// ── Per-request-seq response correlation (F-01 v2) ──────────────────────
//
// Each in-flight roundtrip claims one slot from `s_waiters`. The slot
// records the request seq, the expected response type, and the caller's
// own response buffer + done-sem. When the receiver decodes a frame with
// `ack_seq != 0` we look up the matching slot by seq; on match we copy
// the payload into the caller's buffer (truncating to its cap) and give
// the sem. No shared-buffer copy-out, no per-type single-slot table, no
// REST-side mutex serialising rule/script mutators — concurrent rule
// edits proceed in parallel up to kHapWaiterCount in flight.
//
// kHapWaiterCount is sized for the steady-state concurrency cap. The
// esp_http_server runs a SINGLE worker task (HTTPD_DEFAULT_CONFIG, no
// thread-pool override), so REST requests are serialised one at a time;
// the WS dispatcher adds 1 and the OTA flow holds at most 1, leaving ample
// headroom. Counting-sem `s_waiter_free_sem` gates claim so the (N+1)th
// caller blocks until a slot frees.
struct HapWaiter {
    uint16_t           seq;            // 0 = free
    HapMsgType         rsp_type;
    SemaphoreHandle_t  sem;
    char*              rsp_buf;
    size_t             rsp_cap;
    size_t*            rsp_len_out;
    bool               ok;
};
static constexpr size_t  kHapWaiterCount = 8;
static HapWaiter         s_waiters[kHapWaiterCount];
static SemaphoreHandle_t s_waiter_table_mutex = nullptr;
static SemaphoreHandle_t s_waiter_free_sem    = nullptr;

// Data requests are sent HAP_FLAG_NO_ACK, so the window/retransmit-based
// on_link_dead never fires for them. Track consecutive roundtrip timeouts as
// an independent data-link liveness signal: a streak ⇒ presume the link dead
// and force a re-SYNC (see hap_roundtrip_v2 tail).
static std::atomic<uint8_t>   s_roundtrip_dead_streak{0};
static constexpr uint8_t      HAP_ROUNDTRIP_DEAD_STREAK = 3; // consecutive timeouts ⇒ link presumed dead

// Number of distinct seqs we will try before giving up when every candidate
// collides with an in-flight waiter. With only kHapWaiterCount (8) slots ever
// active, a collision is already vanishingly rare; 8 tries makes exhaustion
// effectively impossible while staying bounded.
static constexpr int kWaiterClaimMaxTries = 8;

// Claim one free slot under the table mutex AND assign it a seq that is not
// already in use by any active waiter. Caller must already have decremented
// s_waiter_free_sem so a free slot exists.
//
// Defect 1 (FINDINGS §1.1): the seq is a uint16. After 65534 sends the
// hap_session counter wraps and can hand out a value that a long-stalled
// in-flight waiter still holds. If two waiters share a seq, waiter_find_by_seq
// matches the OLDER one and delivers caller A's response into caller B's
// buffer. Drawing the seq UNDER the table mutex and rejecting any value that
// collides with a live slot closes the window: a fresh seq cannot alias an
// active waiter. hap_session_next_seq() is the seq source and already skips 0,
// so every candidate here is a valid on-wire seq; we re-check the skip-0
// invariant defensively. On exhaustion we fail the claim (caller bails and
// returns the free-sem) rather than risk a colliding correlation.
static HapWaiter* waiter_claim(HapMsgType rsp_type,
                                char* rsp_buf, size_t rsp_cap,
                                size_t* rsp_len_out,
                                uint16_t* seq_out) {
    xSemaphoreTake(s_waiter_table_mutex, portMAX_DELAY);
    HapWaiter* w = nullptr;
    for (auto& s : s_waiters) {
        if (s.seq == 0) { w = &s; break; }
    }
    if (!w) {
        xSemaphoreGive(s_waiter_table_mutex);
        return nullptr;
    }

    uint16_t seq = 0;
    bool seq_ok  = false;
    for (int attempt = 0; attempt < kWaiterClaimMaxTries; ++attempt) {
        const uint16_t cand = hap_session_next_seq();
        if (cand == 0) continue;   // skip-0 convention (matches next_seq)
        bool collides = false;
        for (auto& s : s_waiters) {
            if (s.seq == cand) { collides = true; break; }
        }
        if (!collides) { seq = cand; seq_ok = true; break; }
    }
    if (!seq_ok) {
        // Every candidate collided with a live slot (or was 0). Do NOT claim
        // the slot — leave it free and signal failure so the caller returns
        // the free-sem and reports a clean error instead of risking a
        // cross-delivered correlation.
        xSemaphoreGive(s_waiter_table_mutex);
        return nullptr;
    }

    w->seq         = seq;
    w->rsp_type    = rsp_type;
    w->rsp_buf     = rsp_buf;
    w->rsp_cap     = rsp_cap;
    w->rsp_len_out = rsp_len_out;
    w->ok          = false;
    xSemaphoreTake(w->sem, 0);   // drain any stale signal from prior use
    *seq_out = seq;
    xSemaphoreGive(s_waiter_table_mutex);
    return w;
}

// Defect 2 (FINDINGS §1.2): atomic response delivery.
//
// The frame callback (task_hap) previously found the waiter under the mutex,
// RELEASED the mutex, then memcpy'd into w->rsp_buf, wrote *rsp_len_out, and
// gave the sem — all unlocked. If the caller had already timed out and called
// waiter_release() in that gap, the slot could be reclaimed and re-pointed at a
// DIFFERENT caller's buffer, so the memcpy landed in the wrong buffer (and the
// sem-give woke the wrong caller). Two reviewers flagged this independently.
//
// Fix: find + re-check + memcpy + len-write + sem-give all happen UNDER the
// table mutex as one indivisible step. A timed-out caller's waiter_release()
// (also mutex-guarded) cannot interleave: either it ran first (seq==0 ⇒ no
// match here, we drop the late frame) or it is blocked until we finish (in
// which case we delivered into the still-valid slot before it freed it). The
// memcpy is bounded (≤ rsp_cap ≤ HAP_MAX_PAYLOAD) and non-blocking, so holding
// the mutex across it is safe — we never block while holding it.
//
// Returns true if a matching, still-waiting slot was found and delivered.
static bool waiter_deliver(uint16_t ack_seq, HapMsgType rsp_type,
                           const uint8_t* payload, uint16_t payload_len) {
    if (ack_seq == 0) return false;
    xSemaphoreTake(s_waiter_table_mutex, portMAX_DELAY);
    HapWaiter* w = nullptr;
    for (auto& s : s_waiters) {
        // Re-check seq AND in-use (seq!=0): a released slot has seq==0 and is
        // skipped, so a late frame for a timed-out caller cannot be written.
        if (s.seq != 0 && s.seq == ack_seq) { w = &s; break; }
    }
    bool delivered = false;
    if (w) {
        if (rsp_type == w->rsp_type) {
            const size_t n =
                std::min(static_cast<size_t>(payload_len), w->rsp_cap);
            if (n > 0 && payload) memcpy(w->rsp_buf, payload, n);
            if (n < payload_len) {
                ESP_LOGW(TAG,
                         "hap_roundtrip_v2: truncated rsp type=0x%02x %u->%zu",
                         static_cast<uint8_t>(rsp_type), payload_len, n);
            }
            *w->rsp_len_out = n;
            w->ok = true;
        } else {
            ESP_LOGW(TAG,
                     "hap_roundtrip_v2: type mismatch seq=%u got=0x%02x want=0x%02x",
                     ack_seq, static_cast<uint8_t>(rsp_type),
                     static_cast<uint8_t>(w->rsp_type));
        }
        // Give the sem INSIDE the lock so the wake is atomic w.r.t. the buffer
        // write and the release: the caller cannot observe a half-written
        // buffer, and a concurrent waiter_release sees a coherent slot.
        xSemaphoreGive(w->sem);
        delivered = true;
    }
    xSemaphoreGive(s_waiter_table_mutex);
    return delivered;
}

static void waiter_release(HapWaiter* w) {
    if (!w) return;
    xSemaphoreTake(s_waiter_table_mutex, portMAX_DELAY);
    w->seq = 0;
    xSemaphoreGive(s_waiter_table_mutex);
    xSemaphoreGive(s_waiter_free_sem);
}

static HapMsgType expected_response_for(HapMsgType req) {
    switch (req) {
        case HapMsgType::GET_DEVICES:       return HapMsgType::DEVICE_LIST;
        case HapMsgType::GET_DEVICE_BY_ID:
        case HapMsgType::DEVICE_SET_NAME:   return HapMsgType::DEVICE_INFO;
        case HapMsgType::DIAG_UNHANDLED_REQ: return HapMsgType::DIAG_UNHANDLED_RSP;
        case HapMsgType::BIND_REQ:          return HapMsgType::BIND_ACK;
        case HapMsgType::DEVICE_DELETE:     return HapMsgType::DEVICE_DELETE_ACK;
        case HapMsgType::DEVICE_OPTIONS_SET:return HapMsgType::DEVICE_OPTIONS_SET_ACK;
        case HapMsgType::ZIGBEE_CFG_SET:    return HapMsgType::ZIGBEE_CFG_SET_ACK;
        case HapMsgType::SET_ATTRIBUTE:     return HapMsgType::SET_ACK;
        case HapMsgType::GROUP_MEMBER_QUERY: return HapMsgType::GROUP_MEMBER_LIST;
        case HapMsgType::RULE_CREATE:
        case HapMsgType::RULE_UPDATE:
        case HapMsgType::RULE_UPDATE_DSL:
        case HapMsgType::RULE_DELETE:       return HapMsgType::RULE_EXEC_RESULT;
        case HapMsgType::RULE_LIST_REQ:     return HapMsgType::RULE_LIST_RSP;
        case HapMsgType::SCRIPT_WRITE:
        case HapMsgType::SCRIPT_DELETE:     return HapMsgType::SCRIPT_ACK;
        case HapMsgType::SCRIPT_LIST_REQ:   return HapMsgType::SCRIPT_LIST_RSP;
        case HapMsgType::SCRIPT_READ_REQ:   return HapMsgType::SCRIPT_READ_RSP;
        case HapMsgType::SCRIPT_CHECK_REQ:  return HapMsgType::SCRIPT_CHECK_RSP;
        case HapMsgType::OTA_CHECKPOINT_REQ:return HapMsgType::OTA_CHECKPOINT_RSP;
        default:                            return static_cast<HapMsgType>(0);
    }
}

bool hap_roundtrip_v2(HapMsgType type,
                      const uint8_t* req, uint16_t req_len,
                      char* rsp_buf, size_t rsp_cap, size_t* rsp_len_out,
                      uint32_t timeout_ms) {
    if (!rsp_buf || rsp_cap == 0 || !rsp_len_out) return false;
    *rsp_len_out = 0;

    const HapMsgType rsp_type = expected_response_for(type);
    if (static_cast<uint8_t>(rsp_type) == 0) {
        ESP_LOGE(TAG, "hap_roundtrip_v2: no rsp_type for req=0x%02x",
                 static_cast<uint8_t>(type));
        return false;
    }

    // Counting-sem decrement is the slot-availability gate. Use the same
    // timeout the caller gave us so high-load blocking respects the SLA.
    if (xSemaphoreTake(s_waiter_free_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "hap_roundtrip_v2: no free waiter slot (req=0x%02x)",
                 static_cast<uint8_t>(type));
        return false;
    }

    uint16_t seq = 0;
    HapWaiter* w = waiter_claim(rsp_type, rsp_buf, rsp_cap, rsp_len_out, &seq);
    if (!w) {
        // Either the table state diverged from the sem count, or every seq
        // candidate collided with a live waiter (Defect 1 exhaustion — should
        // be impossible with 8 slots). Either way give the sem back and bail
        // with a clean error rather than risk a colliding correlation.
        ESP_LOGE(TAG, "hap_roundtrip_v2: waiter_claim failed (req=0x%02x)",
                 static_cast<uint8_t>(type));
        xSemaphoreGive(s_waiter_free_sem);
        return false;
    }

    HapFrame f{};
    f.type        = type;
    f.seq         = seq;
    // NEEDS_ACK (was NO_ACK): a lost request frame used to be completely
    // silent — no retransmit, no log on either side, only this waiter's
    // timeout after the full 5 s (the P4 never saw the request, so its log
    // shows nothing). Once SHADOW_OPTIMISTIC forwarding raised the ambient
    // SPI traffic, that silent loss became user-visible as sporadic
    // device.list 500s blocking the SPA login gate. With NEEDS_ACK the
    // session retransmits at 1 s up to 5×, and the peer's seen-ring dedups
    // a duplicate delivery — a single lost frame now costs ~1 s, not a
    // failed roundtrip.
    f.flags       = HAP_FLAG_NEEDS_ACK;
    f.payload     = req;
    f.payload_len = req_len;
    if (!hap_session_send(f)) {
        // Retransmit window full — local congestion, not link evidence
        // (don't bump the dead-link streak). Fail fast so the caller can
        // retry instead of burning the full roundtrip timeout on a frame
        // that was never queued.
        waiter_release(w);
        return false;
    }

    const bool got = xSemaphoreTake(w->sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    const bool ok  = got && w->ok;
    waiter_release(w);
    if (got) {
        // A reply arrived ⇒ the data link is alive; clear the dead-link streak.
        s_roundtrip_dead_streak.store(0, std::memory_order_release);
    } else {
        ESP_LOGW(TAG, "hap_roundtrip_v2 timeout type=0x%02x seq=%u after %" PRIu32 "ms",
                 static_cast<uint8_t>(type), seq, timeout_ms);
        // Requests are NEEDS_ACK now, so a truly dead link also surfaces via the
        // session's retransmit-exhaust on_link_dead. This streak heuristic stays as
        // belt-and-braces for the half-dead case it was built for (P4 tore its session
        // down and is awaiting SYNC: it ACKs nothing, heartbeats may still flow).
        // Force a re-SYNC to recover — clearing s_synced makes the hap_task loop
        // resend SYNC_REQ. exchange() so we only act on the true→false edge.
        const uint8_t streak = static_cast<uint8_t>(
            s_roundtrip_dead_streak.fetch_add(1, std::memory_order_release) + 1);
        if (streak >= HAP_ROUNDTRIP_DEAD_STREAK &&
            s_synced.exchange(false, std::memory_order_acq_rel)) {
            ESP_LOGE(TAG, "HAP data link dead — %u consecutive roundtrip timeouts, forcing re-SYNC",
                     streak);
            s_roundtrip_dead_streak.store(0, std::memory_order_release);
        }
    }
    return ok;
}

void send_sync_req() {
    uint32_t sid = esp_random();
    uint8_t tx_buf[256];
    uint16_t len = 0;
    if (!hap_json_encode_sync_req(tx_buf, sizeof(tx_buf), &len, sid,
                                  esp_app_get_description()->version)) {
        ESP_LOGE(TAG, "SYNC_REQ encode failed");
        return;
    }
    hap_send(HapMsgType::SYNC, tx_buf, len, 0);   // flags=0: SYNC bypasses window
    ESP_LOGI(TAG, "SYNC_REQ sent sid=0x%08" PRIx32, sid);
}

// ── attr.bulk WebSocket coalescer ────────────────────────────────────────
//
// Under heavy reporting (10+ Zigbee RPS) one ws_event_broadcast per
// per-attr BULK_STATE_UPDATE swamps the WS send buffer for slow clients
// (mobile WiFi, NAT idle) and trips reconnect storms. We accumulate the
// per-attr device_update payloads from P4 and emit one WS frame per
// BULK_COALESCE_WIN_MS — `data` is now an array, the SPA iterates.
//
// Both bulk_append() and bulk_flush() run on task_hap exclusively (the
// frame callback fires from task_hap, the flush is the loop-tail timer
// check); no mutex needed. If another task ever needs to drive these,
// add a lock here.
// BULK_COALESCE_CAP itself lives in s3_internal.h — ws_bridge sizes its
// event envelope from it so a maximal coalesced batch always fits the frame.
static constexpr uint32_t BULK_COALESCE_WIN_MS = 100;

EXT_RAM_BSS_ATTR static char s_bulk_buf[BULK_COALESCE_CAP];
static size_t      s_bulk_len   = 0;
static uint16_t    s_bulk_count = 0;
static TickType_t  s_bulk_last_flush = 0;

static bool bulk_append(const char* payload, size_t len) {
    if (len == 0) return true;
    // Reserve 1 byte for the leading '[' or ',' separator and 1 byte
    // for the closing ']' added at flush time.
    const size_t need = 1 + len + 1;
    if (s_bulk_len + need > BULK_COALESCE_CAP) return false;
    s_bulk_buf[s_bulk_len++] = (s_bulk_count == 0) ? '[' : ',';
    memcpy(s_bulk_buf + s_bulk_len, payload, len);
    s_bulk_len += len;
    s_bulk_count++;
    return true;
}

static void bulk_flush() {
    s_bulk_last_flush = xTaskGetTickCount();
    if (s_bulk_count == 0) return;
    s_bulk_buf[s_bulk_len++] = ']';
    ws_event_broadcast("attr.bulk", s_bulk_buf, s_bulk_len);
    s_bulk_len   = 0;
    s_bulk_count = 0;
}

void task_hap(void*) {
    ESP_LOGI(TAG, "TaskHAP S3 started");

    TaskHandle_t hap_task_handle = xTaskGetCurrentTaskHandle();

    s_p4_metrics_mutex   = xSemaphoreCreateMutex();
    s_waiter_table_mutex = xSemaphoreCreateMutex();
    s_waiter_free_sem    = xSemaphoreCreateCounting(kHapWaiterCount,
                                                      kHapWaiterCount);
    configASSERT(s_waiter_table_mutex && s_waiter_free_sem);
    for (auto& w : s_waiters) {
        w.sem = xSemaphoreCreateBinary();
        configASSERT(w.sem);
        w.seq = 0;
    }

    hap_session_init(HapSessionCfg{
        .send         = hap_master_send,
        .on_frame     = [](const HapFrame& f) {
            _METRIC_COUNTER_INC(METRIC_HAP_RX_FRAMES_TOTAL, 1);
            _METRIC_TIMER_SCOPE(METRIC_HAP_RX_HANDLE);
            // INVARIANT (load-bearing — see hap_session_on_receive's
            // dispatch-before-ACK): every branch here MUST fully read/copy
            // `f.payload` BEFORE issuing any HAP send (hap_master_send / a
            // roundtrip). `f.payload` points into the transport's shared
            // dispatch buffer; a send re-enters the two-stage exchange and
            // overwrites it. `waiter_deliver` below copies it out immediately;
            // a new case that sends MUST do so only AFTER consuming the payload,
            // else it silently delivers a clobbered payload (the device.list
            // empty-list bug).
            // v2 dispatch — runs FIRST so the per-type cases below stay
            // focused on side-effects (WS broadcast, MQTT fanout, log,
            // cache). A response with ack_seq==0 is unsolicited (legacy
            // push) and falls through. rsp_type mismatch on a matched
            // seq still gives the sem so the caller doesn't hang —
            // w->ok stays false → caller returns error.
            if (f.ack_seq != 0) {
                // Defect 2 (FINDINGS §1.2): the find + buffer write + sem-give
                // are now one atomic step under the table mutex (see
                // waiter_deliver) so a concurrently-timing-out caller's
                // waiter_release can never reclaim the slot mid-copy and steer
                // the memcpy into a different caller's buffer.
                waiter_deliver(f.ack_seq, f.type, f.payload, f.payload_len);
            }
            switch (f.type) {
                case HapMsgType::HEARTBEAT: {
                    HapHeartbeat hb{};
                    const bool hb_ok =
                        hap_json_decode_heartbeat(f.payload, f.payload_len, hb);
                    // P4-restart detector. A successfully-decoded uptime that
                    // went BACKWARDS means P4 rebooted and re-ran
                    // hap_session_init — its HAP seq counter rewound to 1. Force
                    // a full re-SYNC: the handshake's SYNC_ACK is where
                    // hap_session clears our stale receive-side dedup high-water.
                    // Without this, every fresh low-seq NEEDS_ACK reply from the
                    // restarted P4 (DEVICE_LIST / DEVICE_INFO / SET_ACK — the only
                    // P4→S3 NEEDS_ACK frames) is silently dropped as "behind the
                    // window" while NO_ACK traffic (heartbeats, *_RSP) keeps
                    // flowing, so device.list/get/set wedge with NO self-heal: the
                    // roundtrip dead-streak that would otherwise re-SYNC keeps
                    // getting reset by the NO_ACK roundtrips that still succeed.
                    // Heartbeats are NO_ACK and arrive every interval regardless
                    // of request traffic, so this catches the restart within one
                    // heartbeat. exchange() ⇒ act only on the synced→unsynced edge
                    // (and skip the work entirely once already unsynced).
                    if (hb_ok) {
                        const uint32_t prev_uptime =
                            s_p4_uptime_s.load(std::memory_order_relaxed);
                        if (prev_uptime != 0 && hb.uptime < prev_uptime &&
                            s_synced.exchange(false, std::memory_order_acq_rel)) {
                            ESP_LOGW(TAG, "P4 restart detected (uptime %" PRIu32
                                     " -> %" PRIu32 ") — forcing re-SYNC to clear "
                                     "stale dedup window", prev_uptime, hb.uptime);
                        }
                    }
                    s_p4_uptime_s.store(hb.uptime,        std::memory_order_relaxed);
                    s_p4_psram_free.store(hb.psram_free,  std::memory_order_relaxed);
                    s_p4_psram_total.store(hb.psram_total,std::memory_order_relaxed);
                    s_p4_cpu_pct_c0.store(hb.cpu_pct_c0,  std::memory_order_relaxed);
                    s_p4_cpu_pct_c1.store(hb.cpu_pct_c1,  std::memory_order_relaxed);
                    s_p4_proto_mask.store(hb.proto_mask,  std::memory_order_relaxed);
                    s_p4_heap_free.store(hb.heap,                           std::memory_order_relaxed);
                    s_p4_heap_min_free.store(hb.heap_min_free,              std::memory_order_relaxed);
                    s_p4_internal_free.store(hb.internal_free,              std::memory_order_relaxed);
                    s_p4_internal_min_free.store(hb.internal_min_free,      std::memory_order_relaxed);
                    s_p4_internal_largest_block.store(hb.internal_largest_block, std::memory_order_relaxed);
                    s_p4_psram_min_free.store(hb.psram_min_free,            std::memory_order_relaxed);
                    s_p4_psram_largest_block.store(hb.psram_largest_block,  std::memory_order_relaxed);
                    s_p4_task_stack_hwm_bytes.store(hb.task_stack_hwm_bytes,std::memory_order_relaxed);
                    s_p4_device_count.store(hb.device_count,                std::memory_order_relaxed);
                    s_last_p4_hb_tick.store(xTaskGetTickCount(),
                                             std::memory_order_relaxed);
                    if (s_p4_unresponsive.exchange(false,
                                                   std::memory_order_relaxed)) {
                        ESP_LOGI(TAG, "P4 heartbeat recovered");
                        static const char kRecover[] =
                            "{\"kind\":\"p4_unresponsive\",\"state\":false}";
                        ws_event_broadcast("alert", kRecover,
                                            sizeof(kRecover) - 1);
                    }
                    ESP_LOGI(TAG, "HEARTBEAT from P4 uptime=%" PRIu32 " proto_mask=0x%02x",
                             hb.uptime, (unsigned)hb.proto_mask);
                    // status.tick event disabled — generating a full
                    // status snapshot (NVS reads, WiFi state, memory
                    // caps) in the TaskHAP heartbeat path and
                    // broadcasting 2 KB on every tick was enough to
                    // contend with bootstrap traffic when the SPA
                    // connects. If the Info page needs live refresh,
                    // the SPA can poll `status.get` on its own cadence.
                    break;
                }
                case HapMsgType::DEVICE_LIST: {
                    ESP_LOGI(TAG, "DEVICE_LIST received len=%d", f.payload_len);
                    // No SPA listener for `device.list.snapshot`. v2 hook
                    // delivers the payload to the requesting caller via
                    // the round-trip envelope; per-device deltas come over
                    // `device.added` / `device.removed`. s_p4_device_count
                    // is driven by HEARTBEAT exclusively.
                    break;
                }
                case HapMsgType::DEVICE_INFO:
                    // v2 dispatch handles it; no broadcast side-effect.
                    break;
                case HapMsgType::BULK_STATE_UPDATE: {
                    // Demote from INFO to DEBUG: under heavy reporting traffic
                    // (multiple devices reporting at once) the per-frame log
                    // line floods the console at >100/sec, slows task_hap
                    // enough to back up SPI exchanges, and hampers WS clients
                    // that subscribe to log streaming.
                    ESP_LOGD(TAG, "BULK len=%d", f.payload_len);
                    // F-02: a single payload >= BULK_COALESCE_CAP can never
                    // fit, so don't even try — flush whatever was queued so
                    // the bypass path's WS broadcast keeps ordering with
                    // prior coalesced entries, then ship the oversize frame
                    // directly. Without this guard the retry below would
                    // also return false and we'd silently lose the update.
                    const auto* bulk_pl =
                        reinterpret_cast<const char*>(f.payload);
                    const size_t bulk_pl_len = f.payload_len;
                    if (bulk_pl_len + 2 > BULK_COALESCE_CAP) {
                        bulk_flush();
                        ESP_LOGW(TAG,
                                 "BULK payload %u B exceeds coalescer cap %u — "
                                 "bypassing coalescer",
                                 (unsigned)bulk_pl_len,
                                 (unsigned)BULK_COALESCE_CAP);
                        ws_event_broadcast("attr.bulk", bulk_pl, bulk_pl_len);
                    } else if (!bulk_append(bulk_pl, bulk_pl_len)) {
                        bulk_flush();
                        // After flush the buffer is empty and the payload
                        // fits (size precheck above). If this still fails
                        // we surface it as an error rather than the prior
                        // silent drop.
                        if (!bulk_append(bulk_pl, bulk_pl_len)) {
                            ESP_LOGE(TAG,
                                     "BULK append dropped after flush: len=%u",
                                     (unsigned)bulk_pl_len);
                        }
                    }
                    // Forward the per-attr update to MQTT on a per-device
                    // topic so subscribers can filter by IEEE without parsing
                    // every message: `<root>/devices/<ieee>/state`. The
                    // payload already carries `ieee`, `attrs`, `lqi`,
                    // `last_seen`. ArduinoJson parse is cheap (~100-200 bytes,
                    // single field lookup) on the task_hap stack.
                    {
                        JsonDocument doc;
                        if (deserializeJson(doc,
                                              reinterpret_cast<const char*>(f.payload),
                                              f.payload_len) == DeserializationError::Ok) {
                            const char* ieee_str =
                                doc["ieee"] | (const char*)nullptr;
                            // Strip leading "0x" if present so the topic
                            // segment is just the hex address.
                            if (ieee_str && ieee_str[0] == '0' &&
                                (ieee_str[1] == 'x' || ieee_str[1] == 'X')) {
                                ieee_str += 2;
                            }
                            if (ieee_str && ieee_str[0]) {
                                char suffix[64];
                                int sn = snprintf(suffix, sizeof(suffix),
                                                   "devices/%s/state", ieee_str);
                                if (sn > 0 && (size_t)sn < sizeof(suffix)) {
                                    char topic[96];
                                    if (mqtt_gw_format_topic(topic, sizeof(topic),
                                                              suffix) > 0) {
                                        mqtt_gw_publish(topic,
                                                         reinterpret_cast<const char*>(f.payload),
                                                         f.payload_len, 0, false);
                                    }
                                }
                            }

                            // Task 16 fix: RainMaker bridge OUT direction.
                            // This BULK_STATE_UPDATE frame — not
                            // device_shadow/event_bus, which are never fed
                            // on S3 (see rmk_bridge.c's file banner) — is
                            // the real per-attribute fan-out. Gate the
                            // whole per-attribute walk behind the one
                            // cheap rmk_bridge_active() check so a
                            // non-RainMaker build, or one with zero
                            // exposed devices, pays nothing beyond that
                            // call (flag-off: always false, no-op stub).
                            if (ieee_str && ieee_str[0] && rmk_bridge_active()) {
                                uint64_t rmk_ieee = parse_ieee(ieee_str);
                                JsonObjectConst attrs = doc["attrs"];
                                for (JsonPairConst kv : attrs) {
                                    JsonVariantConst v = kv.value();
                                    uint8_t val_type;
                                    int32_t ival;
                                    if (v.is<bool>()) {
                                        val_type = VAL_BOOL;
                                        ival = v.as<bool>() ? 1 : 0;
                                    } else if (v.is<int32_t>()) {
                                        // KNOWN, VERIFIED RESIDUAL AMBIGUITY
                                        // (see task-16 fix report): this
                                        // wire shape carries no explicit
                                        // val_type tag, and a whole-number
                                        // VAL_FLOAT attribute serializes
                                        // without a decimal point (traced
                                        // through the vendored
                                        // components/arduinojson v7.4.3
                                        // TextFormatter::writeFloat: the
                                        // fractional digits are only
                                        // emitted when the decomposed
                                        // remainder is nonzero) — such a
                                        // value is wire-indistinguishable
                                        // from a genuine integer and lands
                                        // here, reported un-scaled instead
                                        // of ×100. Narrow and one-
                                        // directional: a genuine integer
                                        // (brightness/color_temp/state) is
                                        // always encoded via setInteger()
                                        // and can never spuriously tag as
                                        // float, so this branch is only
                                        // ever wrong for an exact-whole-
                                        // unit float reading (display
                                        // value error, not a crash).
                                        val_type = VAL_INT;
                                        ival = v.as<int32_t>();
                                    } else if (v.is<float>()) {
                                        // Reconstruct the raw shadow ×100
                                        // convention rmk_bridge_on_attr_
                                        // update expects for VAL_FLOAT —
                                        // the wire value here is already
                                        // real/unscaled
                                        // (hap_json_encode_device_attr_
                                        // update: `attrs[key] =
                                        // (float)int_val / 100.0f`).
                                        val_type = VAL_FLOAT;
                                        ival = (int32_t)lroundf(v.as<float>() * 100.0f);
                                    } else {
                                        continue;   // string/null/object — skip
                                    }
                                    // force=false: this is a real device
                                    // report — skip-unchanged + continuous-
                                    // param rate-limit apply (protects the
                                    // RainMaker MQTT budget from sensor spam).
                                    rmk_bridge_on_attr_update(rmk_ieee, kv.key().c_str(),
                                                              val_type, ival, /*force=*/false);
                                }
                            }
                        }
                    }
                    break;
                }
                case HapMsgType::ALERT: {
                    HapAlert alert{};
                    if (hap_json_decode_alert(f.payload, f.payload_len, alert)) {
                        ESP_LOGW(TAG, "ALERT code=%u ieee=0x%016llX msg=%s",
                                 static_cast<uint8_t>(alert.code),
                                 (unsigned long long)alert.ieee, alert.msg);
                        xSemaphoreTake(s_alert_log_mutex, portMAX_DELAY);
                        s_alert_log[s_alert_log_head] = { alert, (uint32_t)(esp_timer_get_time() / 1000000) };
                        s_alert_log_head = (s_alert_log_head + 1) % ALERT_LOG_MAX;
                        if (s_alert_log_count < ALERT_LOG_MAX) s_alert_log_count++;
                        xSemaphoreGive(s_alert_log_mutex);
                        alert_log_schedule_persist();
                    }
                    // F48 (FINDINGS.md): the legacy "alert.fired" broadcast had
                    // zero consumers (the SPA listens to "alert.added"; cloud +
                    // MQTT get the bare "alert" topic below) — removed as dead
                    // weight. SPA alerts store subscribes to "alert.added" so
                    // new entries appear without polling `alerts.get`.
                    ws_event_broadcast("alert.added",
                                        reinterpret_cast<const char*>(f.payload),
                                        f.payload_len);
                    {
                        char topic[64];
                        if (mqtt_gw_format_topic(topic, sizeof(topic), "alert") > 0)
                            mqtt_gw_publish(topic,
                                            reinterpret_cast<const char*>(f.payload),
                                            f.payload_len, 0, false);
                    }
                    break;
                }
                case HapMsgType::DEVICE_JOIN: {
                    uint64_t ieee = 0;
                    hap_json_decode_device_join(f.payload, f.payload_len, &ieee);
                    ESP_LOGI(TAG, "DEVICE_JOIN ieee=0x%016llX", (unsigned long long)ieee);
                    ws_event_broadcast("device.added",
                                        reinterpret_cast<const char*>(f.payload),
                                        f.payload_len);
                    {
                        char topic[64];
                        if (mqtt_gw_format_topic(topic, sizeof(topic), "device/join") > 0)
                            mqtt_gw_publish(topic,
                                            reinterpret_cast<const char*>(f.payload),
                                            f.payload_len, 0, false);
                    }
                    break;
                }
                case HapMsgType::DEVICE_LEAVE: {
                    uint64_t ieee = 0;
                    hap_json_decode_device_join(f.payload, f.payload_len, &ieee);
                    ESP_LOGI(TAG, "DEVICE_LEAVE ieee=0x%016llX", (unsigned long long)ieee);
                    // Task 16 fix: pulls the RainMaker unpair cleanup
                    // forward from Task 18's own registry-reconciliation
                    // pass. Cheap no-op (flag-off stub, or ieee simply
                    // never exposed) — safe to call unconditionally.
                    rmk_bridge_on_device_gone(ieee);
                    // Task 21 fix: also drop ieee from the persisted NVS
                    // exposed-set (see the forward declaration's own
                    // comment above) — the in-memory registry cleanup above
                    // alone left a stale ieee in NVS forever. Same "cheap
                    // no-op, safe unconditionally" contract.
                    rmk_on_device_gone(ieee);
                    ws_event_broadcast("device.removed",
                                        reinterpret_cast<const char*>(f.payload),
                                        f.payload_len);
                    {
                        char topic[64];
                        if (mqtt_gw_format_topic(topic, sizeof(topic), "device/leave") > 0)
                            mqtt_gw_publish(topic,
                                            reinterpret_cast<const char*>(f.payload),
                                            f.payload_len, 0, false);
                    }
                    break;
                }
                // v2 dispatch handles SET/BIND/DEVICE_DELETE/DEVICE_OPTIONS/
                // ZIGBEE_CFG/RULE/SCRIPT/DIAG responses — no per-type
                // side-effects beyond the caller's payload copy.
                case HapMsgType::METRICS_RSP: {
                    // Unsolicited in the sense that the /metrics
                    // scrape isn't blocking on it — the bridge loop
                    // periodically asks, we cache whatever arrives.
                    const size_t copy_len =
                        f.payload_len < sizeof(s_p4_metrics_buf) - 1
                            ? f.payload_len
                            : sizeof(s_p4_metrics_buf) - 1;
                    if (s_p4_metrics_mutex) {
                        xSemaphoreTake(s_p4_metrics_mutex, portMAX_DELAY);
                        memcpy(s_p4_metrics_buf, f.payload, copy_len);
                        s_p4_metrics_buf[copy_len] = '\0';
                        s_p4_metrics_len =
                            static_cast<uint16_t>(copy_len);
                        xSemaphoreGive(s_p4_metrics_mutex);
                    }
                    break;
                }
                // SCRIPT_READ_RSP handled by v2 dispatch above.
                case HapMsgType::MQTT_PUBLISH: {
                    HapMqttPublish msg{};
                    if (hap_json_decode_mqtt_publish(f.payload, f.payload_len, msg)) {
                        mqtt_gw_publish(msg.topic, msg.payload,
                                         strlen(msg.payload),
                                         msg.qos, msg.retain);
                    } else {
                        ESP_LOGE(TAG, "MQTT_PUBLISH decode failed");
                    }
                    break;
                }
                case HapMsgType::TG_SETTOKEN: {
                    tg_gw_handle_settoken(f.payload, f.payload_len);
                    break;
                }
                case HapMsgType::TG_SETCHAT: {
                    tg_gw_handle_setchat(f.payload, f.payload_len);
                    break;
                }
                case HapMsgType::TG_SEND: {
                    tg_gw_handle_send(f.payload, f.payload_len);
                    break;
                }
                case HapMsgType::OTA_STATUS: {
                    HapOtaStatus st{};
                    if (hap_json_decode_ota_status(f.payload, f.payload_len, st)) {
                        s_p4ota_status = st;
                        xSemaphoreGive(s_p4ota_rsp_sem);
                        // err[] is meaningful only when ok=false; include
                        // it in the log so the failure cause (e.g.
                        // ESP_ERR_NO_MEM, ESP_ERR_FLASH_OP_FAIL, offset
                        // mismatch) is visible without the SPA UI.
                        if (st.ok) {
                            ESP_LOGI(TAG, "P4 OTA_STATUS ok=1 rcvd=%" PRIu32 "/%" PRIu32,
                                     st.rcvd, st.total);
                        } else {
                            ESP_LOGE(TAG, "P4 OTA_STATUS ok=0 rcvd=%" PRIu32 "/%" PRIu32
                                     " err='%s'",
                                     st.rcvd, st.total, st.err);
                        }
                    }
                    break;
                }
                case HapMsgType::OTA_CHECKPOINT_RSP: {
                    // Special-case: main.cpp's OTA flow uses hap_send +
                    // s_p4ota_ckpt_rsp_sem directly (not hap_roundtrip_v2),
                    // so the legacy sem-give path stays. Frames carry
                    // ack_seq==0 because the request goes via hap_send.
                    if (f.payload_len >= 8) {
                        s_p4ota_ckpt_offset =
                            (uint32_t)f.payload[0]        |
                            ((uint32_t)f.payload[1] << 8)  |
                            ((uint32_t)f.payload[2] << 16) |
                            ((uint32_t)f.payload[3] << 24);
                    }
                    xSemaphoreGive(s_p4ota_ckpt_rsp_sem);
                    break;
                }
                case HapMsgType::LOG_LINE: {
                    char msg[HAP_LOG_MSG_MAX]{};
                    if (hap_json_decode_log_line(f.payload, f.payload_len, msg, sizeof(msg))) {
                        // Route through the log sinks so `src` reads "p4"
                        // on the MQTT / WS output, and the line appears in
                        // /api/logs alongside S3-side entries.
                        log_sinks_publish_p4('I', "p4", msg);
                    }
                    break;
                }
                // Response-only types fully handled by the v2 dispatch
                // hook at the top of this lambda — no per-type side-effect.
                case HapMsgType::SET_ACK:
                case HapMsgType::BIND_ACK:
                case HapMsgType::DEVICE_DELETE_ACK:
                case HapMsgType::DEVICE_OPTIONS_SET_ACK:
                case HapMsgType::ZIGBEE_CFG_SET_ACK:
                case HapMsgType::RULE_EXEC_RESULT:
                case HapMsgType::RULE_LIST_RSP:
                case HapMsgType::SCRIPT_ACK:
                case HapMsgType::SCRIPT_LIST_RSP:
                case HapMsgType::SCRIPT_CHECK_RSP:
                case HapMsgType::SCRIPT_READ_RSP:
                case HapMsgType::DIAG_UNHANDLED_RSP:
                    break;
                default:
                    ESP_LOGW(TAG, "unhandled HAP type=0x%02x",
                             static_cast<uint8_t>(f.type));
                    break;
            }
        },
        .on_sync      = [](const HapFrame& f) {
            HapSyncInfo info{};
            if (!hap_json_decode_sync(f.payload, f.payload_len, info)) return;
            if (!info.is_ack) return;
            s_synced.store(true, std::memory_order_release);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
            // F-08: SYNC_ACK is proof the new firmware can talk to P4
            // over SPI — the strongest health signal we have. Cancel
            // rollback so a clean reboot won't revert. Idempotent: only
            // fires once per boot.
            static std::atomic<bool> s_marked_valid{false};
            bool expected = false;
            if (s_marked_valid.compare_exchange_strong(expected, true)) {
                esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
                if (mv == ESP_OK) {
                    ESP_LOGI(TAG, "OTA: marked image valid (SYNC_ACK)");
                } else {
                    ESP_LOGW(TAG, "OTA: mark_valid failed: %s",
                             esp_err_to_name(mv));
                }
            }
#endif
            // F-10: `s_p4_device_count` is now driven exclusively by the
            // HEARTBEAT path. The boot-time seed is harmless on its own
            // but a third writer was the F-10 race; first HEARTBEAT
            // arrives within ~5 s of SYNC anyway.
            {
                const size_t cap = sizeof(s_p4_fw_ver) - 1;
                const size_t n   = strnlen(info.fw_ver, cap);
                xSemaphoreTake(s_p4_metrics_mutex, portMAX_DELAY);
                memcpy(s_p4_fw_ver, info.fw_ver, n);
                s_p4_fw_ver[n] = '\0';
                xSemaphoreGive(s_p4_metrics_mutex);
            }
            // fw_ver is now a git-describe release string (e.g. v2026061501),
            // not a protocol version — don't gate on it (that old check would
            // false-warn on every release). Protocol compatibility is conveyed
            // by proto_mask; the peer build is just recorded for the Info page.
            ESP_LOGI(TAG, "SYNC_ACK received — P4 has %d devices, fw=%s",
                     info.device_count, info.fw_ver);
        },
        .on_link_dead = []() {
            s_synced.store(false, std::memory_order_release);
            ESP_LOGE(TAG, "HAP link dead — will retry SYNC");
        },
    });

    hap_master_set_callback([hap_task_handle](const HapFrame& f) {
        hap_session_on_receive(f);
        xTaskNotifyGive(hap_task_handle);
    });
    hap_master_set_task_handle(hap_task_handle);

    send_sync_req();

    TickType_t last_sync_attempt = xTaskGetTickCount();
    TickType_t last_hb           = xTaskGetTickCount();
    TickType_t last_metrics_req  = 0;   // send one on first sync

    while (true) {
        // pdFALSE: decrement count by 1 (treat as counting semaphore).
        // pdTRUE would clear all pending notifies on each call — under
        // burst traffic, multiple DRDY edges could accumulate between
        // iterations and we'd silently drop every frame past the first.
        // That manifested as S3 seeing only 1-in-10 of P4's 5 s
        // heartbeats and missing many BULK_STATE_UPDATE pushes too.
        uint32_t notified = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(100));
        if (notified) {
            hap_master_recv();
        }
        hap_session_tick();

        if (!s_synced.load(std::memory_order_acquire) &&
            xTaskGetTickCount() - last_sync_attempt >= pdMS_TO_TICKS(2000)) {
            last_sync_attempt = xTaskGetTickCount();
            send_sync_req();
        }

        // Detect a silent P4: if three heartbeat intervals have passed
        // without a HEARTBEAT from P4, flag the link as unresponsive. Flag
        // is cleared by the HEARTBEAT receive path above.
        if (s_synced.load(std::memory_order_acquire)) {
            const TickType_t last_rx = s_last_p4_hb_tick.load(
                std::memory_order_relaxed);
            if (last_rx != 0 &&
                xTaskGetTickCount() - last_rx >=
                    pdMS_TO_TICKS(HAP_HEARTBEAT_INTERVAL_MS * 3) &&
                !s_p4_unresponsive.exchange(true,
                                             std::memory_order_relaxed)) {
                ESP_LOGW(TAG, "P4 unresponsive: no heartbeat for %d ms",
                         HAP_HEARTBEAT_INTERVAL_MS * 3);
                static const char kTimeout[] =
                    "{\"kind\":\"p4_unresponsive\",\"state\":true}";
                ws_event_broadcast("alert", kTimeout,
                                    sizeof(kTimeout) - 1);
            }
        }

        if (s_synced.load(std::memory_order_acquire) &&
            xTaskGetTickCount() - last_hb >= pdMS_TO_TICKS(HAP_HEARTBEAT_INTERVAL_MS)) {
            last_hb = xTaskGetTickCount();
            uint8_t hb_buf[384];
            uint16_t len = 0;
            HapHeartbeat hbs{};
            hbs.uptime      = (uint32_t)(esp_timer_get_time() / 1000000UL);
            hbs.heap        = esp_get_free_heap_size();
            hbs.psram_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            hbs.psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            // S3 CPU is reported via /api/status (sampled in the REST handler);
            // no value to carry over the wire to P4 here.
            hbs.cpu_pct_c0  = 0;
            hbs.cpu_pct_c1  = 0;
            hbs.proto_mask  = 0;
            if (hap_json_encode_heartbeat(hb_buf, sizeof(hb_buf), &len, hbs)) {
                hap_send(HapMsgType::HEARTBEAT, hb_buf, len, HAP_FLAG_NO_ACK);
            }
        }

        // Periodic P4 metrics refresh — fire-and-forget request; the
        // METRICS_RSP case above caches whatever arrives.
        if (s_synced.load(std::memory_order_acquire) &&
            xTaskGetTickCount() - last_metrics_req >=
                pdMS_TO_TICKS(HAP_METRICS_REFRESH_MS)) {
            last_metrics_req = xTaskGetTickCount();
            hap_send(HapMsgType::METRICS_REQ, nullptr, 0, HAP_FLAG_NO_ACK);
        }

        // Flush the attr.bulk coalescer on the BULK_COALESCE_WIN_MS
        // cadence. Skipping this lets accumulated attrs grow stale on
        // the SPA; tightening defeats the throughput win.
        if (xTaskGetTickCount() - s_bulk_last_flush >=
                pdMS_TO_TICKS(BULK_COALESCE_WIN_MS)) {
            bulk_flush();
        }
    }
}
