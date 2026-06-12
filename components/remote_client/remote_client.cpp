// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Remote WSS client — premium pay feature, opt-in at build time
// (Kconfig) AND run time (NVS). See:
//   - spec : docs/superpowers/specs/2026-05-19-remote-client-design.md
//   - plan : docs/superpowers/plans/2026-05-19-remote-client-plan.md
//
// Hot-path constraints:
//   - publish_event() must be <2 cycles when not running (atomic load).
//   - No mutex shared with ws_server or ws_bridge.
//   - TX queue NEVER persists across disconnect: the cloud bootstrap
//     supersedes any queued state (spec §1.5 non-goals).

#include "remote_client.h"
#include "remote_state.h"
#include "remote_allow.h"
#include "remote_backoff.h"
#include "remote_nvs.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "ArduinoJson.h"

#include "ws_server.h"  // ws_server_register_reply_hook

// Forward decl for the dispatcher exported by ws_bridge.cpp.
// Defined in zhac-net-core/main/ws_bridge.cpp; the linker resolves the
// symbol because the main component (which links ws_bridge.cpp) is part
// of the firmware image that pulls in remote_client. This decl avoids
// a cross-component header reach into `main/` (components in IDF don't
// see main/'s private headers unless main is a REQUIRES, which would
// invert the dependency direction).
//
// ArduinoJson is already in this component's REQUIRES list, so the
// JsonDocument type is available via the include above.
extern "C" void dispatch_envelope_for_remote(int fd, JsonDocument& doc);

static const char* TAG = "remote_client";

// ── State + atomics ──────────────────────────────────────────────────────
static std::atomic<bool>           s_running{false};      // hot-path gate
static std::atomic<RemoteState>    s_state{REMOTE_STATE_DISABLED};
static uint8_t                     s_attempt = 0;          // backoff counter
static uint32_t                    s_auth_id = 0;          // F22: id of the in-flight auth request (0 = none pending)
static uint32_t                    s_auth_enter_ms = 0;    // F22: tick (ms) we entered AUTHENTICATING
static esp_websocket_client_handle_t s_ws = nullptr;
static TaskHandle_t                s_task = nullptr;
static QueueHandle_t               s_tx_queue = nullptr;
static QueueHandle_t               s_rx_queue = nullptr;

// NOTE: deliberately NOT `static`. Task 11 needs to reference this from
// app_main via an `extern EventGroupHandle_t s_remote_evt;` declaration
// so wifi event handlers can post EVB_WIFI_UP / EVB_WIFI_DOWN bits.
// The leading `s_` is a vestige of the pre-extern naming kept for
// internal-use clarity inside this TU.
EventGroupHandle_t s_remote_evt = nullptr;

// Counters surfaced via api_remote_status.
static std::atomic<uint32_t> s_tx_drops{0};
static std::atomic<uint32_t> s_auth_fails{0};
static std::atomic<uint32_t> s_connected_since{0};
static std::atomic<uint32_t> s_last_event_at{0};
static std::atomic<uint16_t> s_rtt_ms{0};

// Event bits.
static constexpr EventBits_t EVB_ENABLE       = 1 << 0;
static constexpr EventBits_t EVB_DISABLE      = 1 << 1;
static constexpr EventBits_t EVB_WIFI_UP      = 1 << 2;
static constexpr EventBits_t EVB_WIFI_DOWN    = 1 << 3;
static constexpr EventBits_t EVB_WSS_EVENT    = 1 << 4;
static constexpr EventBits_t EVB_AUTH_OK      = 1 << 5;
static constexpr EventBits_t EVB_AUTH_FAIL    = 1 << 6;
static constexpr EventBits_t EVB_BACKOFF_DONE = 1 << 7;
static constexpr EventBits_t EVB_TX           = 1 << 8; // RX/TX frame pending — wake the loop to drain the queues now
// Defensive (FINDINGS §5, :536): remote_client_enable() must NOT load NVS into
// s_cfg from the API caller's task while a live task_remote concurrently reads
// s_cfg.url / s_cfg.token (start_ws_client / send_auth_frame) — that races a
// torn credential read. Instead enable() sets this bit and the remote task
// reloads s_cfg from NVS at a safe point (top of the loop, before any connect),
// so only task_remote ever touches s_cfg. api_remote_connect persists the new
// url/token to NVS *before* calling enable(), so the deferred reload always
// observes the fresh creds.
static constexpr EventBits_t EVB_RELOAD_CFG   = 1 << 9;

