// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wifi_mgr.h"
#include "mqtt_gw.h"

#include <atomic>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "s3_internal.h"

static const char* TAG = "wifi_mgr";

// ── Internal state ───────────────────────────────────────────────────────
static bool            s_ap_mode       = false;
static int             s_retry_count   = 0;
// F38 (FINDINGS.md): raised 5 → 20 so a transient AP/handshake blip can't
// wipe valid credentials. Only sustained failure (~160 s given the backoff
// below) clears them, and the AP stays up for manual re-provision regardless.
// NO_AP_FOUND / BEACON_TIMEOUT never trigger a wipe (see credential_error).
static constexpr int   MAX_STA_RETRIES = 20;  // sustained-failure threshold; AP stays up
static char            s_ap_ssid[16]   = {};
static char            s_ip_str[20]    = "0.0.0.0";

static esp_netif_t*    s_netif_sta     = nullptr;
static esp_netif_t*    s_netif_ap      = nullptr;

// Scan results
static wifi_ap_record_t s_scan_results[20];
static uint16_t         s_scan_count = 0;

// ── Forward declarations ─────────────────────────────────────────────────
static void start_ap_mode(void);
static void start_sta_mode(const char* ssid, const char* pass);
static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data);
static void task_dns_captive(void*);
static void task_gpio_reset(void*);
static bool load_ap_disabled(void);
static void clear_sta_creds(void);

// ── Event handler ────────────────────────────────────────────────────────
static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t id, void* event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected.store(false, std::memory_order_release);
        s_retry_count++;
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        uint8_t reason = ev ? ev->reason : 0;
        const char* reason_str;
        switch (reason) {
            case WIFI_REASON_AUTH_EXPIRE:            reason_str = "auth expired"; break;
            case WIFI_REASON_AUTH_LEAVE:             reason_str = "auth leave"; break;
            case WIFI_REASON_ASSOC_TOOMANY:          reason_str = "too many stations"; break;
            case WIFI_REASON_ASSOC_LEAVE:            reason_str = "assoc leave"; break;
            case WIFI_REASON_ASSOC_NOT_AUTHED:       reason_str = "assoc not authed"; break;
            case WIFI_REASON_BEACON_TIMEOUT:         reason_str = "beacon timeout (AP out of range?)"; break;
            case WIFI_REASON_NO_AP_FOUND:            reason_str = "AP not found (wrong SSID?)"; break;
            case WIFI_REASON_AUTH_FAIL:              reason_str = "auth failed (wrong password)"; break;
            case WIFI_REASON_ASSOC_FAIL:             reason_str = "assoc failed"; break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT:      reason_str = "handshake timeout (wrong password)"; break;
            case WIFI_REASON_CONNECTION_FAIL:        reason_str = "connection failed (AP rejected)"; break;
            case WIFI_REASON_AP_TSF_RESET:           reason_str = "AP TSF reset"; break;
            case WIFI_REASON_ROAMING:                reason_str = "roaming"; break;
            default:                                 reason_str = "unknown"; break;
        }
        // Only wipe NVS creds on errors that genuinely point at wrong
        // password / wrong SSID accepted at AUTH layer. NO_AP_FOUND and
        // BEACON_TIMEOUT are routinely caused by transient AP outages
        // (reboot, power blip, channel scan miss) — wiping creds there
        // forced an unrelated factory-reset path on real users. Keep
        // retrying with progressive backoff for those instead.
        const bool credential_error =
            (reason == WIFI_REASON_AUTH_FAIL ||
             reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
        if (credential_error && s_retry_count > MAX_STA_RETRIES) {
            ESP_LOGE(TAG, "STA failed after %d retries (last: %s, code=%u) — clearing credentials, AP stays up", MAX_STA_RETRIES, reason_str, reason);
            clear_sta_creds();
            // Keep AP running; don't restart WiFi stack (avoids httpd socket churn)
            return;
        }
        ESP_LOGW(TAG, "STA disconnected %d: %s (code=%u)", s_retry_count, reason_str, reason);
        // Backoff: 2 s for the first 5 retries, 10 s for 5..30, 60 s thereafter.
        uint32_t backoff_ms = 2000;
        if      (s_retry_count > 30) backoff_ms = 60000;
        else if (s_retry_count >  5) backoff_ms = 10000;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(event_data);
        s_retry_count = 0;
        s_wifi_connected.store(true, std::memory_order_release);
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA connected — IP: %s", s_ip_str);

        // Bring up MQTT only now — before this point esp-mqtt would
        // hit fast-path disconnect loops and trip its auto-disable cap.
        mqtt_gw_on_sta_up();

        // User opted to disable AP once STA is up
        if (load_ap_disabled()) {
            ESP_LOGI(TAG, "ap_disabled=1 — switching to STA-only mode");
            s_ap_mode = false;
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        // Start SNTP once
        static bool s_sntp_started = false;
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP started");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        auto* ev = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(TAG, "AP: station " MACSTR " joined (aid=%d)",
                 MAC2STR(ev->mac), ev->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto* ev = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(TAG, "AP: station " MACSTR " left (aid=%d)",
                 MAC2STR(ev->mac), ev->aid);
    }
}

