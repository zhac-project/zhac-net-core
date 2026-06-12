// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "s3_internal.h"
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ws_server.h"
#include "remote_client.h"  // remote_client_publish_event (inline no-op when Kconfig off)
#include "mqtt_gw.h"
#include "ArduinoJson.h"
#include "api_handlers.h"

static const char* TAG = "ws_bridge";

// ── Envelope dispatch table ─────────────────────────────────────────────
// Maps WS command names → shared `api_*` handlers. The transport layer
// (this file) is the only thing that knows about the JSON envelope
// `{id, cmd, args}` ↔ `{id, ok, data|err}`. Handlers themselves are
// transport-agnostic — REST wrappers in `rest_*.cpp` call exactly the
// same functions.
//
// S-F4 (docs/OPTIMIZATIONS.md): the table is generated from the
// shared `api_routes.def` X-macro. Same source feeds the REST
// dispatcher in `rest_ops.cpp`, so adding a handler is one row in
// `api_routes.def` instead of two-place editing.
struct WsCmd {
    const char* name;
    ApiStatus (*fn)(const char*, size_t, char*, size_t, size_t*);
};
static const WsCmd kWsCmds[] = {
    #define ZHAC_API_ROUTE(ws_cmd, fn, method, uri)  { ws_cmd, fn },
    #include "api_routes.def"
};

static const WsCmd* ws_lookup(const char* name) {
    for (size_t i = 0; i < sizeof(kWsCmds) / sizeof(kWsCmds[0]); i++) {
        if (strcmp(kWsCmds[i].name, name) == 0) return &kWsCmds[i];
    }
    return nullptr;
}

// `id` may be any JSON value; we pass it through verbatim by quoting if
// it was a string. Most clients will send an int.
static void send_envelope_error(int fd, JsonVariantConst id_var,
                                  const char* err) {
    char buf[256];
    int n;
    if (id_var.is<int>()) {
        n = snprintf(buf, sizeof(buf),
                     "{\"id\":%d,\"ok\":false,\"err\":\"%s\"}",
                     id_var.as<int>(), err);
    } else if (id_var.is<const char*>()) {
        n = snprintf(buf, sizeof(buf),
                     "{\"id\":\"%s\",\"ok\":false,\"err\":\"%s\"}",
                     id_var.as<const char*>(), err);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"id\":null,\"ok\":false,\"err\":\"%s\"}", err);
    }
    if (n > 0 && (size_t)n < sizeof(buf)) ws_server_reply(fd, buf, (size_t)n);
}