// Persistent config (loaded from NVS, refreshed on _enable()).
struct RemoteCfg {
    bool enabled;
    char url[REMOTE_NVS_URL_MAX];
    char token[REMOTE_NVS_TOKEN_MAX];
    char devid[REMOTE_NVS_DEVID_MAX];
};
static RemoteCfg s_cfg = {};

// TX queue item — caller-allocated PSRAM blob, ownership transferred.
struct TxItem {
    char*  json;
    size_t len;
};

// RX queue item — esp_websocket_client gives us frames as raw bytes
// up to its configured buffer size; we copy to PSRAM and post.
struct RxItem {
    char*  json;
    size_t len;
};

// ── Hot path: publish_event ──────────────────────────────────────────────

extern "C" bool remote_client_is_running(void) {
    return s_running.load(std::memory_order_relaxed);
}

extern "C" void remote_client_publish_event(const char* name,
                                            const char* payload_json,
                                            size_t payload_len) {
    if (!s_running.load(std::memory_order_relaxed)) return;
    if (!remote_event_allowed(name)) return;
    if (s_state.load(std::memory_order_acquire) != REMOTE_STATE_READY) return;

    // Format the full envelope into a PSRAM blob — transfer ownership
    // to the queue. Bound: 8 KB matches inbound RX cap.
    constexpr size_t kCap = 8 * 1024;
    char* buf = (char*)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        // OOM. Spec §7 #9.
        ESP_LOGW(TAG, "publish_event OOM (event=%s)", name);
        return;
    }
    int n;
    if (payload_json && payload_len > 0) {
        n = snprintf(buf, kCap, "{\"event\":\"%s\",\"data\":%.*s}",
                     name, (int)payload_len, payload_json);
    } else {
        n = snprintf(buf, kCap, "{\"event\":\"%s\",\"data\":null}", name);
    }
    if (n <= 0 || (size_t)n >= kCap) {
        heap_caps_free(buf);
        return;
    }

    TxItem item{ buf, (size_t)n };
    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        // Queue full — drop oldest, free, enqueue this.
        TxItem old{};
        if (xQueueReceive(s_tx_queue, &old, 0) == pdTRUE) {
            heap_caps_free(old.json);
            s_tx_drops.fetch_add(1);
        }
        if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
            heap_caps_free(buf);
            s_tx_drops.fetch_add(1);
        }
    }
    // Wake the remote task so it drains + sends this frame promptly. Without this it sleeps in
    // xEventGroupWaitBits until the 1 s timeout, adding up to ~1 s of latency to every forwarded
    // event (e.g. the attr.bulk live-state stream to the cloud).
    if (s_remote_evt) xEventGroupSetBits(s_remote_evt, EVB_TX);
}

extern "C" void remote_client_get_status(RemoteStatusSnap* out) {
    if (!out) return;
    *out = {};
    out->enabled         = s_cfg.enabled;
    out->state           = (uint8_t)s_state.load(std::memory_order_relaxed);
    out->connected_since = s_connected_since.load();
    out->last_event_at   = s_last_event_at.load();
    out->rtt_ms          = s_rtt_ms.load();
    out->tx_drops        = s_tx_drops.load();
    out->auth_fails      = s_auth_fails.load();
}

