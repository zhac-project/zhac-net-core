// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared command handlers. See api_handlers.h for the contract.
// First landed as the Phase-1 shape-proof of the SPA-WS migration —
// plan doc `docs/plans/2026-04-22-unified-api.md`.
//
// Handlers here are PURE business logic. Auth, rate-limiting, URI parsing,
// and transport framing live in rest_*.cpp (HTTP) and will live in
// ws_bridge.cpp (WebSocket) in Phase 2.
//
// Exceptions that stay as REST-only (not migrated):
//   - handle_get_metrics         (Prometheus text, not JSON)
//   - handle_get_static          (SPIFFS streaming)
//   - handle_post_scripts_bulk   (body up to HAP_MAX_PAYLOAD; shared-handler
//                                  buffers would pin too much stack)

#include "api_handlers.h"
#include "s3_internal.h"
#include "nvs_helpers.h"
#include "log_ring.h"
#include "groups_store.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "wifi_mgr.h"
#include "task_stacks.h"
#include "ArduinoJson.h"
#include "esp_timer.h"
#include "esp_app_desc.h"   // esp_app_get_description()->version (S3 FW version)
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char* TAG_API = "api_system";


// ── OTA URL validation ────────────────────────────────────────────────────
// Reject URLs containing control characters (\r, \n, bytes < 0x20) to prevent
// HTTP request smuggling when the URL is passed to esp_http_client.
static bool url_has_control_chars(const char* url) {
    for (const char* p = url; *p; p++) {
        if ((unsigned char)*p < 0x20) return true;
    }
    return false;
}

// Per-core CPU sampling lives in zap_common/sys_metrics.h — shared
// with the P4 heartbeat.
#include "sys_metrics.h"

