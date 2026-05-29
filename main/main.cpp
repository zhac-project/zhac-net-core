// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <atomic>
#include <cstdio>
#include <cinttypes>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "hap_master.h"
#include "hap_session.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "metrics/metrics.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"   // F3: verify OTA server cert against CA bundle
#include "esp_http_client.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "tg_gw.h"
#include "esp_sntp.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "ArduinoJson.h"
#include "s3_internal.h"
#include "nvs_helpers.h"
#include "log_ring.h"
#include "task_stacks.h"
#include "remote_client.h"  // inline no-op when Kconfig off

static const char* TAG = "s3_main";

// ── Helpers ───────────────────────────────────────────────────────────────

uint64_t parse_ieee(const char* s) {
    if (!s || *s == '\0') return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char* end;
    uint64_t v = strtoull(s, &end, 16);
    if (end == s || *end != '\0') return 0;
    return v;
}

// ── API authentication ────────────────────────────────────────────────────
char s_api_token[33] = {};
bool s_auth_enabled = false;

// Auth-failure rate limit (CC-F8). Sliding 60-second window of failed
// attempts; once we cross AUTH_FAIL_LIMIT in the window every check_auth
// returns false until the window slides past the oldest entry. Prevents
// online brute-force of the 32-hex-char token.
static constexpr uint8_t  AUTH_FAIL_LIMIT  = 5;
static constexpr uint32_t AUTH_FAIL_WINDOW_MS = 60 * 1000;
static uint32_t s_auth_fail_ts[AUTH_FAIL_LIMIT] = {};
static uint8_t  s_auth_fail_head = 0;
static SemaphoreHandle_t s_auth_fail_mutex = nullptr;

