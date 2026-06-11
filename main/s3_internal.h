// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "hap_session.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "esp_http_server.h"
#include "wifi_mgr.h"
#include "api_handlers.h"

// Canonical "OK" ack body — every api_* handler that just needs to
// report success emits this exact JSON.
static inline size_t api_write_ok(char* buf, size_t cap) {
    return (size_t)snprintf(buf, cap, "{\"ok\":true}");
}

// Canonical bare-failure ack body — paired with api_write_ok() for
// handlers that have no extra fields to surface beyond a boolean.
static inline size_t api_write_err(char* buf, size_t cap) {
    return (size_t)snprintf(buf, cap, "{\"ok\":false}");
}

// ── Timing constants ─────────────────────────────────────────────────────
static constexpr uint32_t P4_ROUNDTRIP_TIMEOUT_MS    = 5000;   // REST handler wait for P4 round-trip
static constexpr uint32_t P4_OTA_RESPONSE_TIMEOUT_MS = 30000;  // wait for P4 OTA_STATUS after flash
static constexpr uint32_t HAP_HEARTBEAT_INTERVAL_MS  = 5000;   // S3→P4 heartbeat cadence
static constexpr uint32_t STACK_MON_INITIAL_DELAY_MS = 15000;  // let all tasks start before first HWM log

// ── Shared buffer sizing ──────────────────────────────────────────────────
// Cap of hap_bridge.cpp's attr.bulk coalescer (largest single event payload
// the bridge ever emits). Exported so ws_bridge can size its event envelope
// to fit a maximal coalesced batch — a smaller envelope silently replaced
// big batches with `data:null`, losing live updates under load.
static constexpr size_t BULK_COALESCE_CAP = 4096;

// ── CORS ──────────────────────────────────────────────────────────────────
// Default: same-origin only (no Access-Control-Allow-Origin emitted →
// browsers block cross-origin requests). The WebUI is served from the
// device itself, so same-origin is sufficient for normal use.
//
// For dev tools running on a different origin (e.g. a local vite
// server hitting the device API), set CONFIG_ZHAC_CORS_WILDCARD=y in
// sdkconfig. This emits `Access-Control-Allow-Origin: *` — do NOT
// enable in production images, it lets any web page with a valid
// `X-Api-Key` drive the controller.
#ifdef CONFIG_ZHAC_CORS_WILDCARD
#define SET_CORS_HEADERS(req) \
    do { \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Origin",  "*"); \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Headers", \
                           "Content-Type, X-Api-Key"); \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Methods", \
                           "GET, POST, PUT, DELETE, OPTIONS"); \
    } while (0)
#else
#define SET_CORS_HEADERS(req) \
    do { \
        /* Same-origin: no Access-Control-Allow-Origin header — */ \
        /* browsers enforce the default same-origin policy. */ \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Headers", \
                           "Content-Type, X-Api-Key"); \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Methods", \
                           "GET, POST, PUT, DELETE, OPTIONS"); \
    } while (0)
#endif

// ── API token ─────────────────────────────────────────────────────────────
extern char s_api_token[33];
extern bool s_auth_enabled;

// ── OTA signalling ────────────────────────────────────────────────────────
extern char              s_ota_url[256];
extern SemaphoreHandle_t s_ota_sem;

// ── P4 state cache ────────────────────────────────────────────────────────
extern std::atomic<uint16_t> s_p4_device_count;
extern std::atomic<uint32_t> s_p4_uptime_s;
extern std::atomic<uint32_t> s_p4_psram_free;
extern std::atomic<uint32_t> s_p4_psram_total;
extern std::atomic<uint8_t>  s_p4_cpu_pct_c0;
extern std::atomic<uint8_t>  s_p4_cpu_pct_c1;
extern std::atomic<uint32_t> s_p4_proto_mask;
extern char                  s_p4_fw_ver[16];
// Extended memory diagnostics forwarded from P4's HEARTBEAT payload.
extern std::atomic<uint32_t> s_p4_heap_free;
extern std::atomic<uint32_t> s_p4_heap_min_free;
extern std::atomic<uint32_t> s_p4_internal_free;
extern std::atomic<uint32_t> s_p4_internal_min_free;
extern std::atomic<uint32_t> s_p4_internal_largest_block;
extern std::atomic<uint32_t> s_p4_psram_min_free;
extern std::atomic<uint32_t> s_p4_psram_largest_block;
extern std::atomic<uint32_t> s_p4_task_stack_hwm_bytes;

// Copy the most recent cached P4 Prometheus snapshot into `out`.
// Returns bytes written (excluding NUL); 0 when no snapshot received
// yet or when the cache hasn't been initialised. Output is always
// NUL-terminated when `max > 0`.
size_t hap_bridge_copy_p4_metrics(char* out, size_t max);

// True when no HEARTBEAT has been received from P4 for 3 intervals (15 s).
// Set by task_hap timeout path, cleared on next HEARTBEAT.
bool hap_bridge_is_p4_unresponsive();