// ── esp_websocket_client event handler ───────────────────────────────────
// Runs on the IDF websocket-client internal task. Posts to event group
// or RX queue and returns fast — no processing on this task's stack.

static void ws_event_handler(void* /*arg*/, esp_event_base_t /*base*/,
                              int32_t event_id, void* event_data) {
    auto* d = (esp_websocket_event_data_t*)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ws connected");
            xEventGroupSetBits(s_remote_evt, EVB_WSS_EVENT);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "ws disconnected/error");
            xEventGroupSetBits(s_remote_evt, EVB_WSS_EVENT);
            break;
        case WEBSOCKET_EVENT_DATA:
            // INBOUND FRAME CONTRACT (FINDINGS §5, :199) — 8 KB single-frame cap.
            //
            // This handler dispatches ONLY complete, single text frames:
            //   - op_code 0x01 (text). Binary (0x02) and, deliberately,
            //     fragmentation continuation frames (op_code 0x00 with a
            //     non-zero payload_offset) are dropped — reassembly across
            //     CONTINUATION frames is NOT implemented in this batch.
            //   - data_len in (0, 8192]. The esp_websocket_client buffer_size
            //     is fixed at 8 KB (see start_ws_client) and the outbound
            //     publish_event envelope shares the same 8 KB cap, so both
            //     directions agree on the bound.
            //
            // CONTRACT for the cloud: every device-bound message MUST fit in a
            // single ≤ 8 KB text frame. Larger or fragmented payloads are
            // silently discarded here and will never reach dispatch — the
            // device-sync protocol must page/chunk to stay under the cap
            // (cross-ref: project_hub_cloud_devicesync — remote device.list /
            // events; the reconcile relist must not emit a >8 KB frame). If a
            // future protocol revision needs larger frames, raise buffer_size
            // AND implement continuation reassembly here together.
            if (d->op_code != 0x01) break;
            if (d->data_len == 0 || d->data_len > 8192) break;
            {
                char* copy = (char*)heap_caps_malloc(d->data_len + 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!copy) break;
                std::memcpy(copy, d->data_ptr, d->data_len);
                copy[d->data_len] = 0;
                RxItem item{ copy, (size_t)d->data_len };
                if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
                    heap_caps_free(copy);
                }
                // Wake the remote task to drain + dispatch RX now, instead of waiting out the 1 s
                // xEventGroupWaitBits timeout — that delay is why cloud commands/reconcile felt
                // ~1 s slow while the local ws_server path (no queue) is instant.
                xEventGroupSetBits(s_remote_evt, EVB_TX);
            }
            break;
        default: break;
    }
}

// ── Auth frame send ──────────────────────────────────────────────────────

static void send_auth_frame() {
    // F22 (FINDINGS.md): use a fresh, unpredictable id per auth attempt. The
    // server echoes the request id back (spec §3 "the id field is opaque to
    // the device; the device echoes it back"), so handle_rx_frame can bind the
    // reply to *this* request and reject stale / replayed / unsolicited frames.
    s_auth_id = esp_random() & 0x7FFFFFFFu;   // positive JSON int range
    if (s_auth_id == 0) s_auth_id = 1;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"id\":%u,\"cmd\":\"remote.auth\","
        "\"args\":{\"token\":\"%s\",\"device_id\":\"%s\",\"fw_version\":\"%s\"}}",
        (unsigned)s_auth_id, s_cfg.token, s_cfg.devid, "vDEV");
    if (n > 0 && (size_t)n < sizeof(buf) && s_ws) {
        esp_websocket_client_send_text(s_ws, buf, n, pdMS_TO_TICKS(2000));
    }
}

// ── RX dispatch ──────────────────────────────────────────────────────────

extern "C" void remote_client_send_reply(const char* json, size_t len) {
    if (!s_ws || s_state.load(std::memory_order_acquire) != REMOTE_STATE_READY) return;
    esp_websocket_client_send_text(s_ws, json, len, pdMS_TO_TICKS(2000));
}