// Dispatch one envelope. Allocates response + envelope buffers in PSRAM
// since `logs.get` alone needs 32 KB and stack is precious in TaskHTTP.
static void dispatch_envelope(int fd, JsonDocument& doc) {
    JsonVariantConst id_var = doc["id"];
    const char* cmd = doc["cmd"] | "";
    if (!cmd[0]) {
        send_envelope_error(fd, id_var, "missing cmd");
        return;
    }
    // F18/A2 (FINDINGS.md): when auth is enabled, a real WS socket (fd >= 0)
    // must authenticate with a first {"cmd":"auth","args":{"token":"..."}}
    // message before any other command runs — the token rides a WS frame, not
    // the URL. The remote/cloud sentinel fd (< 0) has its own auth + allow-list
    // and bypasses this gate.
    if (s_auth_enabled && fd >= 0 && !ws_server_fd_is_authed(fd)) {
        if (strcmp(cmd, "auth") == 0) {
            if (auth_check_token(doc["args"]["token"] | "")) {
                ws_server_fd_set_authed(fd);
                char ok[96];
                int n = id_var.is<const char*>()
                    ? snprintf(ok, sizeof(ok), "{\"id\":\"%s\",\"ok\":true}",
                               id_var.as<const char*>())
                    : snprintf(ok, sizeof(ok), "{\"id\":null,\"ok\":true}");
                if (n > 0 && (size_t)n < sizeof(ok)) ws_server_reply(fd, ok, (size_t)n);
            } else {
                send_envelope_error(fd, id_var, "auth failed");
            }
            return;
        }
        send_envelope_error(fd, id_var, "auth required");
        return;
    }
    const WsCmd* entry = ws_lookup(cmd);
    if (!entry) {
        send_envelope_error(fd, id_var, "unknown cmd");
        return;
    }
    ESP_LOGI(TAG, "ws dispatch cmd=%s", cmd);

    // Serialize args object to a string body so the api_* helper sees
    // the same input it would from a REST POST/PUT. measureJson first:
    // serializeJson silently truncates at the buffer cap, which turned an
    // oversized args object into corrupt JSON for the handler — reject it
    // with an explicit error instead.
    char args_buf[2048];
    size_t args_len = 0;
    if (doc["args"].is<JsonObjectConst>()) {
        if (measureJson(doc["args"]) >= sizeof(args_buf)) {
            send_envelope_error(fd, id_var, "args too large");
            return;
        }
        args_len = serializeJson(doc["args"], args_buf, sizeof(args_buf));
    }

    // Response + envelope live in PSRAM. `logs.get` alone needs ~32 KB;
    // most commands fit comfortably in 8 KB. Starting small keeps
    // per-command allocation churn low during bootstrap bursts (8+
    // commands hit the backend the moment the SPA connects). If the
    // handler's output would overflow the cap we bail with an error.
    //
    // `device.list` is now PAGED + reassembled into one full
    // `{"devices":[...]}` (HOTFIX) — a large fleet (200 devices ≈ 64 KB
    // worst case) blows past 8 KB, so give it the big bucket too; ~40 KB
    // covers ~125 devices, beyond which the accumulator truncates with a
    // logged warning rather than emitting broken JSON.
    const bool is_logs = (strcmp(cmd, "logs.get") == 0);
    const bool is_devlist = (strcmp(cmd, "device.list") == 0);
    const size_t kRspCap      = (is_logs || is_devlist) ? kDevListTransportCap : (8 * 1024);
    const size_t kEnvelopeCap = kRspCap + 1024;
    char* rsp_buf = static_cast<char*>(heap_caps_malloc(kRspCap, MALLOC_CAP_SPIRAM));
    char* env_buf = static_cast<char*>(heap_caps_malloc(kEnvelopeCap, MALLOC_CAP_SPIRAM));
    if (!rsp_buf || !env_buf) {
        heap_caps_free(rsp_buf);
        heap_caps_free(env_buf);
        send_envelope_error(fd, id_var, "alloc failed");
        return;
    }

    size_t rsp_len = 0;
    ApiStatus st = entry->fn(args_buf, args_len, rsp_buf, kRspCap, &rsp_len);

    if (st != API_OK) {
        // Default per-status text — used when the handler didn't fill
        // rsp_buf with a JSON body carrying a richer message.
        const char* err = "internal error";
        if      (st == API_BAD_REQUEST)        err = "bad request";
        else if (st == API_NOT_FOUND)          err = "not found";
        else if (st == API_METHOD_NOT_ALLOWED) err = "method not allowed";

        // Some handlers (api_rule_create / api_rule_update on
        // ParseResult::ERR_*) put the DSL parse error verbatim into
        // rsp_buf. Prefer that detail over the generic status label
        // so the SPA shows e.g. "cron expression too long (max 31)"
        // instead of "bad request".
        char detail[160] = {};
        if (rsp_len > 0 && rsp_len < kRspCap) {
            JsonDocument ed;
            if (deserializeJson(ed, rsp_buf, rsp_len) ==
                    DeserializationError::Ok) {
                const char* e = ed["err"] | (const char*)nullptr;
                if (e && e[0]) {
                    strncpy(detail, e, sizeof(detail) - 1);
                    err = detail;
                }
            }
        }
        send_envelope_error(fd, id_var, err);
        heap_caps_free(rsp_buf);
        heap_caps_free(env_buf);
        return;
    }

    // Build envelope. id passes through verbatim — int or string.
    int n;
    if (id_var.is<int>()) {
        n = snprintf(env_buf, kEnvelopeCap,
                     "{\"id\":%d,\"ok\":true,\"data\":%.*s}",
                     id_var.as<int>(), (int)rsp_len, rsp_buf);
    } else if (id_var.is<const char*>()) {
        n = snprintf(env_buf, kEnvelopeCap,
                     "{\"id\":\"%s\",\"ok\":true,\"data\":%.*s}",
                     id_var.as<const char*>(), (int)rsp_len, rsp_buf);
    } else {
        n = snprintf(env_buf, kEnvelopeCap,
                     "{\"id\":null,\"ok\":true,\"data\":%.*s}",
                     (int)rsp_len, rsp_buf);
    }
    if (n > 0 && (size_t)n < kEnvelopeCap) {
        ws_server_reply(fd, env_buf, (size_t)n);
    } else {
        send_envelope_error(fd, id_var, "response too large");
    }
    heap_caps_free(rsp_buf);
    heap_caps_free(env_buf);
}