// Copy the latest P4 firmware-version string into `out` under the
// metrics mutex. `cap` should be ≥ 16 (sizeof s_p4_fw_ver). Output is
// always NUL-terminated.
void hap_bridge_copy_p4_fw_ver(char* out, size_t cap);

// ── P4 OTA ────────────────────────────────────────────────────────────────
extern SemaphoreHandle_t s_p4ota_sem;
extern SemaphoreHandle_t s_p4ota_rsp_sem;
extern SemaphoreHandle_t s_p4ota_ckpt_rsp_sem;
extern char              s_p4ota_url[256];
extern HapOtaStatus      s_p4ota_status;
extern uint32_t          s_p4ota_ckpt_offset;

// ── Alert ring buffer ─────────────────────────────────────────────────────
#define ALERT_LOG_MAX 32
struct AlertLogEntry { HapAlert alert; uint32_t wall_ts; };
extern AlertLogEntry     s_alert_log[ALERT_LOG_MAX];
extern uint8_t           s_alert_log_head;
extern uint8_t           s_alert_log_count;
extern SemaphoreHandle_t s_alert_log_mutex;

// ── Cached NVS handles (opened once at init, kept open — S4) ─────────────
extern nvs_handle_t s_nvs_zhac_opt;   // "zhac_opt" — per-device options

// ── S3 state ──────────────────────────────────────────────────────────────
extern std::atomic<bool> s_synced;
extern std::atomic<bool> s_wifi_connected;
extern bool              s_metrics_enabled;
extern bool              s_ap_disabled;

// Call an api_* handler and forward the result as JSON. Maps
// API_BAD_REQUEST to 400, any other non-OK to 500. Caller provides the
// response buffer (stack or heap). Returns ESP_OK after sending the
// response; err_label is echoed in the error body on failure.
esp_err_t rest_api_reply(httpd_req_t* req, ApiHandlerFn fn,
                          const char* body, size_t body_len,
                          char* buf, size_t cap, const char* err_label);

// F33 (FINDINGS.md): PSRAM-preferred allocation for per-request large buffers
// (free() with free()), and body read with an early Content-Length 413 cap.
// rest_body_recv returns bytes read, or -1 after itself sending 413/400 —
// callers then just `return ESP_OK`. See rest_ops.cpp for full contracts.
void* rest_big_alloc(size_t n);
int   rest_body_recv(httpd_req_t* req, char* buf, size_t cap);

// ── Macros shared across files ────────────────────────────────────────────

#define REQUIRE_AUTH(req) \
    do { \
        SET_CORS_HEADERS(req); \
        if (!check_auth(req)) { \
            httpd_resp_set_status((req), "401 Unauthorized"); \
            httpd_resp_set_type((req), "application/json"); \
            httpd_resp_sendstr((req), "{\"error\":\"unauthorized\"}"); \
            return ESP_OK; \
        } \
    } while (0)

#define RATE_LIMIT(req, last_us, cooldown_us) \
    do { \
        int64_t _now = esp_timer_get_time(); \
        if (_now - (last_us) < (cooldown_us)) { \
            httpd_resp_set_status((req), "429 Too Many Requests"); \
            httpd_resp_set_type((req), "application/json"); \
            httpd_resp_sendstr((req), "{\"error\":\"rate limited\"}"); \
            return ESP_OK; \
        } \
        (last_us) = _now; \
    } while (0)

#define TRY_CHANNEL(req, mutex) \
    do { \
        if (xSemaphoreTake((mutex), 0) != pdTRUE) { \
            httpd_resp_set_status((req), "429 Too Many Requests"); \
            httpd_resp_set_type((req), "application/json"); \
            httpd_resp_sendstr((req), "{\"error\":\"channel busy\"}"); \
            httpd_resp_set_hdr((req), "Retry-After", "1"); \
            return ESP_OK; \
        } \
    } while (0)

// ── Functions defined in main.cpp used elsewhere ─────────────────────────
uint64_t parse_ieee(const char* s);
bool     check_auth(httpd_req_t* req);
bool     auth_check_token(const char* token);   // F18: WS first-message auth
void     auth_init();

// ── Functions defined in hap_bridge.cpp ──────────────────────────────────
void alert_log_load();
void alert_log_schedule_persist();
void alert_persist_task_init();
void hap_send(HapMsgType type, const uint8_t* payload, uint16_t payload_len,
              uint8_t flags = HAP_FLAG_NO_ACK);
// Per-request-seq response correlation. Caller owns rsp_buf — no shared
// scratch, no per-type mutex. Concurrent calls proceed in parallel up to
// the bridge's waiter-slot cap (returns false on slot starvation within
// timeout_ms). Returns true with `*rsp_len_out` set on success; truncates
// + LOGW if the response payload exceeds rsp_cap.
bool hap_roundtrip_v2(HapMsgType type,
                      const uint8_t* req, uint16_t req_len,
                      char* rsp_buf, size_t rsp_cap, size_t* rsp_len_out,
                      uint32_t timeout_ms = P4_ROUNDTRIP_TIMEOUT_MS);