// ── AP mode ──────────────────────────────────────────────────────────────
static void build_ap_ssid(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ZHAC-%02X%02X",
             mac[4], mac[5]);
}

// Load ap_disabled flag from NVS (set via /api/settings after STA is stable)
static bool load_ap_disabled(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("sys_cfg", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "ap_disabled", &v);
        nvs_close(h);
    }
    return v != 0;
}

static void clear_sta_creds(void) {
    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "STA credentials cleared from NVS");
    }
}

// F12/A5 (FINDINGS.md): random per-device WPA2 PSK for the provisioning AP,
// persisted in NVS. Random (NOT MAC-derived) because the AP BSSID == MAC is
// broadcast in every beacon; a derived key would be trivially recoverable.
// 12 hex chars (>= the WPA2 8-char minimum). Printed to serial on bring-up.
static char s_ap_pass[16] = {};
static void load_or_gen_ap_pass(void) {
    if (s_ap_pass[0]) return;
    nvs_handle_t h;
    if (nvs_open("sys_cfg", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_ap_pass);
        if (nvs_get_str(h, "ap_pass", s_ap_pass, &len) != ESP_OK || !s_ap_pass[0]) {
            uint8_t r[6];
            esp_fill_random(r, sizeof(r));
            for (int i = 0; i < 6; i++) snprintf(s_ap_pass + i * 2, 3, "%02x", r[i]);
            nvs_set_str(h, "ap_pass", s_ap_pass);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    if (!s_ap_pass[0]) {
        // Q61 (QWEN_FINDINGS triage): NVS unavailable (open failed above) — fall
        // back to a random per-boot PSK, never the well-known "zhacsetup". Not
        // persisted (NVS is down) so it differs each boot, but it's serial-printed
        // below, so the operator still learns it — and it's never guessable.
        uint8_t r[6];
        esp_fill_random(r, sizeof(r));
        for (int i = 0; i < 6; i++) snprintf(s_ap_pass + i * 2, 3, "%02x", r[i]);
    }
}

static void configure_ap(void) {
    build_ap_ssid();
    load_or_gen_ap_pass();
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {};
    strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel        = 1;
    // F12/A5 (FINDINGS.md): WPA2-protect the provisioning AP (was OPEN — any
    // RF-adjacent client could join the captive portal and drive setup). PSK
    // is the random, NVS-persisted per-device value from load_or_gen_ap_pass.
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), s_ap_pass,
            sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    // Serial/label is the out-of-band channel for the operator to learn the
    // PSK (same model as the API token).
    printf("\n*** ZHAC provisioning AP (WPA2): SSID=%s  password=%s ***\n\n",
           s_ap_ssid, s_ap_pass);
    fflush(stdout);
}

static void configure_sta(const char* ssid, const char* pass) {
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t sta_cfg = {};
    {
        const size_t cap = sizeof(sta_cfg.sta.ssid) - 1;
        const size_t n   = strnlen(ssid, cap);
        memcpy(sta_cfg.sta.ssid, ssid, n);
    }
    {
        const size_t cap = sizeof(sta_cfg.sta.password) - 1;
        const size_t n   = strnlen(pass, cap);
        memcpy(sta_cfg.sta.password, pass, n);
    }
    // F12 (FINDINGS.md): pin the STA minimum auth level so the device won't
    // associate to a rogue OPEN AP advertising the saved SSID (evil-twin /
    // downgrade). Open only when no password was provisioned.
    sta_cfg.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
}

// AP-only (no creds OR STA gave up)
static void start_ap_mode(void) {
    s_ap_mode = true;
    s_wifi_connected.store(false, std::memory_order_release);

    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));  // APSTA so scan works
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "AP mode started — SSID: %s  IP: %s", s_ap_ssid, s_ip_str);

    static bool s_dns_started = false;
    if (!s_dns_started) {
        s_dns_started = true;
        xTaskCreate(task_dns_captive, "TaskDNS", 3072, nullptr, 3, nullptr);
    }
}