static void handle_rx_frame(const char* data, size_t len) {
    // Allocate the parser doc in PSRAM (8 KB).
    JsonDocument doc;
    auto err = deserializeJson(doc, data, len);
    if (err) {
        ESP_LOGW(TAG, "rx parse err: %s", err.c_str());
        return;
    }

    // Auth response path — only while AUTHENTICATING, and only for the id we
    // actually sent (F22). Any other frame in this state is ignored: we are
    // not authenticated yet, so it must not reach command dispatch.
    RemoteState st_rx = s_state.load(std::memory_order_acquire);
    if (st_rx == REMOTE_STATE_AUTHENTICATING) {
        uint32_t id = doc["id"] | 0u;
        if (s_auth_id != 0 && id == s_auth_id) {
            bool ok = doc["ok"] | false;
            s_auth_id = 0;   // consume — one reply per request
            if (ok) {
                // Enter READY here, not on the next main-loop pass. The cloud pipelines the first
                // command (reconcile's device.list) right behind the auth ack, and both frames are
                // commonly drained in THIS same rx pass — so the very next handle_rx_frame() must see
                // READY or it would drop device.list as an unexpected auth-state frame (→ the cloud
                // waits out its 10 s request timeout and reconcile fails). The main loop still runs
                // the READY-entry side effects (s_running / connected_since / TX drain) when it
                // observes EVB_AUTH_OK. handle_rx_frame() only ever runs on the main task, so this
                // store does not race the step() writer.
                s_state.store(REMOTE_STATE_READY, std::memory_order_release);
                xEventGroupSetBits(s_remote_evt, EVB_AUTH_OK);
            } else {
                s_auth_fails.fetch_add(1);
                xEventGroupSetBits(s_remote_evt, EVB_AUTH_FAIL);
            }
        } else {
            ESP_LOGW(TAG, "auth-state frame id=%u (want %u) — ignored",
                     (unsigned)id, (unsigned)s_auth_id);
        }
        return;
    }

    // READY path — dispatch by cmd.
    const char* cmd = doc["cmd"] | "";

    // Mid-session remote.auth challenge — re-send token, stay in READY.
    if (st_rx == REMOTE_STATE_READY && std::strcmp(cmd, "remote.auth") == 0) {
        send_auth_frame();
        s_last_event_at.store((uint32_t)(esp_log_timestamp() / 1000));
        return;
    }

    // Allow-list gate.
    if (!remote_cmd_allowed(cmd)) {
        char err_buf[160];
        int en = snprintf(err_buf, sizeof(err_buf),
            "{\"id\":%d,\"ok\":false,\"err\":\"cmd_not_allowed\"}",
            doc["id"] | 0);
        if (en > 0) remote_client_send_reply(err_buf, en);
        return;
    }

    // Hand off to the shared ws_bridge dispatcher.
    dispatch_envelope_for_remote(REMOTE_VIRTUAL_FD, doc);
    s_last_event_at.store((uint32_t)(esp_log_timestamp() / 1000));
}

// ── Task body ────────────────────────────────────────────────────────────

static void start_ws_client() {
    esp_websocket_client_config_t cfg = {};
    cfg.uri                = s_cfg.url;
    cfg.buffer_size        = 8 * 1024;
    cfg.ping_interval_sec  = CONFIG_ZHAC_REMOTE_PING_INTERVAL_S;
    cfg.network_timeout_ms = 10 * 1000;
    cfg.disable_auto_reconnect = true;  // we run our own backoff
    cfg.crt_bundle_attach  = esp_crt_bundle_attach;
    // The TLS handshake — including X.509 chain verification (RSA bignum, up to the RSA-4096
    // ISRG root for a Let's Encrypt cert) — runs in the esp_websocket_client task. The default
    // WEBSOCKET_TASK_STACK_SIZE (~4 KiB) overflows there: a plaintext endpoint bails before any
    // cert math, but a real wss:// chain triggers a "Stack canary watchpoint (websocket_task)"
    // panic. 10 KiB gives the verify path comfortable headroom.
    cfg.task_stack         = 10 * 1024;

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "ws_client_init failed"); return; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);
    esp_websocket_client_start(s_ws);
}