void send_sync_req();
void task_hap(void*);

// ── Functions defined in rest_devices.cpp ────────────────────────────────
esp_err_t handle_get_devices(httpd_req_t* req);
esp_err_t handle_get_device_by_id(httpd_req_t* req);
esp_err_t handle_post_device_bind(httpd_req_t* req);
esp_err_t handle_delete_device(httpd_req_t* req);
esp_err_t handle_put_device(httpd_req_t* req);
esp_err_t handle_post_interview(httpd_req_t* req);
// Single POST /api/devices/* dispatcher — ESP-IDF httpd only supports
// trailing-star wildcards, so `/api/devices/*/bind` etc. don't match.
// This one handler inspects the URI suffix and delegates.
esp_err_t handle_post_device_subroute(httpd_req_t* req);

// ── Functions defined in rest_rules.cpp ──────────────────────────────────
esp_err_t handle_get_rules(httpd_req_t* req);
esp_err_t handle_post_rules(httpd_req_t* req);
esp_err_t handle_delete_rules(httpd_req_t* req);
esp_err_t handle_put_rules(httpd_req_t* req);
esp_err_t handle_put_rule_dsl(httpd_req_t* req);
esp_err_t handle_get_scripts(httpd_req_t* req);
esp_err_t handle_get_script_by_id(httpd_req_t* req);
esp_err_t handle_delete_script(httpd_req_t* req);
esp_err_t handle_post_script(httpd_req_t* req);
esp_err_t handle_post_scripts_bulk(httpd_req_t* req);
esp_err_t handle_get_groups(httpd_req_t* req);
esp_err_t handle_post_groups(httpd_req_t* req);
esp_err_t handle_get_group_by_id(httpd_req_t* req);
esp_err_t handle_put_group(httpd_req_t* req);
esp_err_t handle_delete_group(httpd_req_t* req);
esp_err_t handle_post_group_cmd(httpd_req_t* req);

// ── Functions defined in rest_ops.cpp ────────────────────────────────────
esp_err_t handle_get_status(httpd_req_t* req);
esp_err_t handle_get_metrics(httpd_req_t* req);
esp_err_t handle_get_alerts(httpd_req_t* req);
esp_err_t handle_get_diagnostics_unhandled(httpd_req_t* req);
esp_err_t handle_post_permit_join(httpd_req_t* req);
esp_err_t handle_post_zigbee_reset(httpd_req_t* req);
esp_err_t handle_post_zigbee_settings(httpd_req_t* req);
esp_err_t handle_post_ota(httpd_req_t* req);
esp_err_t handle_post_p4_ota(httpd_req_t* req);
esp_err_t handle_post_wifi(httpd_req_t* req);
esp_err_t handle_get_wifi_scan(httpd_req_t* req);
esp_err_t handle_delete_wifi(httpd_req_t* req);
esp_err_t handle_get_wifi_status(httpd_req_t* req);
esp_err_t handle_post_settings(httpd_req_t* req);
esp_err_t handle_get_static(httpd_req_t* req);
void      task_http(void*);

// ── Functions defined in ws_bridge.cpp ───────────────────────────────────
void on_ws_rx(int fd, const char* data, size_t len);
// F33: start the WS broadcast TX worker (decouples event producers from the
// per-client fan-out). Call once at boot before RX can trigger broadcasts.
void ws_bridge_tx_init();
void on_mqtt_rx(const char* topic, int topic_len, const char* data, int data_len);

// Broadcast a server-initiated event to every connected WS client as
// `{"event":"<name>","data":<payload_json>}`. `payload_json` must be a
// fragment of valid JSON (object, array, or scalar). Used by the SPA's
// signal stores to react without polling. Cheap; runs in caller task.
void ws_event_broadcast(const char* name, const char* payload_json, size_t payload_len);

// Copy `json` and hand it to the WS-TX worker queue for fan-out. The ONLY
// sanctioned way to broadcast a pre-formatted frame from an arbitrary task:
// never blocks, never logs (log-pipeline-safe — overflow/OOM drops are
// counted in ws_bridge_tx_drops()). ws_server_broadcast itself must only
// ever be called by the TX worker.
void ws_bridge_broadcast_enqueue(const char* json, size_t len);

// Frames dropped on the WS-TX path (queue full, queue not started, or
// snapshot alloc failure). Surfaced in the status.get payload.
uint32_t ws_bridge_tx_drops();

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
// Implemented in ws_bridge.cpp as a thin wrapper around the file-static
// dispatch_envelope. Lets the remote_client component reuse the same
// envelope dispatch as the local /ws path. ArduinoJson is on the
// component's REQUIRES list so the JsonDocument type is reachable.
#include "ArduinoJson.h"
extern "C" void dispatch_envelope_for_remote(int fd, JsonDocument& doc);
#endif
