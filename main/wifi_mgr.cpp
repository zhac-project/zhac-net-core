// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wifi_mgr.h"
#include "mqtt_gw.h"

#include <atomic>
#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
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
// T16 (FINDINGS §5.1): retry counter is touched from the esp_event task
// (incremented in STA_DISCONNECTED, reset in STA_GOT_IP) and read nowhere
// else after the backoff was moved off the loop — the reconnect timer cb
// no longer inspects it. std::atomic is kept as a defensive belt: the
// timer arming and the handler both run as short, non-overlapping critical
// regions, so there is no torn read, but atomicity costs nothing here and
// documents the cross-context intent.
static std::atomic<int> s_retry_count{0};
// F38 (FINDINGS.md): raised 5 → 20 so a transient AP/handshake blip can't
// wipe valid credentials. Only sustained failure (~160 s given the backoff
// below) clears them, and the AP stays up for manual re-provision regardless.
// NO_AP_FOUND / BEACON_TIMEOUT never trigger a wipe (see credential_error).
static constexpr int   MAX_STA_RETRIES = 20;  // sustained-failure threshold; AP stays up
static char            s_ap_ssid[16]   = {};
static char            s_ip_str[20]    = "0.0.0.0";

static esp_netif_t*    s_netif_sta     = nullptr;
static esp_netif_t*    s_netif_ap      = nullptr;

// Scan results — PSRAM: ~1.6 KB, written once per user-triggered scan
// (esp_wifi_scan_get_ap_records runs in task context, never ISR/DMA).
// T16 (FINDINGS §5.2): wifi.scan is remote-allow-listed, so the local httpd
// task and the remote-client task can both drive a scan concurrently. The
// trigger+write and the snapshot read are serialised under s_scan_mtx so a
// concurrent scan can't tear the array out from under a reader.
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_scan_results[WIFI_MGR_MAX_SCAN];
static uint16_t           s_scan_count = 0;
static SemaphoreHandle_t  s_scan_mtx   = nullptr;

// T16 (FINDINGS §5.1): one-shot reconnect timer. The STA_DISCONNECTED handler
// runs in the default esp_event task and must NEVER sleep — a vTaskDelay there
// stalls IP/AP events, MQTT bring-up and remote_client wifi bits for the whole
// backoff (up to 60 s). Instead the handler arms this timer with the computed
// backoff and returns; the callback (esp_timer task, NOT an ISR) issues the
// reconnect.
// Never esp_timer_delete'd — process-lifetime singleton, reused per disconnect.
static esp_timer_handle_t s_reconnect_timer = nullptr;

// T16 (FINDINGS §5.3): shared across both AP-start paths AND the DNS task so
// the task can clear it on clean exit and a later AP-enable can respawn it.
// Was two independent function-local statics (could never re-arm correctly).
static bool               s_dns_started = false;

// ── Forward declarations ─────────────────────────────────────────────────
static void start_ap_mode(void);
static void start_sta_mode(const char* ssid, const char* pass);
static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data);
static void task_dns_captive(void*);
static void task_gpio_reset(void*);
static bool load_ap_disabled(void);
static void clear_sta_creds(void);
static void reconnect_timer_cb(void*);
static void arm_reconnect_timer(uint32_t backoff_ms);

// ── Event handler ────────────────────────────────────────────────────────
// Provisioning-AP auto-drop (REPORT.md §2.2). The SoftAP is only needed until
// WiFi is configured; leaving it up in APSTA for the whole runtime is idle
// attack surface. Once STA has held a connection for AP_GRACE_MS we drop to
// STA-only. Armed on GOT_IP, cancelled on STA disconnect (so a flaky link never
// strands the user); re-provisioning is via the WiFi-reset button (erase creds
// + reboot into AP).
static constexpr int64_t  AP_GRACE_MS      = 120000;   // 2 min of stable STA
static esp_timer_handle_t s_ap_grace_timer = nullptr;