static void stop_ws_client() {
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = nullptr;
}

static void drain_tx_queue() {
    TxItem item{};
    while (xQueueReceive(s_tx_queue, &item, 0) == pdTRUE) {
        heap_caps_free(item.json);
    }
}

// True when the STA interface currently holds a non-zero IPv4 lease. Used to self-heal a missed
// EVB_WIFI_UP edge (see the IDLE_NO_WIFI case): app_main posts EVB_WIFI_UP on IP_EVENT_STA_GOT_IP,
// but if got-IP fired before this task processed EVB_ENABLE the one-shot bit is lost and we would
// otherwise sit in IDLE_NO_WIFI forever despite a live STA.
static bool sta_has_ip() {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) return false;
    esp_netif_ip_info_t ip{};
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void task_remote_body(void*) {
    // Initial enable is driven exclusively by the EVB_ENABLE bit that
    // remote_client_init()/remote_client_enable() set after spawning us — we do
    // NOT read s_cfg here. Reading s_cfg at body entry would (a) race a
    // concurrent remote_client_disable() clearing s_cfg on the API task during
    // a respawn, and (b) duplicate the EVB_ENABLE step. enable() also queues
    // EVB_RELOAD_CFG, so s_cfg is refreshed in-loop before the first connect.

    // State-entry detection for AUTHENTICATING — LOCAL (not static): the task
    // self-deletes on DISABLED and is respawned on re-enable, so a static would
    // retain the previous incarnation's state and break just_entered_auth edge
    // detection. A fresh task always starts from DISABLED.
    RemoteState last_state = REMOTE_STATE_DISABLED;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_remote_evt,
            EVB_ENABLE | EVB_DISABLE | EVB_WIFI_UP | EVB_WIFI_DOWN |
            EVB_WSS_EVENT | EVB_AUTH_OK | EVB_AUTH_FAIL | EVB_BACKOFF_DONE |
            EVB_TX | EVB_RELOAD_CFG,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

        // Deferred credential reload (FINDINGS §5, :536). remote_client_enable()
        // requested an NVS reload by setting EVB_RELOAD_CFG instead of touching
        // s_cfg itself. Reload here — on task_remote, the SOLE owner of s_cfg —
        // before any state step / connect this pass, so start_ws_client() and
        // send_auth_frame() below always read a consistent url/token. The next
        // connect attempt (after the EVB_ENABLE that rode in alongside this bit)
        // picks up the fresh creds; a live connection on stale creds is torn
        // down by the surrounding enable→reconnect flow.
        if (bits & EVB_RELOAD_CFG) {
            remote_nvs_load(&s_cfg.enabled,
                            s_cfg.url,   sizeof(s_cfg.url),
                            s_cfg.token, sizeof(s_cfg.token),
                            s_cfg.devid, sizeof(s_cfg.devid));
        }

        // Map bits -> RemoteEvent and step state machine.
        auto step = [](RemoteEvent ev) {
            auto cur = s_state.load(std::memory_order_relaxed);
            s_state.store(remote_state_next(cur, ev),
                          std::memory_order_release);
        };
        if (bits & EVB_DISABLE)       step(REMOTE_EV_DISABLE);
        if (bits & EVB_ENABLE)        step(REMOTE_EV_ENABLE);
        if (bits & EVB_WIFI_DOWN)     step(REMOTE_EV_WIFI_DOWN);
        if (bits & EVB_WIFI_UP)       step(REMOTE_EV_WIFI_UP);

        // Handle queued RX frames regardless of bit flags (the websocket
        // event handler may have posted data without setting a bit).
        RxItem rx{};
        while (xQueueReceive(s_rx_queue, &rx, 0) == pdTRUE) {
            handle_rx_frame(rx.json, rx.len);
            heap_caps_free(rx.json);
        }

        // Side effects per state transitions.
        RemoteState cur_state = s_state.load(std::memory_order_acquire);
        bool just_entered_auth =
            (cur_state == REMOTE_STATE_AUTHENTICATING) &&
            (last_state != REMOTE_STATE_AUTHENTICATING);

        switch (cur_state) {
            case REMOTE_STATE_DISABLED:
                s_running.store(false);
                stop_ws_client();
                drain_tx_queue();
                s_task = nullptr;
                vTaskDelete(nullptr);
                return;

            case REMOTE_STATE_IDLE_NO_WIFI:
                s_running.store(false);
                stop_ws_client();
                // Self-heal a missed EVB_WIFI_UP edge: if the STA already has an IP (got-IP fired
                // before we processed EVB_ENABLE, so the one-shot bit was lost), promote to
                // CONNECTING. This case re-runs every ~1 s on the wait-bits timeout, so recovery
                // is bounded to ~1 s without relying on a fresh wifi event.
                if (sta_has_ip()) step(REMOTE_EV_WIFI_UP);
                break;

            case REMOTE_STATE_CONNECTING:
                if (!s_ws) start_ws_client();
                if (bits & EVB_WSS_EVENT) {
                    // Check connection status.
                    if (s_ws && esp_websocket_client_is_connected(s_ws)) {
                        step(REMOTE_EV_WSS_CONNECT);
                    } else {
                        step(REMOTE_EV_WSS_ERROR);
                    }
                }
                break;

            case REMOTE_STATE_AUTHENTICATING:
                // Send auth frame exactly once per entry into this state and
                // arm the timeout window (F22).
                if (just_entered_auth) {
                    s_auth_enter_ms = esp_log_timestamp();
                    send_auth_frame();
                }
                if (bits & EVB_AUTH_OK) {
                    step(REMOTE_EV_AUTH_OK);
                } else if (bits & EVB_AUTH_FAIL) {
                    step(REMOTE_EV_AUTH_FAIL);   // F45: s_attempt is bumped once, in BACKOFF
                } else if (bits & EVB_WSS_EVENT &&
                           s_ws && !esp_websocket_client_is_connected(s_ws)) {
                    step(REMOTE_EV_WSS_ERROR);
                } else if ((esp_log_timestamp() - s_auth_enter_ms) >= 5000) {
                    // F22 (FINDINGS.md) + spec §7 #3: bounded 5 s auth timeout.
                    // A server that stalls after the TLS handshake no longer
                    // pins us in AUTHENTICATING forever — close + BACKOFF.
                    // (Subtraction is tick-wrap safe.)
                    ESP_LOGW(TAG, "auth timeout (5s) — no valid reply, backing off");
                    s_auth_id = 0;   // F45: BACKOFF bumps s_attempt — don't double-count here
                    step(REMOTE_EV_AUTH_TIMEOUT);
                }
                break;

            case REMOTE_STATE_READY: {
                // Detect a dropped socket EVERY tick, not only when a WSS event happens to be set.
                // A silent drop — or a DISCONNECTED event whose is_connected race we missed — would
                // otherwise pin us in READY, retrying sends into a dead socket forever ("Websocket
                // client is not connected") and forcing a manual remote.disconnect/connect. The
                // ~1 s wait-bits timeout re-runs this, bounding recovery → BACKOFF → auto-reconnect.
                if (s_ws && !esp_websocket_client_is_connected(s_ws)) {
                    step(REMOTE_EV_WSS_ERROR);
                    break;   // don't drain TX into a dead socket
                }
                s_running.store(true);
                s_attempt = 0;
                if (s_connected_since.load() == 0) {
                    s_connected_since.store((uint32_t)(esp_log_timestamp() / 1000));
                }
                // Drain TX queue.
                TxItem tx{};
                while (xQueueReceive(s_tx_queue, &tx, 0) == pdTRUE) {
                    if (s_ws) {
                        esp_websocket_client_send_text(s_ws, tx.json, tx.len, pdMS_TO_TICKS(2000));
                    }
                    heap_caps_free(tx.json);
                }
                break;
            }

            case REMOTE_STATE_BACKOFF: {
                s_running.store(false);
                stop_ws_client();
                s_connected_since.store(0);
                s_attempt++;
                uint32_t delay = remote_backoff_compute(
                    s_attempt, CONFIG_ZHAC_REMOTE_BACKOFF_MAX_S, esp_random());
                ESP_LOGI(TAG, "backoff %u s (attempt=%u)", (unsigned)delay, s_attempt);
                // FINDINGS §5 (:491): wait out the backoff ON THE EVENT GROUP,
                // not via vTaskDelay. A bare vTaskDelay (up to BACKOFF_MAX_S)
                // pinned the state-machine task asleep, so an EVB_DISABLE or
                // EVB_WIFI_DOWN posted during backoff sat unprocessed for the
                // whole sleep (a disable/wifi-down felt unresponsive for up to
                // a full backoff window). Block on those two bits with the
                // backoff as the TIMEOUT: an abort event wakes us immediately.
                //
                // clear-on-exit = pdFALSE: leave the abort bit set so the
                // top-of-loop step() dispatch handles it next iteration (→
                // DISABLED / IDLE_NO_WIFI). On a clean timeout (no abort bit)
                // fire BACKOFF_DONE and advance to CONNECTING as before.
                EventBits_t bo = xEventGroupWaitBits(
                    s_remote_evt, EVB_DISABLE | EVB_WIFI_DOWN,
                    pdFALSE, pdFALSE, pdMS_TO_TICKS(delay * 1000));
                if (bo & (EVB_DISABLE | EVB_WIFI_DOWN)) {
                    // Aborted — re-loop; the abort bit is still set and will be
                    // consumed (pdTRUE) + stepped at the top of the loop.
                    break;
                }
                // belt-and-suspenders; the step() below is the real
                // BACKOFF->CONNECTING transition (the bit is consumed as a
                // no-op by the next clearOnExit wait).
                xEventGroupSetBits(s_remote_evt, EVB_BACKOFF_DONE);
                step(REMOTE_EV_BACKOFF_DONE);
                break;
            }
        }

        last_state = cur_state;
    }
}