// ── Status / system ──────────────────────────────────────────────────────
extern "C" ApiStatus api_status_get(const char* /*body*/, size_t /*body_len*/,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    char ip_str[16]  = "0.0.0.0";
    char mac_str[18] = "00:00:00:00:00:00";

    uint8_t mqtt_en_u8        = 0;
    char mqtt_broker_nvs[128] = {};
    char mqtt_root_nvs[32]    = {};
    char mqtt_cid_nvs[32]     = {};
    nvs_read_mqtt_cfg(&mqtt_en_u8,
                      mqtt_broker_nvs, sizeof(mqtt_broker_nvs),
                      mqtt_root_nvs,   sizeof(mqtt_root_nvs),
                      mqtt_cid_nvs,    sizeof(mqtt_cid_nvs));
    bool mqtt_enabled_nvs = (mqtt_en_u8 != 0);
    if (!mqtt_root_nvs[0]) strncpy(mqtt_root_nvs, mqtt_gw_get_root_topic(),
                                    sizeof(mqtt_root_nvs) - 1);
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info{};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint32_t psram_free  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_free        = (uint32_t)esp_get_free_heap_size();
    uint32_t heap_min_free    = (uint32_t)esp_get_minimum_free_heap_size();
    uint32_t int_free         = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t int_min_free     = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t int_largest_blk  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t psram_min_free   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_largest_blk= (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    uint32_t s3_stack_hwm = 0;
    {
        uint32_t min_hwm = UINT32_MAX;
        for (const auto* e = zhac::stack::kTable; e->name != nullptr; ++e) {
            TaskHandle_t h = xTaskGetHandle(e->name);
            if (!h) continue;
            uint32_t free_b = (uint32_t)uxTaskGetStackHighWaterMark(h) *
                              (uint32_t)sizeof(StackType_t);
            if (free_b < min_hwm) min_hwm = free_b;
        }
        s3_stack_hwm = (min_hwm == UINT32_MAX) ? 0 : min_hwm;
    }

    // Caller-owned CPU%-baseline window for the /api/status cadence —
    // own copy so it never crosses the P4 heartbeat's window (FINDINGS
    // §8: the sampler dropped its shared per-TU static). The default
    // single httpd worker serialises status requests; should the server
    // ever fan status across worker threads, the worst case is a
    // transient bogus reading for one poll, not state corruption.
    static sys_metrics_cpu_ctx_t s_status_cpu_ctx{};
    uint8_t  s3_cpu_c0 = 0, s3_cpu_c1 = 0;
    sys_metrics_sample_cpu_pct(s_status_cpu_ctx, s3_cpu_c0, s3_cpu_c1);

    char p4_fw_ver[32] = {};
    hap_bridge_copy_p4_fw_ver(p4_fw_ver, sizeof(p4_fw_ver));
    const char* s3_fw_ver = esp_app_get_description()->version;  // git-derived

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
    const bool remote_available = true;
#else
    const bool remote_available = false;
#endif

    int n = snprintf(rsp_buf, rsp_cap,
        "{\"heap\":%" PRIu32
         ",\"heap_min\":%" PRIu32
         ",\"int_free\":%" PRIu32
         ",\"int_min\":%" PRIu32
         ",\"int_blk\":%" PRIu32
         ",\"psram_free\":%" PRIu32
         ",\"psram_min\":%" PRIu32
         ",\"psram_blk\":%" PRIu32
         ",\"psram_total\":%" PRIu32
         ",\"stack_hwm\":%" PRIu32
         ",\"uptime\":%" PRIu64
         ",\"cpu_c0\":%u"
         ",\"cpu_c1\":%u"
         ",\"ip\":\"%s\""
         ",\"mac\":\"%s\""
         ",\"wifi\":%s"
         ",\"mqtt_connected\":%s"
         ",\"mqtt_active\":%s"
         ",\"mqtt_enabled\":%s"
         ",\"remote_available\":%s"
         ",\"mqtt_broker\":\"%s\""
         ",\"mqtt_root_topic\":\"%s\""
         ",\"mqtt_client_id\":\"%s\""
         ",\"log_mqtt_enabled\":%s"
         ",\"log_mqtt_level\":\"%c\""
         ",\"log_ws_enabled\":%s"
         ",\"log_ws_level\":\"%c\""
         ",\"ap_disabled\":%s"
         ",\"ws_clients\":%d"
         ",\"ws_tx_drops\":%" PRIu32
         ",\"synced\":%s"
         ",\"metrics_enabled\":%s"
         ",\"auth_enabled\":%s"
         ",\"fw_version\":\"%s\""
         ",\"p4_unresponsive\":%s"
         ",\"p4\":{"
           "\"devices\":%" PRIu16
           ",\"uptime\":%" PRIu32
           ",\"fw\":\"%s\""
           ",\"heap\":%" PRIu32
           ",\"heap_min\":%" PRIu32
           ",\"int_free\":%" PRIu32
           ",\"int_min\":%" PRIu32
           ",\"int_blk\":%" PRIu32
           ",\"psram_free\":%" PRIu32
           ",\"psram_min\":%" PRIu32
           ",\"psram_blk\":%" PRIu32
           ",\"psram_total\":%" PRIu32
           ",\"stack_hwm\":%" PRIu32
           ",\"cpu_c0\":%u"
           ",\"cpu_c1\":%u"
           ",\"proto_mask\":%lu"
           ",\"synced\":%s"
         "}}",
        heap_free,
        heap_min_free,
        int_free,
        int_min_free,
        int_largest_blk,
        psram_free,
        psram_min_free,
        psram_largest_blk,
        psram_total,
        s3_stack_hwm,
        esp_timer_get_time() / 1000000ULL,
        (unsigned)s3_cpu_c0,
        (unsigned)s3_cpu_c1,
        ip_str,
        mac_str,
        s_wifi_connected.load(std::memory_order_acquire) ? "true" : "false",
        mqtt_gw_is_connected()                           ? "true" : "false",
        mqtt_gw_is_active()                              ? "true" : "false",
        mqtt_enabled_nvs                                 ? "true" : "false",
        remote_available                                 ? "true" : "false",
        mqtt_broker_nvs,
        mqtt_root_nvs,
        mqtt_cid_nvs,
        log_sinks_get_mqtt_enabled()                     ? "true" : "false",
        log_sinks_get_mqtt_level(),
        log_sinks_get_ws_enabled()                       ? "true" : "false",
        log_sinks_get_ws_level(),
        s_ap_disabled                                    ? "true" : "false",
        ws_server_client_count(),
        ws_bridge_tx_drops(),
        s_synced.load(std::memory_order_acquire)         ? "true" : "false",
        s_metrics_enabled                                ? "true" : "false",
        s_auth_enabled                                   ? "true" : "false",
        s3_fw_ver,
        // F-01 fix: api_token field removed — /api/status is
        // unauthenticated, so echoing the bootstrap token here let any
        // LAN client read it. Token now surfaces only via serial log
        // on first boot (see auth_init in main.cpp) or via the
        // authenticated /api/system/token (api_token_rotate).
        hap_bridge_is_p4_unresponsive()                  ? "true" : "false",
        s_p4_device_count.load(std::memory_order_relaxed),
        s_p4_uptime_s.load(std::memory_order_relaxed),
        p4_fw_ver,
        s_p4_heap_free.load(std::memory_order_relaxed),
        s_p4_heap_min_free.load(std::memory_order_relaxed),
        s_p4_internal_free.load(std::memory_order_relaxed),
        s_p4_internal_min_free.load(std::memory_order_relaxed),
        s_p4_internal_largest_block.load(std::memory_order_relaxed),
        s_p4_psram_free.load(std::memory_order_relaxed),
        s_p4_psram_min_free.load(std::memory_order_relaxed),
        s_p4_psram_largest_block.load(std::memory_order_relaxed),
        s_p4_psram_total.load(std::memory_order_relaxed),
        s_p4_task_stack_hwm_bytes.load(std::memory_order_relaxed),
        (unsigned)s_p4_cpu_pct_c0.load(std::memory_order_relaxed),
        (unsigned)s_p4_cpu_pct_c1.load(std::memory_order_relaxed),
        (unsigned long)s_p4_proto_mask.load(std::memory_order_relaxed),
        s_synced.load(std::memory_order_acquire)         ? "true" : "false");
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_alerts_get(const char* /*body*/, size_t /*body_len*/,
                                     char* rsp_buf, size_t rsp_cap,
                                     size_t* rsp_len) {
    xSemaphoreTake(s_alert_log_mutex, portMAX_DELAY);
    uint8_t count = s_alert_log_count;
    uint8_t start = (s_alert_log_count < ALERT_LOG_MAX)
                    ? 0
                    : s_alert_log_head;

    int pos = 0;
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "[");
    for (uint8_t i = 0; i < count; i++) {
        const AlertLogEntry& e = s_alert_log[(start + i) % ALERT_LOG_MAX];
        if (i > 0) pos += snprintf(rsp_buf + pos, rsp_cap - pos, ",");
        // alert.msg originates from P4 and may include device-supplied
        // strings — escape `"`, `\`, control bytes per RFC 8259.
        char msg_esc[sizeof(e.alert.msg) * 2 + 1];
        hap_json_escape_str(e.alert.msg, msg_esc, sizeof(msg_esc));
        pos += snprintf(rsp_buf + pos, rsp_cap - pos,
            "{\"code\":%u,\"ieee\":\"0x%016llX\",\"msg\":\"%s\",\"ts\":%" PRIu32 "}",
            static_cast<uint8_t>(e.alert.code),
            (unsigned long long)e.alert.ieee,
            msg_esc,
            e.wall_ts);
        if (pos >= (int)rsp_cap - 64) break;
    }
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "]");
    xSemaphoreGive(s_alert_log_mutex);

    if (rsp_len) *rsp_len = (size_t)pos;
    return API_OK;
}

