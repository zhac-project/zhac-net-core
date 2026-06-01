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
            // Only text frames; binary is unexpected, skip.
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
    // Initial event: ENABLE if cfg says so, otherwise stay DISABLED.
    if (s_cfg.enabled && s_cfg.url[0] && s_cfg.token[0]) {
        auto cur = s_state.load(std::memory_order_relaxed);
        s_state.store(remote_state_next(cur, REMOTE_EV_ENABLE),
                      std::memory_order_release);
    }

    // State-entry detection for AUTHENTICATING (Fix 2).
    static RemoteState last_state = REMOTE_STATE_DISABLED;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_remote_evt,
            EVB_ENABLE | EVB_DISABLE | EVB_WIFI_UP | EVB_WIFI_DOWN |
            EVB_WSS_EVENT | EVB_AUTH_OK | EVB_AUTH_FAIL | EVB_BACKOFF_DONE,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));

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
                if (bits & EVB_WSS_EVENT &&
                    s_ws && !esp_websocket_client_is_connected(s_ws)) {
                    step(REMOTE_EV_WSS_ERROR);
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
                vTaskDelay(pdMS_TO_TICKS(delay * 1000));
                xEventGroupSetBits(s_remote_evt, EVB_BACKOFF_DONE);
                step(REMOTE_EV_BACKOFF_DONE);
                break;
            }
        }

        last_state = cur_state;
    }
}

// ── Public API ───────────────────────────────────────────────────────────

extern "C" void remote_client_init(void) {
    // Load NVS.
    if (!remote_nvs_load(&s_cfg.enabled,
                          s_cfg.url,   sizeof(s_cfg.url),
                          s_cfg.token, sizeof(s_cfg.token),
                          s_cfg.devid, sizeof(s_cfg.devid))) {
        ESP_LOGW(TAG, "NVS load failed; remote stays DISABLED");
        return;
    }

    s_tx_queue   = xQueueCreate(CONFIG_ZHAC_REMOTE_TX_QUEUE_DEPTH, sizeof(TxItem));
    s_rx_queue   = xQueueCreate(4, sizeof(RxItem));
    s_remote_evt = xEventGroupCreate();

    // Register the ws_server reply hook so dispatch_envelope replies
    // for the REMOTE_VIRTUAL_FD sentinel route here.
    ws_server_register_reply_hook(REMOTE_VIRTUAL_FD, &remote_client_send_reply);

    // Wire wifi event group bridge: caller registers WIFI_EVENT /
    // IP_EVENT handlers in app_main that set our EVB_WIFI_UP /
    // EVB_WIFI_DOWN bits (see Task 11).

    xTaskCreatePinnedToCore(task_remote_body, "task_remote",
        6 * 1024, nullptr, tskIDLE_PRIORITY + 3, &s_task, 0);

    if (s_cfg.enabled && s_cfg.url[0] && s_cfg.token[0]) {
        xEventGroupSetBits(s_remote_evt, EVB_ENABLE);
    }
}

extern "C" void remote_client_enable(void) {
    // Reload NVS to pick up url/token changes from api_remote_connect.
    remote_nvs_load(&s_cfg.enabled,
                     s_cfg.url,   sizeof(s_cfg.url),
                     s_cfg.token, sizeof(s_cfg.token),
                     s_cfg.devid, sizeof(s_cfg.devid));

    // If the task isn't alive (was DISABLED at boot), spawn it now.
    if (!s_task) {
        remote_client_init();
        return;
    }
    xEventGroupSetBits(s_remote_evt, EVB_ENABLE);
}

extern "C" void remote_client_disable(bool forget_creds) {
    if (forget_creds) {
        remote_nvs_forget_creds();
        s_cfg.url[0]   = 0;
        s_cfg.token[0] = 0;
    }
    s_cfg.enabled = false;
    remote_nvs_save(false, "", "", "");
    if (s_remote_evt) xEventGroupSetBits(s_remote_evt, EVB_DISABLE);
}