// ── Public API ───────────────────────────────────────────────────────────

// Create the kernel objects (TX/RX queues + event group) and register the
// ws_server reply hook EXACTLY ONCE for the life of the process.
//
// FINDINGS §5 (:514): a disable→enable cycle self-deletes task_remote (the
// DISABLED case calls vTaskDelete + clears s_task), and re-enable used to
// re-run remote_client_init, which recreated s_tx_queue / s_rx_queue /
// s_remote_evt over the still-live old handles → 2 queues + 1 event group
// leaked per cycle. The kernel objects are now created here once, guarded by
// s_objects_ready, and REUSED across every task respawn. s_remote_evt in
// particular must be stable: app_main captured it via `extern` to post WIFI
// bits, so recreating it would also strand those posts on a dead handle.
static bool s_objects_ready = false;

static void remote_objects_init_once(void) {
    if (s_objects_ready) return;
    s_tx_queue   = xQueueCreate(CONFIG_ZHAC_REMOTE_TX_QUEUE_DEPTH, sizeof(TxItem));
    s_rx_queue   = xQueueCreate(4, sizeof(RxItem));
    s_remote_evt = xEventGroupCreate();

    // Register the ws_server reply hook so dispatch_envelope replies
    // for the REMOTE_VIRTUAL_FD sentinel route here. (Idempotent target,
    // but registered once alongside the objects regardless.)
    ws_server_register_reply_hook(REMOTE_VIRTUAL_FD, &remote_client_send_reply);

    s_objects_ready = (s_tx_queue && s_rx_queue && s_remote_evt);
}