static void ap_grace_cb(void*) {
    if (s_wifi_connected.load(std::memory_order_acquire) && s_ap_mode) {
        ESP_LOGI(TAG, "STA stable %llds — dropping provisioning AP (STA-only)",
                 AP_GRACE_MS / 1000);
        s_ap_mode = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

static void arm_ap_grace_timer(void) {
    if (!s_ap_grace_timer) {
        const esp_timer_create_args_t a = {
            .callback = ap_grace_cb, .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK, .name = "ap_grace",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&a, &s_ap_grace_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_ap_grace_timer);                 // no-op if not running
    esp_timer_start_once(s_ap_grace_timer, AP_GRACE_MS * 1000);
}

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t id, void* event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected.store(false, std::memory_order_release);
        if (s_ap_grace_timer) esp_timer_stop(s_ap_grace_timer);  // link lost — cancel AP auto-drop
        const int retry = s_retry_count.fetch_add(1, std::memory_order_acq_rel) + 1;
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
        if (credential_error && retry > MAX_STA_RETRIES) {
            ESP_LOGE(TAG, "STA failed after %d retries (last: %s, code=%u) — clearing credentials, AP stays up", MAX_STA_RETRIES, reason_str, reason);
            clear_sta_creds();
            // Keep AP running; don't restart WiFi stack (avoids httpd socket churn)
            return;
        }
        ESP_LOGW(TAG, "STA disconnected %d: %s (code=%u)", retry, reason_str, reason);
        // Backoff: 2 s for the first 5 retries, 10 s for 5..30, 60 s thereafter.
        // T16 (FINDINGS §5.1): schedule the reconnect via a one-shot timer
        // instead of sleeping here — this handler runs in the esp_event task.
        // Schedule is unchanged; only WHERE the wait happens moved.
        uint32_t backoff_ms = 2000;
        if      (retry > 30) backoff_ms = 60000;
        else if (retry >  5) backoff_ms = 10000;
        arm_reconnect_timer(backoff_ms);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(event_data);
        s_retry_count.store(0, std::memory_order_release);
        // A reconnect may have been queued just before the link came up;
        // cancel it so a stale timer can't fire esp_wifi_connect() on a
        // healthy link (harmless but avoids a spurious disconnect churn).
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
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
        } else if (s_ap_mode) {
            // REPORT.md §2.2: not manually disabled — auto-drop the AP once STA
            // has been stable for the grace period (first-boot provisioning
            // still works; WiFi-reset button re-opens the AP).
            arm_ap_grace_timer();
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

// ── Reconnect backoff (off the event loop) ─────────────────────────────────
// T16 (FINDINGS §5.1): runs in the esp_timer task (task dispatch, NOT an ISR),
// so calling esp_wifi_connect() here is safe. Keep it short.
static void reconnect_timer_cb(void*) {
    esp_wifi_connect();
}

// Arm the one-shot reconnect timer with @p backoff_ms. esp_timer_start_once
// rejects an already-running timer, so stop it first (idempotent if idle).
// Lazily created on first use.
static void arm_reconnect_timer(uint32_t backoff_ms) {
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback              = &reconnect_timer_cb,
            .arg                   = nullptr,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "wifi_reconnect",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
            // Defensive: timer unavailable — fall back to an immediate
            // reconnect so the STA still retries (no backoff, but never
            // sleeps the event loop).
            ESP_LOGE(TAG, "reconnect timer create failed — reconnecting now");
            esp_wifi_connect();
            return;
        }
    }
    esp_timer_stop(s_reconnect_timer);  // no-op if not running
    esp_err_t err = esp_timer_start_once(s_reconnect_timer,
                                         (uint64_t)backoff_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconnect timer start failed (%s) — reconnecting now",
                 esp_err_to_name(err));
        esp_wifi_connect();
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

    // Mode BEFORE config: esp_wifi_set_config(WIFI_IF_AP) inside configure_ap()
    // requires the AP interface to already be enabled in the current mode —
    // otherwise it fails ESP_ERR_WIFI_MODE (0x3005). APSTA so scan works.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "AP mode started — SSID: %s  IP: %s", s_ap_ssid, s_ip_str);

    if (!s_dns_started) {
        s_dns_started = true;
        xTaskCreate(task_dns_captive, "TaskDNS", 3072, nullptr, 3, nullptr);
    }
}

// STA concurrent with AP (captive portal stays alive during connect attempts)
static void start_sta_mode(const char* ssid, const char* pass) {
    s_ap_mode = true;  // AP stays up alongside STA
    s_retry_count.store(0, std::memory_order_release);

    // Mode BEFORE config: esp_wifi_set_config(WIFI_IF_AP/STA) inside
    // configure_ap()/configure_sta() requires the interface enabled first —
    // otherwise ESP_ERR_WIFI_MODE (0x3005).
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    configure_sta(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "APSTA started — AP: %s  connecting to STA: \"%s\"", s_ap_ssid, ssid);

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
        s_dns_started = false;  // allow a later AP-enable to respawn
        vTaskDelete(nullptr);
        return;
    }

    // T16 (FINDINGS §5.3): bound the recv so the loop can poll s_ap_mode and
    // exit promptly after AP-stop instead of blocking forever (leaking the
    // socket + ~3 KB stack and wedging s_dns_started until a stray packet).
    struct timeval rcv_to = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));

    struct sockaddr_in saddr = {};
    saddr.sin_family      = AF_INET;
    saddr.sin_port        = htons(53);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        s_dns_started = false;  // allow a later AP-enable to respawn
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
        // Recv timeout (no packet within 1 s): re-check s_ap_mode and loop.
        // EAGAIN/EWOULDBLOCK is the timeout; anything else is also non-fatal
        // here — just re-poll the AP flag. The 1 s timeout paces the spin.
        if (n < 0) continue;
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

    // AP was disabled — tear down cleanly so a later AP-enable can respawn.
    close(sock);
    s_dns_started = false;
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

    // T16 (FINDINGS §5.2): guards the shared scan-results array against
    // concurrent local-httpd / remote-client scans. Created before any task
    // that could trigger a scan is spawned.
    if (!s_scan_mtx) s_scan_mtx = xSemaphoreCreateMutex();

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

    // T16 (FINDINGS §5.2): the blocking scan_start runs OUTSIDE the mutex —
    // esp_wifi's scan engine is single-instance, so a concurrent caller's
    // scan_start simply errors out (handled below) rather than corrupting
    // shared state. The mutex is held only around the (non-blocking) results
    // read-back and count store, which is the actual torn-data window.
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t got = sizeof(s_scan_results) / sizeof(s_scan_results[0]);
    if (s_scan_mtx) xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    err = esp_wifi_scan_get_ap_records(&got, s_scan_results);
    s_scan_count = (err == ESP_OK) ? got : 0;
    const uint16_t count = s_scan_count;
    if (s_scan_mtx) xSemaphoreGive(s_scan_mtx);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan get results failed: %s", esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "Scan complete: %u APs found", count);
    return count;
}

uint16_t wifi_mgr_get_scan_results(wifi_ap_record_t* out, uint16_t max) {
    if (!out || max == 0) return 0;
    // T16 (FINDINGS §5.2): copy a stable snapshot under the lock so a
    // concurrent scan can't overwrite the array while the caller iterates.
    if (s_scan_mtx) xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    uint16_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan_results, (size_t)n * sizeof(s_scan_results[0]));
    if (s_scan_mtx) xSemaphoreGive(s_scan_mtx);
    return n;
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