// Escape a byte array as a JSON string value (without surrounding quotes).
// Writes into out[0..out_cap-1], null-terminates, returns written length.
static size_t json_escape(const char* src, int src_len, char* out, size_t out_cap) {
    size_t wi = 0;
    for (int i = 0; i < src_len && wi + 2 < out_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (wi + 3 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = (char)c;
        } else if (c == '\n') {
            if (wi + 3 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 'n';
        } else if (c == '\r') {
            if (wi + 3 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 'r';
        } else if (c == '\t') {
            if (wi + 3 > out_cap) break;
            out[wi++] = '\\'; out[wi++] = 't';
        } else if (c < 0x20) {
            // Skip other control characters
        } else {
            out[wi++] = (char)c;
        }
    }
    out[wi] = '\0';
    return wi;
}

// ── ws_event_broadcast ──────────────────────────────────────────────────
//
// Wraps `payload_json` in `{"event":"<name>","data":<payload>}` and
// fans it out to every connected WS client. The scratch buffer is a
// file-static PSRAM slab protected by a mutex — callers come from
// arbitrary tasks (hap_bridge, OTA paths, etc.), some of which
// (TaskGPIORst, TaskAlertPrst) have tight stacks where a 1 KB local
// was enough to trip the canary watchpoint.
//
// Sized to fit the largest payload any producer emits: hap_bridge's
// attr.bulk coalescer batches up to BULK_COALESCE_CAP (4096) bytes, and the
// old 2048 B slab silently replaced coalesced batches >~2020 B with
// `data:null` — live updates lost exactly when traffic was heaviest.
// +128 covers the `{"event":"...","data":}` framing.
static SemaphoreHandle_t s_evt_mutex = nullptr;
EXT_RAM_BSS_ATTR static char s_evt_buf[BULK_COALESCE_CAP + 128];

static void evt_mutex_init_once() {
    if (!s_evt_mutex) {
        static StaticSemaphore_t s_mbuf;
        s_evt_mutex = xSemaphoreCreateMutexStatic(&s_mbuf);
    }
}

// F33 (FINDINGS.md): dedicated WS-TX worker. ws_server_broadcast loops
// httpd_ws_send_frame_async per fd; that call queues work onto the single httpd
// task, and when a slow client backs that queue up it blocks the CALLER. Running
// it on the producer task (TaskHAP, log pipeline, alerts, event_bus drain) is
// the head-of-line landmine F33 flags. Producers now format a snapshot, hand it
// to this bounded queue, and return; the worker owns the fan-out + frees the
// snapshot. Overflow drops (rather than blocks a producer) — counted, not logged.
struct WsTxItem { char* json; size_t len; };
static constexpr int     WS_TX_QUEUE_DEPTH = 24;
static QueueHandle_t          s_ws_tx_q     = nullptr;
static std::atomic<uint32_t>  s_ws_tx_drops{0};   // best-effort drop counter (multi-producer)

static void ws_tx_worker(void*) {
    WsTxItem item;
    for (;;) {
        if (xQueueReceive(s_ws_tx_q, &item, portMAX_DELAY) == pdTRUE && item.json) {
            ws_server_broadcast(item.json, item.len);
            free(item.json);
        }
    }
}

void ws_bridge_tx_init() {
    if (s_ws_tx_q) return;   // boot-time, single-threaded — idempotent guard
    s_ws_tx_q = xQueueCreate(WS_TX_QUEUE_DEPTH, sizeof(WsTxItem));
    if (s_ws_tx_q) {
        xTaskCreate(ws_tx_worker, "ws_tx", 3072, nullptr, 4, nullptr);
    }
}

// Queue a heap frame for the TX worker, taking ownership of `buf` (the
// worker frees it; on drop we free it here). Never blocks, NEVER logs —
// this runs on the log pipeline among other places, and any ESP_LOG here
// would re-enter it.
static void tx_enqueue_owned(char* buf, size_t len) {
    WsTxItem item{ buf, len };
    if (!s_ws_tx_q || xQueueSend(s_ws_tx_q, &item, 0) != pdTRUE) {
        s_ws_tx_drops.fetch_add(1, std::memory_order_relaxed);
        free(buf);
    }
}

void ws_bridge_broadcast_enqueue(const char* json, size_t len) {
    if (!json || len == 0) return;
    // PSRAM-preferred snapshot: queue depth (24) × frame size would otherwise
    // nibble at tight internal DRAM under bursts.
    char* copy = static_cast<char*>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) copy = static_cast<char*>(malloc(len));
    if (!copy) {
        s_ws_tx_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    memcpy(copy, json, len);
    tx_enqueue_owned(copy, len);
}

uint32_t ws_bridge_tx_drops() {
    return s_ws_tx_drops.load(std::memory_order_relaxed);
}

void ws_event_broadcast(const char* name, const char* payload_json, size_t payload_len) {
    evt_mutex_init_once();
    if (xSemaphoreTake(s_evt_mutex, portMAX_DELAY) != pdTRUE) return;

    // Format under the mutex (s_evt_buf is shared scratch), then snapshot
    // length and release before calling ws_server_broadcast — that call
    // takes the ws_server fd-table lock and may block in
    // httpd_ws_send_frame_async if a client is slow. Holding s_evt_mutex
    // across a slow send is a priority-inversion landmine that stalls
    // every event producer (TaskHAP, log pipeline, alerts). Static slab
    // means we cannot let a second producer race the formatting half;
    // copy onto the stack so the broadcast half can run unlocked.
    // (WEB-F2 in docs/FINDINGS.md.)
    int n;
    if (payload_json && payload_len > 0) {
        n = snprintf(s_evt_buf, sizeof(s_evt_buf),
                     "{\"event\":\"%s\",\"data\":%.*s}",
                     name, (int)payload_len, payload_json);
    } else {
        n = snprintf(s_evt_buf, sizeof(s_evt_buf),
                     "{\"event\":\"%s\",\"data\":null}", name);
    }
    if (n <= 0 || (size_t)n >= sizeof(s_evt_buf)) {
        // Now-unreachable safety: the slab fits BULK_COALESCE_CAP, the largest
        // payload any producer emits. Logging here is fine — this function is
        // no longer on the log pipeline (the WS log sink enqueues directly).
        ESP_LOGW(TAG, "event '%s' payload %u B overflows %u B envelope — data:null",
                 name, (unsigned)payload_len, (unsigned)sizeof(s_evt_buf));
        n = snprintf(s_evt_buf, sizeof(s_evt_buf),
                     "{\"event\":\"%s\",\"data\":null,\"trunc\":true}", name);
    }

    // Heap-allocate the snapshot rather than putting 2 KB on stack —
    // this function gets called from the master_send dispatch chain
    // (httpd thread -> api_rule_list -> hap_roundtrip_v2 -> SPI exchange ->
    //  peer BULK callback -> ws_event_broadcast -> ws_server_broadcast ->
    //  lwip_send), and the deep call chain plus a 2 KB stack local
    // tripped the stack canary on the HTTP server task.
    char*    local    = nullptr;
    size_t   send_len = 0;
    if (n > 0 && (size_t)n < sizeof(s_evt_buf)) {
        local = static_cast<char*>(malloc((size_t)n));
        if (local) {
            send_len = (size_t)n;
            memcpy(local, s_evt_buf, send_len);
        }
    }
    xSemaphoreGive(s_evt_mutex);

    // F33: hand the local fan-out to the WS-TX worker so a slow socket can't
    // back up this producer. Ownership of `local` transfers to the queue; the
    // worker frees it. On a full / not-yet-started queue, drop + count rather
    // than block (tx_enqueue_owned never logs).
    if (local) tx_enqueue_owned(local, send_len);

    // Mirror to remote channel if enabled + allow-listed. Non-blocking
    // (atomic-load + branch when on but not READY; inline no-op when the remote
    // feature is compiled out) — stays inline.
    remote_client_publish_event(name, payload_json, payload_len);
}

// Called from httpd task context — keep work minimal, no blocking calls.
// `fd` identifies the client socket so envelope responses can be sent
// back point-to-point via `ws_server_reply()`.
//
// Frame routing:
//   - Envelope-shaped frames (`{"id":..,"cmd":..,"args":..}`) → new SPA
//     command surface; dispatch through the api_* registry.
//   - Legacy ad-hoc frames (no `id` field) → preserved for the existing
//     vanilla-JS UI's `set_attr` / `get_devices` paths until the SPA
//     migration is complete.
void on_ws_rx(int fd, const char* data, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;

    // Envelope mode: presence of `id` distinguishes SPA frames from
    // legacy ones. Both intact during Phase 2/3 of the migration.
    if (!doc["id"].isNull()) {
        dispatch_envelope(fd, doc);
        return;
    }

    ESP_LOGW(TAG, "WS cmd: unknown command \"%s\"", doc["cmd"] | "");
}

// Called from the esp-mqtt event task — keep it fast, no blocking.
void on_mqtt_rx(const char* topic, int topic_len,
                const char* data,  int data_len) {
    // Check for interview trigger: <root>/devices/0x.../interview
    {
        char prefix[48];
        int pfx_len = mqtt_gw_format_topic(prefix, sizeof(prefix), "devices/");
        const char* suffix = "/interview";
        int sfx_len = (int)strlen(suffix);
        if (pfx_len > 0 &&
            topic_len > pfx_len + sfx_len &&
            strncmp(topic, prefix, pfx_len) == 0 &&
            strncmp(topic + topic_len - sfx_len, suffix, sfx_len) == 0) {
            char ieee_str[20] = {};
            int seg_len = topic_len - pfx_len - sfx_len;
            if (seg_len > 0 && seg_len < (int)sizeof(ieee_str)) {
                memcpy(ieee_str, topic + pfx_len, (size_t)seg_len);
                uint64_t ieee = parse_ieee(ieee_str);
                if (ieee) {
                    uint8_t hap_buf[48];
                    uint16_t hap_len = 0;
                    if (hap_json_encode_interview_req(hap_buf, sizeof(hap_buf), &hap_len, ieee)) {
                        hap_send(HapMsgType::INTERVIEW_REQ, hap_buf, hap_len);
                    }
                    ESP_LOGI(TAG, "MQTT interview trigger for 0x%016llx", (unsigned long long)ieee);
                }
            }
        }
    }

    // F-05 fix: always escape payload as a JSON string. The previous
    // "splice verbatim if looks-like-JSON" path let a hostile broker or
    // buggy publisher inject extra envelope fields (e.g. truncated `{`
    // or `}` followed by a second key) into every WS client's stream.
    // The SPA does JSON.parse(msg.data) regardless, so escaping costs
    // one decode hop on the client and removes the injection vector.
    char buf[768];
    char topic_esc[256];
    char data_esc[512];

    json_escape(topic, topic_len, topic_esc, sizeof(topic_esc));
    json_escape(data, data_len, data_esc, sizeof(data_esc));

    int n = snprintf(buf, sizeof(buf),
        "{\"type\":\"mqtt\",\"topic\":\"%s\",\"data\":\"%s\"}",
        topic_esc, data_esc);

    if (n > 0 && n < (int)sizeof(buf)) {
        // Runs on the esp-mqtt event task — enqueue to the WS-TX worker
        // instead of fanning out here, so a slow WS client can't stall the
        // MQTT event loop.
        ws_bridge_broadcast_enqueue(buf, (size_t)n);
        ESP_LOGD(TAG, "MQTT→WS topic=%s len=%d", topic_esc, n);
    }

    // Forward to P4 so Lua scripts and simple_rules can react.
    char t_buf[64];
    char p_buf[256];
    int t_copy = (topic_len < (int)sizeof(t_buf) - 1) ? topic_len : (int)sizeof(t_buf) - 1;
    int p_copy = (data_len  < (int)sizeof(p_buf) - 1) ? data_len  : (int)sizeof(p_buf) - 1;
    memcpy(t_buf, topic, t_copy); t_buf[t_copy] = '\0';
    memcpy(p_buf, data,  p_copy); p_buf[p_copy] = '\0';

    uint8_t mqtt_fwd_buf[384];
    uint16_t mqtt_fwd_len = 0;
    if (hap_json_encode_mqtt_msg_in(mqtt_fwd_buf, sizeof(mqtt_fwd_buf), &mqtt_fwd_len,
                                    t_buf, p_buf)) {
        hap_send(HapMsgType::MQTT_MSG_IN, mqtt_fwd_buf, mqtt_fwd_len);
    }
}

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
// Exposed wrapper for the remote_client component. Delegates to the
// file-static dispatch_envelope above. Lives here (and not in
// remote_client) because dispatch_envelope's compile unit owns the
// ws envelope contract and we don't want to leak the static symbol.
//
// The matching decl is provided in s3_internal.h (and forward-declared
// directly inside remote_client.cpp to avoid a cross-component header
// reach into `main/`'s private headers).
extern "C" void dispatch_envelope_for_remote(int fd, JsonDocument& doc) {
    dispatch_envelope(fd, doc);
}
#endif // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