// Spawn task_remote if it is not already alive. Respawnable: the DISABLED
// state deletes the task and clears s_task, and re-enable calls this to bring
// it back. Does NOT recreate kernel objects (see remote_objects_init_once).
static void remote_task_spawn(void) {
    if (s_task) return;
    xTaskCreatePinnedToCore(task_remote_body, "task_remote",
        6 * 1024, nullptr, tskIDLE_PRIORITY + 3, &s_task, 0);
}

extern "C" void remote_client_init(void) {
    // Boot-time NVS load. This runs on app_main (single-threaded — task_remote
    // does not exist yet), so it is safe to populate s_cfg directly here; the
    // concurrent-read hazard (FINDINGS :536) only exists once the task is live,
    // which is why the re-enable path defers the reload via EVB_RELOAD_CFG.
    if (!remote_nvs_load(&s_cfg.enabled,
                          s_cfg.url,   sizeof(s_cfg.url),
                          s_cfg.token, sizeof(s_cfg.token),
                          s_cfg.devid, sizeof(s_cfg.devid))) {
        ESP_LOGW(TAG, "NVS load failed; remote stays DISABLED");
        return;
    }

    remote_objects_init_once();
    if (!s_objects_ready) {
        ESP_LOGE(TAG, "remote object alloc failed; remote stays DISABLED");
        return;
    }

    // Wire wifi event group bridge: caller registers WIFI_EVENT /
    // IP_EVENT handlers in app_main that set our EVB_WIFI_UP /
    // EVB_WIFI_DOWN bits (see Task 11).

    remote_task_spawn();

    if (s_cfg.enabled && s_cfg.url[0] && s_cfg.token[0]) {
        xEventGroupSetBits(s_remote_evt, EVB_ENABLE);
    }
}