extern "C" ApiStatus api_logs_get(const char* /*body*/, size_t /*body_len*/,
                                   char* rsp_buf, size_t rsp_cap,
                                   size_t* rsp_len) {
    size_t n = log_ring_to_json(rsp_buf, rsp_cap);
    if (n == 0) {
        n = (size_t)snprintf(rsp_buf, rsp_cap, "{\"logs\":[]}");
    }
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_diagnostics_unhandled_get(const char* /*body*/,
                                                     size_t /*body_len*/,
                                                     char* rsp_buf,
                                                     size_t rsp_cap,
                                                     size_t* rsp_len) {
    uint8_t req_buf[4] = {};
    uint16_t req_len = 0;
    constexpr size_t kDiagRspCap = HAP_MAX_PAYLOAD;
    auto rsp = std::unique_ptr<char[]>(new (std::nothrow) char[kDiagRspCap]);
    if (!rsp) {
        size_t n = (size_t)snprintf(rsp_buf, rsp_cap, "{\"entries\":[]}");
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    size_t got = 0;
    bool ok = hap_roundtrip_v2(HapMsgType::DIAG_UNHANDLED_REQ, req_buf, req_len,
                                rsp.get(), kDiagRspCap, &got, 2000);
    if (ok) {
        size_t n = (got >= rsp_cap) ? rsp_cap - 1 : got;
        memcpy(rsp_buf, rsp.get(), n);
        rsp_buf[n] = '\0';
        if (rsp_len) *rsp_len = n;
        return API_OK;
    }
    size_t n = (size_t)snprintf(rsp_buf, rsp_cap, "{\"entries\":[]}");
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// ── WiFi ─────────────────────────────────────────────────────────────────
extern "C" ApiStatus api_wifi_status(const char* /*body*/, size_t /*body_len*/,
                                      char* rsp_buf, size_t rsp_cap,
                                      size_t* rsp_len) {
    char ip_buf[16];
    wifi_mgr_get_ip_str(ip_buf, sizeof(ip_buf));

    // SSIDs are operator-controlled but still arbitrary bytes — escape
    // `"`, `\`, control bytes so a quote in the AP/STA name can't break
    // the JSON the SPA parses. Worst case: 32-byte SSID with every byte
    // a `"` or `\` → 64 chars + NUL = 65, hence the *2+1 sizing.
    if (wifi_mgr_is_ap_mode()) {
        const char* ap_ssid = wifi_mgr_get_ap_ssid();
        char ssid_esc[33 * 2 + 1];
        hap_json_escape_str(ap_ssid, ssid_esc, sizeof(ssid_esc));
        int n = snprintf(rsp_buf, rsp_cap,
            "{\"mode\":\"ap\",\"ssid\":\"%s\",\"ip\":\"%s\",\"sta_ssid\":null,\"rssi\":null}",
            ssid_esc, ip_buf);
        if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
        if (rsp_len) *rsp_len = (size_t)n;
        return API_OK;
    }

    wifi_ap_record_t ap{};
    const bool have_live = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    char ssid_out[33] = {0};
    if (have_live && ap.ssid[0] != '\0') {
        memcpy(ssid_out, ap.ssid, sizeof(ap.ssid));
    } else {
        wifi_config_t conf{};
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
            memcpy(ssid_out, conf.sta.ssid, sizeof(conf.sta.ssid));
        }
    }

    char ssid_esc[sizeof(ssid_out) * 2 + 1];
    hap_json_escape_str(ssid_out, ssid_esc, sizeof(ssid_esc));

    int n;
    if (have_live) {
        n = snprintf(rsp_buf, rsp_cap,
            "{\"mode\":\"sta\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
            ssid_esc, ip_buf, ap.rssi);
    } else {
        n = snprintf(rsp_buf, rsp_cap,
            "{\"mode\":\"sta\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":null}",
            ssid_esc, ip_buf);
    }
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_wifi_scan(const char* /*body*/, size_t /*body_len*/,
                                    char* rsp_buf, size_t rsp_cap,
                                    size_t* rsp_len) {
    wifi_mgr_scan();
    // T16 (FINDINGS §5.2): copy a stable snapshot of the scan results — the
    // underlying array is shared with the remote-client task (wifi.scan is
    // remote-allow-listed) and could be overwritten by a concurrent scan if
    // we iterated the live static. Buffer matches the internal capacity.
    static constexpr uint16_t kMaxAps = WIFI_MGR_MAX_SCAN;
    wifi_ap_record_t results[kMaxAps];
    uint16_t count = wifi_mgr_get_scan_results(results, kMaxAps);

    int pos = 0;
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "{\"networks\":[");
    for (uint16_t i = 0; i < count && pos < (int)rsp_cap - 128; i++) {
        if (i > 0) rsp_buf[pos++] = ',';
        const char* auth_str = "unknown";
        switch (results[i].authmode) {
            case WIFI_AUTH_OPEN:            auth_str = "open";       break;
            case WIFI_AUTH_WEP:             auth_str = "wep";        break;
            case WIFI_AUTH_WPA_PSK:         auth_str = "wpa";        break;
            case WIFI_AUTH_WPA2_PSK:        auth_str = "wpa2";       break;
            case WIFI_AUTH_WPA3_PSK:        auth_str = "wpa3";       break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "enterprise"; break;
            default:                        auth_str = "other";      break;
        }
        // Copy the AP SSID into a NUL-terminated buffer first; the
        // 33-byte ssid field in wifi_ap_record_t is not guaranteed to
        // carry a terminator if the SSID is exactly 32 bytes long.
        // Then escape for JSON — broadcast SSIDs are arbitrary bytes
        // controlled by other LAN-visible APs.
        char ssid_raw[33] = {0};
        memcpy(ssid_raw, results[i].ssid, sizeof(results[i].ssid));
        ssid_raw[sizeof(ssid_raw) - 1] = '\0';
        char ssid_esc[sizeof(ssid_raw) * 2 + 1];
        hap_json_escape_str(ssid_raw, ssid_esc, sizeof(ssid_esc));
        pos += snprintf(rsp_buf + pos, rsp_cap - pos,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
            ssid_esc, results[i].rssi, auth_str);
    }
    pos += snprintf(rsp_buf + pos, rsp_cap - pos, "]}");
    if (rsp_len) *rsp_len = (size_t)pos;
    return API_OK;
}

extern "C" ApiStatus api_wifi_connect(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* ssid_str = doc["ssid"] | (const char*)nullptr;
    if (!ssid_str || ssid_str[0] == '\0') return API_BAD_REQUEST;

    char ssid[33] = {};
    char pass[65] = {};
    strncpy(ssid, ssid_str,           sizeof(ssid) - 1);
    strncpy(pass, doc["pass"] | "", sizeof(pass) - 1);

    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_API, "nvs_open(wifi_cfg) failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return API_INTERNAL_ERROR;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);

    // F11 (FINDINGS.md): do not log the SSID value — it lands in the
    // /api/logs ring. Log only that credentials changed.
    ESP_LOGI(TAG_API, "WiFi credentials updated (ssid len=%u) — rebooting in 1 s",
             (unsigned)strlen(ssid));

    // Actually schedule the reboot. The message above used to be a
    // lie — nothing was kicking esp_restart, so the device kept
    // running with the new NVS but the old WiFi connection. Fire a
    // one-shot timer so the current HTTP/WS request can send its
    // response before the reset.
    static esp_timer_handle_t s_reboot_timer = nullptr;
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = {
            // P4-T29: flush any debounced device-options NVS write before the
            // restart so a staged-but-uncommitted option survives the reboot.
            .callback = [](void*) {
                api_device_opt_flush_now();   // flush debounced device-options NVS
                esp_restart();
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "reboot_deferred",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 1000000);   // 1 s in µs

    int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":true,\"reboot\":true}");
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_wifi_disconnect(const char* /*body*/, size_t /*body_len*/,
                                          char* rsp_buf, size_t rsp_cap,
                                          size_t* rsp_len) {
    // Defer the actual erase + reboot to the timer task so the HTTP/WS
    // response can flush before the chip restarts. wifi_mgr_forget_and_reboot
    // does the NVS wipe + 500 ms delay + esp_restart itself.
    static esp_timer_handle_t s_forget_timer = nullptr;
    if (!s_forget_timer) {
        const esp_timer_create_args_t args = {
            // no api_device_opt_flush_now() here: forget wipes NVS + reboots, so
            // a staged device-opt is intentionally discarded with the rest of
            // config (different state class).
            .callback = [](void*) { wifi_mgr_forget_and_reboot(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_forget",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&args, &s_forget_timer);
    }
    esp_timer_start_once(s_forget_timer, 1000000);

    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// ── OTA ──────────────────────────────────────────────────────────────────
extern "C" ApiStatus api_ota_s3(const char* body, size_t body_len,
                                 char* rsp_buf, size_t rsp_cap,
                                 size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* url_str = doc["url"] | (const char*)nullptr;
    if (!url_str || url_str[0] == '\0') return API_BAD_REQUEST;
    if (strlen(url_str) >= sizeof(s_ota_url)) return API_BAD_REQUEST;
    if (strncmp(url_str, "http://", 7) != 0 && strncmp(url_str, "https://", 8) != 0) {
        return API_BAD_REQUEST;
    }
    if (url_has_control_chars(url_str)) return API_BAD_REQUEST;
    strncpy(s_ota_url, url_str, sizeof(s_ota_url) - 1);
    xSemaphoreGive(s_ota_sem);

    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_ota_p4(const char* body, size_t body_len,
                                 char* rsp_buf, size_t rsp_cap,
                                 size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    const char* url_str = doc["url"] | (const char*)nullptr;
    if (!url_str || url_str[0] == '\0') return API_BAD_REQUEST;
    if (strlen(url_str) >= sizeof(s_p4ota_url)) return API_BAD_REQUEST;
    if (strncmp(url_str, "http://", 7) != 0 && strncmp(url_str, "https://", 8) != 0) {
        return API_BAD_REQUEST;
    }
    if (url_has_control_chars(url_str)) return API_BAD_REQUEST;
    strncpy(s_p4ota_url, url_str, sizeof(s_p4ota_url) - 1);
    xSemaphoreGive(s_p4ota_sem);

    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// ── Settings ─────────────────────────────────────────────────────────────
extern "C" ApiStatus api_settings_set(const char* body, size_t body_len,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;

    const char* broker_str = doc["broker_url"] | (const char*)nullptr;
    if (broker_str && broker_str[0] != '\0') {
        if (strlen(broker_str) >= 127) return API_BAD_REQUEST;
        char url[128] = {};
        strncpy(url, broker_str, sizeof(url) - 1);
        nvs_handle_t h;
        if (nvs_open("mqtt_cfg", NVS_READWRITE, &h) != ESP_OK) {
            return API_INTERNAL_ERROR;
        }
        nvs_set_str(h, "broker_url", url);
        nvs_commit(h);
        nvs_close(h);
        mqtt_gw_set_broker_url(url);
        ESP_LOGI(TAG_API, "MQTT broker updated: %s", url);
    }

    // P4-T29 (FINDINGS §6, :544): `sys_cfg` was opened+committed+closed THREE
    // separate times in one request (timezone / metrics_en / ap_disabled) —
    // three flash-page writes for a single "save settings". This handler is
    // operator-paced (a human pressing Save), so a background-flush timer is
    // unwarranted; instead the three same-namespace writes are STAGED into
    // locals here and written under ONE open/commit/close at the end of the
    // function. Live-apply side effects (setenv/tzset, the static flags) stay
    // exactly where they were — only the NVS persistence is coalesced.
    bool        sys_cfg_dirty = false;
    bool        set_tz        = false;  char tz_keep[64] = {};
    bool        set_metrics   = false;  uint8_t metrics_val = 0;
    bool        set_apdis     = false;  uint8_t apdis_val   = 0;

    const char* tz_str = doc["timezone"] | (const char*)nullptr;
    if (tz_str && tz_str[0] != '\0') {
        if (strlen(tz_str) >= 64) return API_BAD_REQUEST;
        strncpy(tz_keep, tz_str, sizeof(tz_keep) - 1);
        set_tz = sys_cfg_dirty = true;
        setenv("TZ", tz_str, 1);
        tzset();
        ESP_LOGI(TAG_API, "Timezone updated: %s", tz_str);
    }

    if (doc["metrics_enabled"].is<bool>()) {
        s_metrics_enabled = doc["metrics_enabled"].as<bool>();
        metrics_val = s_metrics_enabled ? 1 : 0;
        set_metrics = sys_cfg_dirty = true;
        ESP_LOGI(TAG_API, "Prometheus metrics: %s",
                 s_metrics_enabled ? "enabled" : "disabled");
    }

    if (doc["ap_disabled"].is<bool>()) {
        bool en = doc["ap_disabled"].as<bool>();
        s_ap_disabled = en;
        apdis_val = en ? 1 : 0;
        set_apdis = sys_cfg_dirty = true;
        ESP_LOGI(TAG_API, "AP disabled flag: %u (takes effect on next STA connect)",
                 en);
    }

    if (doc["mqtt_enabled"].is<bool>()) {
        bool en = doc["mqtt_enabled"].as<bool>();
        nvs_handle_t h;
        if (nvs_open("mqtt_cfg", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "enabled", en ? 1 : 0);
            nvs_commit(h);
            nvs_close(h);
        }
        // Apply live — start/stop the MQTT client now, no reboot needed.
        // On enable: re-read broker_url/cid/root from NVS (identity may
        // have been touched earlier in this same request body) and
        // restart the client. On disable: tear down cleanly.
        if (en) {
            char url[128] = {}, root[32] = {}, cid[32] = {};
            nvs_read_mqtt_cfg(nullptr,
                              url,  sizeof(url),
                              root, sizeof(root),
                              cid,  sizeof(cid));
            if (root[0]) mqtt_gw_set_root_topic(root);
            if (cid[0])  mqtt_gw_set_client_id(cid);
            if (url[0])  mqtt_gw_set_broker_url(url);
            else         mqtt_gw_start();  // broker URL already set earlier
            ESP_LOGI(TAG_API, "MQTT: enabled (applied)");
        } else {
            mqtt_gw_stop();
            ESP_LOGI(TAG_API, "MQTT: disabled (applied)");
        }
    }

    if (doc["auth_enabled"].is<bool>()) {
        s_auth_enabled = doc["auth_enabled"].as<bool>();
        nvs_handle_t h;
        if (nvs_open("zhac_auth", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "enabled", s_auth_enabled ? 1 : 0);
            nvs_commit(h);
            nvs_close(h);
        }
        ws_server_set_api_token(s_auth_enabled ? s_api_token : nullptr);
        ESP_LOGI(TAG_API, "API auth: %s", s_auth_enabled ? "enabled" : "disabled");
    }

    {
        nvs_handle_t h = 0;
        bool changed = false;
        const char* root = doc["mqtt_root_topic"] | (const char*)nullptr;
        const char* cid  = doc["mqtt_client_id"]  | (const char*)nullptr;
        if ((root && root[0]) || (cid && cid[0])) {
            if (nvs_open("mqtt_cfg", NVS_READWRITE, &h) == ESP_OK) {
                if (root && root[0] && strlen(root) < 32) {
                    nvs_set_str(h, "root_topic", root);
                    mqtt_gw_set_root_topic(root);
                    ESP_LOGI(TAG_API, "MQTT root topic: %s", root);
                    changed = true;
                }
                if (cid && cid[0] && strlen(cid) < 32) {
                    nvs_set_str(h, "client_id", cid);
                    mqtt_gw_set_client_id(cid);
                    ESP_LOGI(TAG_API, "MQTT client_id: %s", cid);
                    changed = true;
                }
                if (changed) nvs_commit(h);
                nvs_close(h);
            }
        }
    }

    {
        bool mqtt_en = log_sinks_get_mqtt_enabled();
        char mqtt_lv = log_sinks_get_mqtt_level();
        bool ws_en   = log_sinks_get_ws_enabled();
        char ws_lv   = log_sinks_get_ws_level();
        bool touched = false;
        if (doc["log_mqtt_enabled"].is<bool>()) {
            mqtt_en = doc["log_mqtt_enabled"].as<bool>(); touched = true;
        }
        if (doc["log_mqtt_level"].is<const char*>()) {
            const char* s = doc["log_mqtt_level"].as<const char*>();
            if (s && s[0]) { mqtt_lv = s[0]; touched = true; }
        }
        if (doc["log_ws_enabled"].is<bool>()) {
            ws_en = doc["log_ws_enabled"].as<bool>(); touched = true;
        }
        if (doc["log_ws_level"].is<const char*>()) {
            const char* s = doc["log_ws_level"].as<const char*>();
            if (s && s[0]) { ws_lv = s[0]; touched = true; }
        }
        if (touched) {
            log_sinks_set_mqtt(mqtt_en, mqtt_lv);
            log_sinks_set_ws  (ws_en,   ws_lv);
            nvs_handle_t h;
            if (nvs_open("log_cfg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "mqtt_en",  mqtt_en ? 1 : 0);
                nvs_set_u8(h, "mqtt_lvl", (uint8_t)mqtt_lv);
                nvs_set_u8(h, "ws_en",    ws_en ? 1 : 0);
                nvs_set_u8(h, "ws_lvl",   (uint8_t)ws_lv);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG_API, "Log sinks: mqtt=%s(%c) ws=%s(%c)",
                     mqtt_en ? "on" : "off", mqtt_lv,
                     ws_en   ? "on" : "off", ws_lv);
        }
    }

    // P4-T29: single coalesced sys_cfg write for whatever subset of
    // timezone / metrics_en / ap_disabled this request touched — one
    // open + (N) sets + one commit + one close, instead of up to three.
    if (sys_cfg_dirty) {
        nvs_handle_t h;
        if (nvs_open("sys_cfg", NVS_READWRITE, &h) == ESP_OK) {
            if (set_tz)      nvs_set_str(h, "timezone",   tz_keep);
            if (set_metrics) nvs_set_u8 (h, "metrics_en", metrics_val);
            if (set_apdis)   nvs_set_u8 (h, "ap_disabled", apdis_val);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

// ── Zigbee control ───────────────────────────────────────────────────────

// Local view of the permit-join window. The P4 is authoritative; we
// track what we last requested so the UI can query the current state
// without a round-trip to P4. Cleared by duration=0.
static int64_t s_permit_join_deadline_us = 0;
static uint8_t s_permit_join_duration    = 0;

extern "C" ApiStatus api_zigbee_permit_join(const char* body, size_t body_len,
                                             char* rsp_buf, size_t rsp_cap,
                                             size_t* rsp_len) {
    JsonDocument doc;
    if (body_len > 0) {
        if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;
    }
    uint8_t duration = doc["duration"] | (uint8_t)254;
    // F21 (FINDINGS.md): clamp to ≤254 s. 255 is the Zigbee "open
    // indefinitely" value, which would leave the network permanently
    // joinable; force a finite, re-armable window instead.
    if (duration > 254) duration = 254;

    uint8_t hap_buf[48];
    uint16_t hap_len = 0;
    if (!hap_json_encode_permit_join(hap_buf, sizeof(hap_buf), &hap_len, duration)) {
        return API_INTERNAL_ERROR;
    }
    hap_send(HapMsgType::PERMIT_JOIN, hap_buf, hap_len);

    if (duration > 0) {
        s_permit_join_deadline_us = esp_timer_get_time()
                                  + (int64_t)duration * 1000000LL;
        s_permit_join_duration    = duration;
    } else {
        s_permit_join_deadline_us = 0;
        s_permit_join_duration    = 0;
    }

    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_zigbee_permit_join_status(const char* /*body*/,
                                                    size_t /*body_len*/,
                                                    char* rsp_buf,
                                                    size_t rsp_cap,
                                                    size_t* rsp_len) {
    const int64_t now_us = esp_timer_get_time();
    const bool    open   = s_permit_join_deadline_us > now_us;
    const int     remaining_sec = open
        ? (int)((s_permit_join_deadline_us - now_us + 999999LL) / 1000000LL)
        : 0;
    const int n = snprintf(rsp_buf, rsp_cap,
        "{\"ok\":true,\"data\":{\"open\":%s,\"remaining_sec\":%d,"
        "\"duration\":%u}}",
        open ? "true" : "false", remaining_sec,
        (unsigned)s_permit_join_duration);
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_zigbee_reset(const char* /*body*/, size_t /*body_len*/,
                                       char* rsp_buf, size_t rsp_cap,
                                       size_t* rsp_len) {
    ESP_LOGW(TAG_API, "Zigbee factory reset requested via REST API");
    hap_send(HapMsgType::ZIGBEE_FACTORY_RESET, nullptr, 0);
    const size_t n = api_write_ok(rsp_buf, rsp_cap);
    if (rsp_len) *rsp_len = n;
    return API_OK;
}

extern "C" ApiStatus api_zigbee_settings_set(const char* body, size_t body_len,
                                              char* rsp_buf, size_t rsp_cap,
                                              size_t* rsp_len) {
    if (body_len == 0) return API_BAD_REQUEST;
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;

    int8_t      chan     = -1;
    const char* key_hex  = nullptr;
    bool        regen    = false;
    if (doc["channel"].is<int>()) {
        int c = doc["channel"].as<int>();
        if (c < 11 || c > 26) return API_BAD_REQUEST;
        chan = (int8_t)c;
    }
    if (doc["net_key_hex"].is<const char*>()) {
        key_hex = doc["net_key_hex"].as<const char*>();
        if (key_hex && strlen(key_hex) != 32) return API_BAD_REQUEST;
    }
    if (doc["regenerate"].is<bool>()) regen = doc["regenerate"].as<bool>();

    uint8_t  hap_buf[96];
    uint16_t hap_len = 0;
    if (!hap_json_encode_zigbee_cfg_set(hap_buf, sizeof(hap_buf), &hap_len,
                                         chan, key_hex, regen)) {
        return API_INTERNAL_ERROR;
    }

    char ack_buf[128];
    size_t ack_len = 0;
    bool got_rsp = hap_roundtrip_v2(HapMsgType::ZIGBEE_CFG_SET,
                                     hap_buf, hap_len,
                                     ack_buf, sizeof(ack_buf), &ack_len, 5000);
    bool    cmd_ok      = false;
    uint8_t rsp_channel = 11;
    bool    rsp_key_set = false;
    if (got_rsp && ack_len > 0) {
        JsonDocument doc2;
        if (deserializeJson(doc2, ack_buf, ack_len) == DeserializationError::Ok) {
            cmd_ok      = doc2["ok"]            | false;
            rsp_channel = (uint8_t)(doc2["channel"]     | 11);
            rsp_key_set = doc2["net_key_set"]   | false;
        }
    }
    const bool ok = got_rsp && cmd_ok;

    int n = snprintf(rsp_buf, rsp_cap,
                     "{\"ok\":%s,\"channel\":%u,\"net_key_set\":%s,"
                     "\"applies\":\"after factory reset\"}",
                     ok ? "true" : "false",
                     (unsigned)rsp_channel,
                     rsp_key_set ? "true" : "false");
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

// Defined in main.cpp — generates new random token, persists to NVS,
// re-arms ws_server. Returns false on NVS failure.
extern "C" bool auth_rotate_token(char* out, size_t out_cap);

extern "C" ApiStatus api_token_rotate(const char* /*body*/, size_t /*body_len*/,
                                       char* rsp_buf, size_t rsp_cap, size_t* rsp_len) {
    char fresh[33] = {};
    if (!auth_rotate_token(fresh, sizeof(fresh))) {
        int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":false,\"err\":\"nvs\"}");
        if (rsp_len) *rsp_len = (size_t)n;
        return API_INTERNAL_ERROR;
    }
    int n = snprintf(rsp_buf, rsp_cap,
                     "{\"ok\":true,\"token\":\"%s\"}", fresh);
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    if (rsp_len) *rsp_len = (size_t)n;
    return API_OK;
}