static void auth_record_failure() {
    if (!s_auth_fail_mutex) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (xSemaphoreTake(s_auth_fail_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_auth_fail_ts[s_auth_fail_head] = now;
    s_auth_fail_head = (s_auth_fail_head + 1) % AUTH_FAIL_LIMIT;
    xSemaphoreGive(s_auth_fail_mutex);
}

static bool auth_is_locked() {
    if (!s_auth_fail_mutex) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (xSemaphoreTake(s_auth_fail_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    uint8_t recent = 0;
    for (int i = 0; i < AUTH_FAIL_LIMIT; i++) {
        if (s_auth_fail_ts[i] != 0 &&
            (now - s_auth_fail_ts[i]) < AUTH_FAIL_WINDOW_MS) recent++;
    }
    xSemaphoreGive(s_auth_fail_mutex);
    return recent >= AUTH_FAIL_LIMIT;
}

void auth_init() {
    if (!s_auth_fail_mutex) s_auth_fail_mutex = xSemaphoreCreateMutex();

    nvs_handle_t h;
    esp_err_t oe = nvs_open("zhac_auth", NVS_READWRITE, &h);
    if (oe != ESP_OK) {
        ESP_LOGE(TAG, "auth_init: nvs_open failed: %s", esp_err_to_name(oe));
        return;
    }

    // F1 (FINDINGS.md): auth defaults OFF per operator decision (2026-05-29).
    // The on-by-default posture broke zero-config onboarding — a fresh unit's
    // SPA could not reach the token-gated WebSocket. NOTE: default-off
    // re-opens the A1 risk — until auth is enabled via /api/system
    // (Settings → "Auth (bearer token)"), any LAN/RF client has full
    // UNAUTHENTICATED control (OTA, zigbee reset, script.run). All
    // enforcement machinery (WS handshake gate, constant-time compare,
    // failure rate-limit, F19 token UI, serial token print) stays intact and
    // activates the moment auth is enabled; for a hardened image set this
    // back to 1.
    uint8_t auth_en = 0;   // default OFF (A1 reopened — see note above)
    nvs_get_u8(h, "enabled", &auth_en);
    s_auth_enabled = (auth_en != 0);

    // Always generate/load a token so it's ready if auth is enabled later
    size_t len = sizeof(s_api_token);
    esp_err_t err = nvs_get_str(h, "token", s_api_token, &len);
    if (err != ESP_OK || s_api_token[0] == '\0') {
        uint8_t rnd[16];
        esp_fill_random(rnd, sizeof(rnd));
        for (int i = 0; i < 16; i++) {
            snprintf(s_api_token + i * 2, 3, "%02x", rnd[i]);
        }
        esp_err_t se = nvs_set_str(h, "token", s_api_token);
        if (se != ESP_OK) ESP_LOGE(TAG, "auth_init: nvs_set token: %s",
                                    esp_err_to_name(se));
        esp_err_t ce = nvs_commit(h);
        if (ce != ESP_OK) ESP_LOGE(TAG, "auth_init: nvs_commit: %s",
                                    esp_err_to_name(ce));
        // F11 (FINDINGS.md): the first-boot token must NOT enter the
        // ESP_LOG ring (it is served by /api/logs). Print it straight to
        // the UART console via printf, which bypasses the log-ring vprintf
        // hook. Serial is the out-of-band bootstrap channel; a production
        // unit should surface it on a label / captive portal instead.
        printf("\n*** ZHAC API TOKEN (first boot, serial-only): %s ***\n\n",
               s_api_token);
        fflush(stdout);
    }
    nvs_close(h);

    if (s_auth_enabled) {
        ws_server_set_api_token(s_api_token);
        // F1/F11: auth is required, so the SPA needs the token. Print it to
        // the SERIAL console (NOT the /api/logs ring) on every boot so the
        // operator can always retrieve it — including on units whose token
        // was generated by an earlier firmware — and paste it into the WebUI
        // (Settings → "This browser's token"). Serial = physical access,
        // which on a non-flash-encrypted unit already implies full
        // compromise, so this discloses nothing new. A flash-encrypted
        // production image (sdkconfig.prod.defaults) should drop this print.
        printf("\n*** ZHAC API auth ENABLED — token (serial-only): %s ***\n"
               "    Paste it into the WebUI: Settings -> \"This browser's token\".\n\n",
               s_api_token);
        fflush(stdout);
        ESP_LOGI(TAG, "API auth enabled (token len=%u)", (unsigned)strlen(s_api_token));
    } else {
        ws_server_set_api_token(nullptr);
        ESP_LOGI(TAG, "API auth disabled");
    }
}

// Generate a new random token, persist, and apply to ws_server. Returns
// the new token via `out` (33 bytes incl. NUL). Used by api_token_rotate.
extern "C" bool auth_rotate_token(char* out, size_t out_cap) {
    if (out_cap < 33) return false;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    char fresh[33] = {};
    for (int i = 0; i < 16; i++) snprintf(fresh + i * 2, 3, "%02x", rnd[i]);

    nvs_handle_t h;
    esp_err_t oe = nvs_open("zhac_auth", NVS_READWRITE, &h);
    if (oe != ESP_OK) { ESP_LOGE(TAG, "rotate: nvs_open: %s", esp_err_to_name(oe)); return false; }
    esp_err_t se = nvs_set_str(h, "token", fresh);
    esp_err_t ce = (se == ESP_OK) ? nvs_commit(h) : se;
    nvs_close(h);
    if (se != ESP_OK || ce != ESP_OK) {
        ESP_LOGE(TAG, "rotate: nvs persist failed");
        return false;
    }
    memcpy(s_api_token, fresh, sizeof(fresh));
    if (s_auth_enabled) ws_server_set_api_token(s_api_token);
    memcpy(out, fresh, sizeof(fresh));
    ESP_LOGW(TAG, "API token rotated");
    return true;
}

bool check_auth(httpd_req_t* req) {
    if (!s_auth_enabled) return true;
    if (auth_is_locked()) return false;     // CC-F8 rate-limit
    char key[64] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Api-Key", key, sizeof(key)) != ESP_OK) {
        auth_record_failure();
        return false;
    }
    if (strlen(key) != 32) { auth_record_failure(); return false; }
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (uint8_t)key[i] ^ (uint8_t)s_api_token[i];
    if (diff != 0) { auth_record_failure(); return false; }
    return true;
}

// F18/A12 (FINDINGS.md): validate a bearer token presented over a non-REST
// path (the WebSocket first-message auth). Mirrors check_auth() — constant-time
// compare + the same sliding-window lockout, so WS auth is rate-limited too
// (closes A12). Returns true when auth is disabled.
bool auth_check_token(const char* token) {
    if (!s_auth_enabled) return true;
    if (auth_is_locked()) return false;
    if (!token || strlen(token) != 32) { auth_record_failure(); return false; }
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (uint8_t)token[i] ^ (uint8_t)s_api_token[i];
    if (diff != 0) { auth_record_failure(); return false; }
    return true;
}

// ── OTA signalling ────────────────────────────────────────────────────────
char              s_ota_url[256] = {};
SemaphoreHandle_t s_ota_sem      = nullptr;

// ── P4 state cache (updated from SYNC_ACK and HEARTBEAT) ─────────────────
std::atomic<uint16_t> s_p4_device_count{0};
std::atomic<uint32_t> s_p4_uptime_s{0};
std::atomic<uint32_t> s_p4_psram_free{0};
std::atomic<uint32_t> s_p4_psram_total{0};
std::atomic<uint8_t>  s_p4_cpu_pct_c0{0};
std::atomic<uint8_t>  s_p4_cpu_pct_c1{0};
std::atomic<uint32_t> s_p4_proto_mask{0};
char                  s_p4_fw_ver[16] = {};
std::atomic<uint32_t> s_p4_heap_free{0};
std::atomic<uint32_t> s_p4_heap_min_free{0};
std::atomic<uint32_t> s_p4_internal_free{0};
std::atomic<uint32_t> s_p4_internal_min_free{0};
std::atomic<uint32_t> s_p4_internal_largest_block{0};
std::atomic<uint32_t> s_p4_psram_min_free{0};
std::atomic<uint32_t> s_p4_psram_largest_block{0};
std::atomic<uint32_t> s_p4_task_stack_hwm_bytes{0};

// ── P4 OTA ────────────────────────────────────────────────────────────────
SemaphoreHandle_t      s_p4ota_sem          = nullptr;
SemaphoreHandle_t      s_p4ota_rsp_sem       = nullptr;
SemaphoreHandle_t      s_p4ota_ckpt_rsp_sem  = nullptr;
char                   s_p4ota_url[256]      = {};
HapOtaStatus           s_p4ota_status        = {};
uint32_t               s_p4ota_ckpt_offset   = 0;

// ── Alert ring buffer ─────────────────────────────────────────────────────
AlertLogEntry     s_alert_log[ALERT_LOG_MAX];
uint8_t           s_alert_log_head  = 0;
uint8_t           s_alert_log_count = 0;
SemaphoreHandle_t s_alert_log_mutex = nullptr;

// ── Cached NVS handles ────────────────────────────────────────────────────
nvs_handle_t s_nvs_zhac_opt = 0;

// ── S3 HAP + WiFi state ───────────────────────────────────────────────────
std::atomic<bool> s_synced{false};
std::atomic<bool> s_wifi_connected{false};
bool              s_metrics_enabled = false;
bool              s_ap_disabled     = false;

// ── WiFi (managed by wifi_mgr.cpp) ───────────────────────────────────────

// ── task_time_sync ────────────────────────────────────────────────────────
static void task_time_sync(void*) {
    ESP_LOGI(TAG, "task_time_sync started");
    static constexpr time_t MIN_VALID_TS = 1577836800; // 2020-01-01 UTC

    for (;;) {
        time_t now = time(nullptr);
        while (now < MIN_VALID_TS) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            now = time(nullptr);
        }

        uint8_t ts_buf[32];
        uint16_t ts_len = 0;
        if (hap_json_encode_time_sync(ts_buf, sizeof(ts_buf), &ts_len,
                                      static_cast<uint32_t>(now))) {
            hap_send(HapMsgType::TIME_SYNC, ts_buf, ts_len);
            ESP_LOGI(TAG, "TIME_SYNC sent ts=%lld", (long long)now);
        }

        vTaskDelay(pdMS_TO_TICKS(3600 * 1000));
    }
}

// ── task_ota ──────────────────────────────────────────────────────────────
static void task_ota(void*) {
    ESP_LOGI(TAG, "TaskOTA ready");
    while (true) {
        xSemaphoreTake(s_ota_sem, portMAX_DELAY);
        ESP_LOGI(TAG, "OTA: downloading from %s", s_ota_url);

        // ── Multi-step OTA so the SPA can render a progress bar ────────
        //
        // The one-shot `esp_https_ota()` blocks until done and gives no
        // per-byte progress. Switching to the multi-step API (begin →
        // perform → finish) lets us poll `esp_https_ota_get_image_len_read`
        // between `_perform` calls and push `ota.progress` WS events.
        // Devices stay paired across the post-flash reboot — same NVS
        // partition layout, same network state.
        // F3 (FINDINGS.md): OTA must use verified HTTPS. Refuse non-HTTPS
        // URLs (drops the plain-HTTP MITM vector) and validate the server
        // certificate chain + CN/SAN against the bundled CA store.
        if (strncmp(s_ota_url, "https://", 8) != 0) {
            ESP_LOGE(TAG, "OTA refused: URL is not https://");
            const char* err_ev = "{\"target\":\"s3\",\"ok\":false,\"err\":\"https required\"}";
            ws_event_broadcast("ota.complete", err_ev, strlen(err_ev));
            continue;
        }
        esp_http_client_config_t http_cfg = {};
        http_cfg.url                      = s_ota_url;
        http_cfg.crt_bundle_attach        = esp_crt_bundle_attach;  // verify cert chain
        http_cfg.skip_cert_common_name_check = false;               // verify CN/SAN too

        esp_https_ota_config_t ota_cfg = {};
        ota_cfg.http_config            = &http_cfg;

        ws_event_broadcast("ota.start",
            "{\"target\":\"s3\",\"total\":0,\"offset\":0}", 38);

        esp_https_ota_handle_t handle = nullptr;
        esp_err_t ret = esp_https_ota_begin(&ota_cfg, &handle);
        if (ret != ESP_OK || handle == nullptr) {
            ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
            char ev[128];
            int en = snprintf(ev, sizeof(ev),
                "{\"target\":\"s3\",\"ok\":false,\"err\":\"%s\"}",
                esp_err_to_name(ret));
            ws_event_broadcast("ota.complete", ev, (size_t)en);
            continue;
        }

        int total = esp_https_ota_get_image_size(handle);
        if (total < 0) total = 0;
        int last_pct = -1;
        while (true) {
            ret = esp_https_ota_perform(handle);
            if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
            const int read_now = esp_https_ota_get_image_len_read(handle);
            const int pct = (total > 0) ? (read_now * 100 / total) : 0;
            // Throttle WS pushes: emit only when whole-percent crosses.
            if (pct != last_pct) {
                char ev[160];
                int en = snprintf(ev, sizeof(ev),
                    "{\"target\":\"s3\",\"offset\":%d,\"total\":%d,\"pct\":%d}",
                    read_now, total, pct);
                ws_event_broadcast("ota.progress", ev, (size_t)en);
                last_pct = pct;
            }
        }

        const bool dl_done = esp_https_ota_is_complete_data_received(handle);
        if (ret == ESP_OK && dl_done) {
            ret = esp_https_ota_finish(handle);
        } else {
            esp_https_ota_abort(handle);
            if (ret == ESP_OK) ret = ESP_FAIL;   // download truncated
        }

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA complete — rebooting in 2 s");
            char ev[96];
            int en = snprintf(ev, sizeof(ev),
                "{\"target\":\"s3\",\"ok\":true,\"total\":%d}", total);
            ws_event_broadcast("ota.complete", ev, (size_t)en);
            // F-04 fix: flush pending rule edits before reboot. The
            // deferred writeback task has up to 5 s of in-PSRAM dirty
            // rows that would otherwise be lost across the OTA restart.
            extern void rule_store_flush_now();
            rule_store_flush_now();
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
            char ev[128];
            int en = snprintf(ev, sizeof(ev),
                "{\"target\":\"s3\",\"ok\":false,\"err\":\"%s\"}",
                esp_err_to_name(ret));
            ws_event_broadcast("ota.complete", ev, (size_t)en);
        }
    }
}

// ── task_p4_ota ───────────────────────────────────────────────────────────
static void task_p4_ota(void*) {
    ESP_LOGI(TAG, "TaskP4OTA ready");
    while (true) {
        xSemaphoreTake(s_p4ota_sem, portMAX_DELAY);
        ESP_LOGI(TAG, "P4 OTA: downloading from %s", s_p4ota_url);

        // F3 (FINDINGS.md): verified HTTPS only for the P4 image download.
        if (strncmp(s_p4ota_url, "https://", 8) != 0) {
            ESP_LOGE(TAG, "P4 OTA refused: URL is not https://");
            const char* err_ev = "{\"target\":\"p4\",\"ok\":false,\"err\":\"https required\"}";
            ws_event_broadcast("ota.complete", err_ev, strlen(err_ev));
            continue;
        }

        s_p4ota_ckpt_offset = 0;
        xSemaphoreTake(s_p4ota_ckpt_rsp_sem, 0);
        hap_send(HapMsgType::OTA_CHECKPOINT_REQ, nullptr, 0, 0);
        bool got_ckpt = xSemaphoreTake(s_p4ota_ckpt_rsp_sem, pdMS_TO_TICKS(2000)) == pdTRUE;
        uint32_t resume_offset = got_ckpt ? s_p4ota_ckpt_offset : 0;
        if (resume_offset > 0) {
            ESP_LOGI(TAG, "P4 OTA: resuming from offset %" PRIu32, resume_offset);
        }

        esp_http_client_config_t http_cfg = {};
        http_cfg.url                      = s_p4ota_url;
        // F3 (FINDINGS.md): validate the server certificate chain against
        // the bundled CA store and verify CN/SAN (no more skip).
        http_cfg.crt_bundle_attach        = esp_crt_bundle_attach;
        http_cfg.skip_cert_common_name_check = false;

        esp_http_client_handle_t client = nullptr;
        int content_len = 0;
        uint32_t total  = 0;
        bool open_ok    = false;

        // Try resume; on any disagreement from the server, fall back to a
        // clean restart from offset 0. Loops at most twice.
        for (int attempt = 0; attempt < 2; ++attempt) {
            client = esp_http_client_init(&http_cfg);
            if (!client) {
                ESP_LOGE(TAG, "P4 OTA: esp_http_client_init failed");
                break;
            }

            if (resume_offset > 0) {
                char range_hdr[32];
                snprintf(range_hdr, sizeof(range_hdr), "bytes=%" PRIu32 "-", resume_offset);
                esp_http_client_set_header(client, "Range", range_hdr);
            }

            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "P4 OTA: open failed: %s", esp_err_to_name(err));
                esp_http_client_cleanup(client);
                client = nullptr;
                break;
            }
            content_len = esp_http_client_fetch_headers(client);

            if (resume_offset == 0) {
                total = (content_len > 0) ? (uint32_t)content_len : 0;
                open_ok = true;
                break;
            }

            // Resume requested — verify server honoured the Range request.
            int status = esp_http_client_get_status_code(client);
            char* cr_val = nullptr;
            esp_http_client_get_header(client, "Content-Range", &cr_val);

            uint32_t cr_start = 0, cr_end = 0, cr_total = 0;
            bool cr_ok = (cr_val != nullptr) &&
                         (sscanf(cr_val, "bytes %" SCNu32 "-%" SCNu32 "/%" SCNu32,
                                 &cr_start, &cr_end, &cr_total) == 3);

            if (status == 206 && cr_ok && cr_start == resume_offset) {
                total = (cr_total > 0)
                        ? cr_total
                        : ((content_len > 0)
                           ? (resume_offset + (uint32_t)content_len)
                           : 0);
                open_ok = true;
                break;
            }

            ESP_LOGW(TAG,
                     "P4 OTA: server ignored Range (status=%d, Content-Range=%s) — restarting from 0",
                     status, cr_val ? cr_val : "<none>");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            resume_offset = 0;
            // Loop again to reopen with no Range header.
        }

        if (!open_ok) {
            if (client) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
            }
            continue;
        }

        EXT_RAM_BSS_ATTR static uint8_t chunk_buf[HAP_MAX_PAYLOAD];
        uint8_t* data_ptr = chunk_buf + HAP_OTA_CHUNK_HDR_SIZE;
        const size_t data_max = sizeof(chunk_buf) - HAP_OTA_CHUNK_HDR_SIZE;

        uint32_t offset = resume_offset;
        bool download_ok = true;

        // ── Backpressure + progress ─────────────────────────────────────
        //
        // Per-chunk 5 ms delay keeps P4's HAP RX queue drained (P4 needs
        // ~1-2 ms per esp_ota_write). Every kChunkBatch chunks we layer a
        // round-trip OTA_CHECKPOINT_REQ as a reliability backstop: P4's
        // response tells us its `s_ota_expected_offset` — if that diverges
        // from S3's expectation, the transfer aborts cleanly with a real
        // error rather than hanging until the WiFi timeout. Each verified
        // batch also pushes an `ota.progress` WS event so the SPA can
        // render a progress bar driven by ground truth from P4, not by
        // S3-side queue depth.
        constexpr uint32_t kChunkBatch          = 32;     // ~128 KB / batch
        // P4's hap_slave dispatch is single-threaded; under concurrent
        // Zigbee traffic + esp_ota_write (flash, ~1-2 ms each), the
        // checkpoint REQ can queue behind ~32 chunks + reports for
        // several seconds. 15 s is the safety budget. If even that
        // expires we fall through to a SOFT warning rather than abort —
        // the 5 ms throttle already prevents chunks from being dropped,
        // so a missed checkpoint just means we lose the progress probe
        // for that batch, not the transfer itself.
        constexpr uint32_t kCheckpointTimeoutMs = 15000;
        uint32_t chunks_in_batch = 0;
        bool     batch_failed    = false;

        // Start event so the SPA can switch from "idle" to "in progress".
        {
            char ev[96];
            int en = snprintf(ev, sizeof(ev),
                "{\"target\":\"p4\",\"total\":%" PRIu32 ",\"offset\":%" PRIu32 "}",
                total, offset);
            ws_event_broadcast("ota.start", ev, (size_t)en);
        }

        while (true) {
            int n = esp_http_client_read(client, reinterpret_cast<char*>(data_ptr), data_max);
            if (n < 0) {
                ESP_LOGE(TAG, "P4 OTA: read error");
                download_ok = false;
                break;
            }
            if (n == 0) break;

            auto* hdr    = reinterpret_cast<HapOtaChunkHdr*>(chunk_buf);
            hdr->total   = total;
            hdr->offset  = offset;
            hdr->flags   = esp_http_client_is_complete_data_received(client) ? 0x01 : 0x00;
            memset(hdr->_pad, 0, sizeof(hdr->_pad));

            offset += (uint32_t)n;
            const bool is_last = (hdr->flags & 0x01) != 0;

            hap_send(HapMsgType::OTA_CHUNK, chunk_buf,
                     (uint16_t)(HAP_OTA_CHUNK_HDR_SIZE + n), 0);

            // Per-chunk throttle (see comment block above for the
            // chunks-dropping pathology this prevents).
            vTaskDelay(pdMS_TO_TICKS(5));

            chunks_in_batch++;

            if (chunks_in_batch >= kChunkBatch || is_last) {
                // Drain any stale sem from a previous batch whose RSP
                // arrived after we'd given up waiting. Counts as "got"
                // only if the take actually pulls a fresh post-REQ
                // signal in this iteration.
                xSemaphoreTake(s_p4ota_ckpt_rsp_sem, 0);
                hap_send(HapMsgType::OTA_CHECKPOINT_REQ, nullptr, 0, 0);
                bool got = xSemaphoreTake(s_p4ota_ckpt_rsp_sem,
                                          pdMS_TO_TICKS(kCheckpointTimeoutMs)) == pdTRUE;

                bool verified = false;
                if (got) {
                    if (s_p4ota_ckpt_offset == offset) {
                        verified = true;
                    } else if (s_p4ota_ckpt_offset > offset) {
                        // P4 reported MORE bytes than S3 has sent — impossible
                        // without state corruption. Hard abort.
                        ESP_LOGE(TAG,
                                 "P4 OTA: checkpoint impossible — S3=%" PRIu32
                                 " P4=%" PRIu32 " (P4 ahead) — aborting",
                                 offset, s_p4ota_ckpt_offset);
                        batch_failed = true;
                        break;
                    } else {
                        // P4 lags by more than one batch — chunks lost
                        // somewhere. Should not happen with the 5ms
                        // throttle, but if it does abort cleanly.
                        const uint32_t lag = offset - s_p4ota_ckpt_offset;
                        if (lag > 2 * kChunkBatch * 4084) {
                            ESP_LOGE(TAG,
                                     "P4 OTA: checkpoint divergence — S3=%" PRIu32
                                     " P4=%" PRIu32 " (lag=%" PRIu32 " B) — aborting",
                                     offset, s_p4ota_ckpt_offset, lag);
                            batch_failed = true;
                            break;
                        }
                        ESP_LOGW(TAG,
                                 "P4 OTA: P4 lagging — S3=%" PRIu32
                                 " P4=%" PRIu32 " (lag=%" PRIu32 " B) — continuing",
                                 offset, s_p4ota_ckpt_offset, lag);
                        verified = true;   // tolerate small lag
                    }
                } else {
                    // Soft timeout: P4 too busy to respond within the
                    // window. Don't abort — the 5 ms throttle already
                    // protects against chunk drops; this checkpoint is
                    // a "nice to have" progress probe, not a safety net.
                    // Push progress based on S3's offset so the SPA bar
                    // still advances; flag the timeout in the log.
                    ESP_LOGW(TAG,
                             "P4 OTA: checkpoint timeout at offset=%" PRIu32
                             " — continuing (P4 busy?)", offset);
                    verified = true;   // pretend ok to keep going
                }

                if (verified) {
                    chunks_in_batch = 0;
                    char ev[128];
                    const uint32_t pct = total ? (offset * 100u / total) : 0;
                    int en = snprintf(ev, sizeof(ev),
                        "{\"target\":\"p4\",\"offset\":%" PRIu32
                        ",\"total\":%" PRIu32 ",\"pct\":%" PRIu32 "}",
                        offset, total, pct);
                    ws_event_broadcast("ota.progress", ev, (size_t)en);
                }
            }

            if (is_last) break;
        }

        if (batch_failed) download_ok = false;

        if (!download_ok) {
            ESP_LOGI(TAG, "P4 OTA: interrupted at offset %" PRIu32 " — resumable", offset);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (!download_ok) {
            // download_ok=false covers HTTP read errors AND checkpoint
            // failures from the batch loop above. Tell the SPA so the
            // progress bar flips to an error state instead of dangling.
            char ev[160];
            int en = snprintf(ev, sizeof(ev),
                "{\"target\":\"p4\",\"ok\":false,\"offset\":%" PRIu32
                ",\"total\":%" PRIu32 ",\"err\":\"%s\"}",
                offset, total,
                batch_failed ? "checkpoint failure" : "download interrupted");
            ws_event_broadcast("ota.complete", ev, (size_t)en);
            continue;
        }

        bool acked = xSemaphoreTake(s_p4ota_rsp_sem, pdMS_TO_TICKS(P4_OTA_RESPONSE_TIMEOUT_MS)) == pdTRUE;
        const char* err_msg = nullptr;
        bool ok = false;
        if (!acked) {
            xSemaphoreTake(s_p4ota_rsp_sem, 0);
            ESP_LOGE(TAG, "P4 OTA: timeout — no OTA_STATUS from P4");
            err_msg = "no OTA_STATUS";
        } else if (!s_p4ota_status.ok) {
            ESP_LOGE(TAG, "P4 OTA failed: %s", s_p4ota_status.err);
            err_msg = s_p4ota_status.err;
        } else {
            ESP_LOGI(TAG, "P4 OTA success — P4 will reboot");
            ok = true;
        }
        char ev[160];
        int en = snprintf(ev, sizeof(ev),
            "{\"target\":\"p4\",\"ok\":%s,\"offset\":%" PRIu32
            ",\"total\":%" PRIu32 ",\"err\":\"%s\"}",
            ok ? "true" : "false", offset, total,
            err_msg ? err_msg : "");
        ws_event_broadcast("ota.complete", ev, (size_t)en);
    }
}