extern "C" void remote_client_enable(void) {
    // FINDINGS §5 (:536): do NOT load NVS into s_cfg here. This runs on the API
    // caller's task and a live task_remote may be reading s_cfg.url/.token
    // concurrently. Instead ensure the kernel objects + task exist, then ask
    // the remote task to reload its own s_cfg via EVB_RELOAD_CFG. The task is
    // the sole writer/reader of s_cfg. api_remote_connect already persisted the
    // new url/token to NVS before calling us, so the deferred reload sees them.
    remote_objects_init_once();
    if (!s_objects_ready) return;   // alloc failed at boot; nothing to enable

    remote_task_spawn();   // respawn if a prior disable self-deleted the task

    // RELOAD_CFG must be processed before the connect that EVB_ENABLE drives —
    // the task handles RELOAD_CFG at the top of its loop, ahead of any step().
    xEventGroupSetBits(s_remote_evt, EVB_RELOAD_CFG | EVB_ENABLE);
}

extern "C" void remote_client_disable(bool forget_creds) {
    if (forget_creds) {
        remote_nvs_forget_creds();
        // KNOWN cold-path race (FINDINGS §5 residual): this clears s_cfg on the API
        // task, not task_remote — symmetric to the enable() torn-cred hazard, but here
        // DISABLE drives teardown (stop_ws_client + vTaskDelete) so the worst case is an
        // empty-URL read on a connection already dying. Future: route via an
        // EVB_FORGET_CFG bit like EVB_RELOAD_CFG so only task_remote touches s_cfg.
        s_cfg.url[0]   = 0;
        s_cfg.token[0] = 0;
    }
    s_cfg.enabled = false;
    remote_nvs_save(false, "", "", "");
    if (s_remote_evt) xEventGroupSetBits(s_remote_evt, EVB_DISABLE);
}