// STA concurrent with AP (captive portal stays alive during connect attempts)
static void start_sta_mode(const char* ssid, const char* pass) {
    s_ap_mode = true;  // AP stays up alongside STA
    s_retry_count = 0;

    configure_ap();
    configure_sta(ssid, pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "APSTA started — AP: %s  connecting to STA: \"%s\"", s_ap_ssid, ssid);

    static bool s_dns_started = false;
    if (!s_dns_started) {
        s_dns_started = true;
        xTaskCreate(task_dns_captive, "TaskDNS", 3072, nullptr, 3, nullptr);
    }
}

// ── DNS captive portal ───────────────────────────────────────────────────
// Minimal DNS responder: answer every A query with 192.168.4.1
static void task_dns_captive(void*) {
    ESP_LOGI(TAG, "DNS captive portal started on :53");

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in saddr = {};
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(53);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in caddr;
    socklen_t clen;

    // 192.168.4.1 in network byte order
    const uint8_t captive_ip[4] = {192, 168, 4, 1};

    while (s_ap_mode) {
        clen = sizeof(caddr);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&caddr, &clen);
        if (n < 12) continue;  // too short for DNS header

        // Build response: copy query, set response flags, append answer
        // DNS header flags: QR=1 (response), AA=1 (authoritative), RCODE=0 (no error)
        buf[2] = 0x81;  // QR=1, Opcode=0, AA=1
        buf[3] = 0x80;  // RA=1, RCODE=0
        // ANCOUNT = 1
        buf[6] = 0x00;
        buf[7] = 0x01;

        // Append answer section after the query
        int off = n;
        if (off + 16 <= (int)sizeof(buf)) {
            // Name pointer to offset 12 (start of question)
            buf[off++] = 0xC0;
            buf[off++] = 0x0C;
            // Type A
            buf[off++] = 0x00;
            buf[off++] = 0x01;
            // Class IN
            buf[off++] = 0x00;
            buf[off++] = 0x01;
            // TTL 60 seconds
            buf[off++] = 0x00;
            buf[off++] = 0x00;
            buf[off++] = 0x00;
            buf[off++] = 0x3C;
            // RDLENGTH 4
            buf[off++] = 0x00;
            buf[off++] = 0x04;
            // RDATA = 192.168.4.1
            memcpy(buf + off, captive_ip, 4);
            off += 4;
        }

        sendto(sock, buf, off, 0, (struct sockaddr*)&caddr, clen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS captive portal stopped");
    vTaskDelete(nullptr);
}

// ── GPIO reset button ────────────────────────────────────────────────────
static void task_gpio_reset(void*) {
    const gpio_num_t btn = static_cast<gpio_num_t>(CONFIG_ZHAC_WIFI_RESET_GPIO);

    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = 1ULL << btn;
    io_cfg.mode         = GPIO_MODE_INPUT;
    io_cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_cfg.intr_type    = GPIO_INTR_DISABLE;
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
    io_cfg.hys_ctrl_mode = GPIO_HYS_SOFT_ENABLE;
#endif
    gpio_config(&io_cfg);

    int held_ms = 0;
    const int poll_ms   = 100;
    const int target_ms = 5000;

    ESP_LOGI(TAG, "GPIO reset task started (GPIO%d, hold %ds to erase WiFi)",
             btn, target_ms / 1000);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        // Active-low: pressed when level == 0
        if (gpio_get_level(btn) == 0) {
            held_ms += poll_ms;
            if (held_ms >= target_ms) {
                ESP_LOGW(TAG, "GPIO reset triggered — erasing WiFi credentials");
                wifi_mgr_forget_and_reboot();
                // does not return
            }
        } else {
            if (held_ms > 0) {
                ESP_LOGD(TAG, "Button released after %d ms (need %d ms)", held_ms, target_ms);
            }
            held_ms = 0;
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────

void wifi_mgr_init(void) {
    ESP_LOGI(TAG, "Initialising WiFi manager");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

    // Build AP SSID early so wifi_mgr_get_ap_ssid() works before AP starts
    build_ap_ssid();

    // Try loading credentials from NVS
    char ssid[33] = {};
    char pass[65] = {};
    bool has_creds = false;
    {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
            size_t slen = sizeof(ssid);
            size_t plen = sizeof(pass);
            esp_err_t s_err = nvs_get_str(h, "ssid", ssid, &slen);
            nvs_get_str(h, "pass", pass, &plen);
            nvs_close(h);
            if (s_err == ESP_OK && ssid[0] != '\0') {
                has_creds = true;
                ESP_LOGI(TAG, "NVS credentials found for \"%s\"", ssid);
            }
        }
    }

    // Kconfig fallback removed 2026-04-20 — compile-time credentials
    // leaked into build artefacts and core dumps. Empty NVS now falls
    // through to AP-mode provisioning; the user POSTs creds to
    // /api/wifi from the captive portal.

    if (has_creds) {
        start_sta_mode(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials — starting AP mode");
        start_ap_mode();
    }

    // GPIO reset button task
    xTaskCreate(task_gpio_reset, "TaskGPIORst", 2048, nullptr, 2, nullptr);
}

bool wifi_mgr_is_ap_mode(void) {
    return s_ap_mode;
}

void wifi_mgr_get_ip_str(char* buf, size_t len) {
    if (buf && len > 0) {
        strncpy(buf, s_ip_str, len - 1);
        buf[len - 1] = '\0';
    }
}

const char* wifi_mgr_get_ap_ssid(void) {
    return s_ap_ssid;
}

uint16_t wifi_mgr_scan(void) {
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        s_scan_count = 0;
        return 0;
    }

    s_scan_count = sizeof(s_scan_results) / sizeof(s_scan_results[0]);
    err = esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan get results failed: %s", esp_err_to_name(err));
        s_scan_count = 0;
        return 0;
    }

    ESP_LOGI(TAG, "Scan complete: %u APs found", s_scan_count);
    return s_scan_count;
}

const wifi_ap_record_t* wifi_mgr_get_scan_results(uint16_t* count_out) {
    if (count_out) *count_out = s_scan_count;
    return s_scan_results;
}

void wifi_mgr_forget_and_reboot(void) {
    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "WiFi credentials erased from NVS");
    }
    ESP_LOGW(TAG, "Rebooting into AP mode...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