// ── task_stack_mon ────────────────────────────────────────────────────────
static void task_stack_mon(void*) {
    vTaskDelay(pdMS_TO_TICKS(STACK_MON_INITIAL_DELAY_MS));
    while (true) {
        ESP_LOGI(TAG, "=== Stack HWM (S3) ===");
        for (const auto* e = zhac::stack::kTable; e->name != nullptr; ++e) {
            TaskHandle_t h = xTaskGetHandle(e->name);
            if (!h) continue;
            uint32_t free_bytes = uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);
            if (free_bytes < 512) {
                ESP_LOGW(TAG, "  %-16s  LOW STACK: free=%" PRIu32 " bytes", e->name, free_bytes);
            }
            uint32_t used = (free_bytes > e->size) ? 0 : (e->size - free_bytes);
            ESP_LOGI(TAG, "  %-16s  total=%4" PRIu32 "  free=%4" PRIu32 "  used=%4" PRIu32,
                     e->name, e->size, free_bytes, used);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

// Provided by metrics_mqtt.cpp — C linkage so main can forward-declare
// without pulling in a tiny header.
extern "C" void metrics_mqtt_publisher_start();

// ── app_main ──────────────────────────────────────────────────────────────
extern "C" void app_main() {
    esp_reset_reason_t rr = esp_reset_reason();
    static const char* const s_reset_names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
        "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO",
    };
    const char* rr_str = (rr < (esp_reset_reason_t)(sizeof(s_reset_names)/sizeof(s_reset_names[0])))
                         ? s_reset_names[rr] : "?";
    ESP_LOGI(TAG, "ZHAC S3 core starting (reset reason: %s)", rr_str);

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms      = 10000,
        .idle_core_mask  = 0,
        .trigger_panic   = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_reconfigure(&wdt_cfg));

    // TODO: Enable NVS encryption (nvs_flash_secure_init) — see docs/TODO.md
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }

    // Capture log lines into an in-memory ring for /api/logs. Installed
    // early so subsequent ESP_LOG* calls are captured from boot onward.
    log_ring_init();

    // Metrics engine — zero-init shard storage. Safe to call at any
    // time before the first counter/value/timer emission.
    metrics::init();

    // Start the periodic MQTT snapshot publisher (no-op when exporter
    // disabled in sdkconfig).
    metrics_mqtt_publisher_start();

    // SPI master must init before WiFi — WiFi driver corrupts the interrupt
    // descriptor linked list (vector_desc_head), causing LoadProhibited in
    // esp_intr_alloc when SPI2 tries to allocate its interrupt afterwards.
    hap_master_init();

    // WiFi manager handles esp_netif_init() + esp_event_loop_create_default()
    wifi_mgr_init();

    // Open hot-path NVS namespaces once and keep handles cached (S4)
    nvs_open("zhac_opt", NVS_READWRITE, &s_nvs_zhac_opt);  // best-effort; 0 means uncached

    s_ota_sem = xSemaphoreCreateBinary();
    configASSERT(s_ota_sem);

    // Per-type request mutexes, response semaphores and shared response
    // buffers were removed when hap_roundtrip_v2 took over (F-01 v2):
    // every caller now owns its own response buffer + waiter slot, so
    // there's no shared state left to gate.
    s_p4ota_sem          = xSemaphoreCreateBinary();
    s_p4ota_rsp_sem      = xSemaphoreCreateBinary();
    s_p4ota_ckpt_rsp_sem = xSemaphoreCreateBinary();
    s_alert_log_mutex    = xSemaphoreCreateMutex();
    configASSERT(s_alert_log_mutex);
    configASSERT(s_p4ota_sem);
    configASSERT(s_p4ota_rsp_sem);
    configASSERT(s_p4ota_ckpt_rsp_sem);

    // Mount SPIFFS for web UI static files
    {
        esp_vfs_spiffs_conf_t spiffs_conf = {
            .base_path              = "/spiffs",
            .partition_label        = nullptr,
            .max_files              = 8,
            .format_if_mount_failed = false,
        };
        esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS mount failed (%s) — web UI unavailable", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
            alert_log_load();
        }
    }

    // Load persisted timezone and metrics toggle from NVS
    {
        nvs_handle_t h;
        char tz[64] = {};
        size_t tz_len = sizeof(tz);
        uint8_t metrics_en = 0;
        uint8_t ap_disabled = 0;
        if (nvs_open("sys_cfg", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_str(h, "timezone", tz, &tz_len);
            nvs_get_u8(h, "metrics_en", &metrics_en);
            nvs_get_u8(h, "ap_disabled", &ap_disabled);
            nvs_close(h);
        }
        if (tz[0]) { setenv("TZ", tz, 1); tzset(); ESP_LOGI(TAG, "Timezone: %s", tz); }
        s_metrics_enabled = (metrics_en != 0);
        s_ap_disabled     = (ap_disabled != 0);
        ESP_LOGI(TAG, "Prometheus metrics: %s", s_metrics_enabled ? "enabled" : "disabled");
    }

    // hap_master_init() moved before wifi_mgr_init() for test 2
    ws_server_init();

    // ── Log-streaming sink toggles — load regardless of WiFi mode ──
    {
        nvs_handle_t h;
        if (nvs_open("log_cfg", NVS_READONLY, &h) == ESP_OK) {
            uint8_t en = 0;
            uint8_t lvl = (uint8_t)'I';
            nvs_get_u8(h, "mqtt_en",  &en);
            nvs_get_u8(h, "mqtt_lvl", &lvl);
            log_sinks_set_mqtt(en != 0, (char)lvl);
            en = 0; lvl = (uint8_t)'I';
            nvs_get_u8(h, "ws_en",    &en);
            nvs_get_u8(h, "ws_lvl",   &lvl);
            log_sinks_set_ws(en != 0, (char)lvl);
            nvs_close(h);
        }
    }

    // MQTT config must be staged at boot even when we're still in
    // AP+STA concurrent mode — `wifi_mgr_is_ap_mode()` returns true for
    // the whole APSTA lifetime, not just pure-AP fallback. Gating this
    // block on !is_ap_mode() would skip staging forever on boards that
    // keep the captive portal up in parallel with STA. The actual
    // client start is deferred to the WiFi STA-got-IP handler.
    //
    // mqtt_gw_init creates the publish worker queue + task. Without it
    // every mqtt_gw_publish silently early-outs on null s_pubq even
    // after the broker connects, leaving the WebUI showing "connected"
    // while not a single message reaches the broker.
    mqtt_gw_init();
    tg_gw_init();
    {
        // MQTT disabled by default — opt-in via /api/settings {mqtt_enabled:true}
        uint8_t mqtt_en = 0;
        char url[128] = {};
        char root[32] = {};
        char cid[32]  = {};
        nvs_read_mqtt_cfg(&mqtt_en,
                          url,  sizeof(url),
                          root, sizeof(root),
                          cid,  sizeof(cid));
        if (mqtt_en && url[0]) {
            mqtt_gw_configure(url, root, cid);
            ESP_LOGI(TAG, "MQTT configured — will start on STA_GOT_IP; broker=%s root=%s cid=%s",
                     url, mqtt_gw_get_root_topic(),
                     cid[0] ? cid : "(auto)");
        } else {
            if (root[0]) mqtt_gw_set_root_topic(root);
            if (cid[0])  mqtt_gw_set_client_id(cid);
            ESP_LOGI(TAG, "MQTT disabled (enabled=%u, url=%s)", mqtt_en, url[0] ? "set" : "empty");
        }
    }

    // TDD Section 8.2 — WiFi is already running (wifi_mgr_init above)
    xTaskCreatePinnedToCore(task_hap,   "TaskHAP",   zhac::stack::kHapS3, nullptr, 5, nullptr, 1);
    xTaskCreate(             task_http, "TaskHTTP",  zhac::stack::kHttp, nullptr, 3, nullptr);
    alert_persist_task_init();
    ws_server_set_rx_callback(on_ws_rx);
    {
        // Wire the MQTT rx callback unconditionally. The subscribe
        // below is a no-op until the client is actually running, which
        // happens on STA_GOT_IP; mqtt_gw_subscribe stores the filter
        // and applies it on the next CONNECTED event.
        mqtt_gw_set_rx_callback(on_mqtt_rx);
        // Subscribe under the configured root so two controllers on
        // one broker don't receive each other's device commands.
        char sub[40];
        snprintf(sub, sizeof(sub), "%s/#", mqtt_gw_get_root_topic());
        mqtt_gw_subscribe(sub, 0);
    }
    xTaskCreate(             task_time_sync, "TaskTimeSync", zhac::stack::kTimeSync, nullptr, 2, nullptr);
    xTaskCreate(             task_ota,    "TaskOTA",   zhac::stack::kOta, nullptr, 2, nullptr);
    xTaskCreate(             task_p4_ota, "TaskP4OTA", zhac::stack::kP4Ota, nullptr, 2, nullptr);
    xTaskCreate(             task_stack_mon,"TaskStackMon", zhac::stack::kStackMonS3, nullptr, 1, nullptr);

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    // Bridge wifi events to remote_client's event group so the task
    // can transition IDLE_NO_WIFI ↔ CONNECTING / BACKOFF as the
    // network state changes. Bit positions match the EVB_WIFI_UP
    // (1<<2) and EVB_WIFI_DOWN (1<<3) constants in remote_client.cpp.
    extern EventGroupHandle_t s_remote_evt;
    esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        [](void*, esp_event_base_t, int32_t, void*) {
            if (s_remote_evt) xEventGroupSetBits(s_remote_evt, 1 << 2);
        },
        nullptr);
    esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        [](void*, esp_event_base_t, int32_t, void*) {
            if (s_remote_evt) xEventGroupSetBits(s_remote_evt, 1 << 3);
        },
        nullptr);
#endif

    remote_client_init();

    vTaskDelete(nullptr);
}
