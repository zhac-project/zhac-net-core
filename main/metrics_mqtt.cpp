// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// metrics_mqtt.cpp — periodic MQTT snapshot publisher.
//
// Owns an esp_timer that fires every CONFIG_METRICS_MQTT_INTERVAL_S
// seconds, formats the metric snapshot into a static buffer, and
// publishes to `<root>/metrics/snapshot`. No-op when the MQTT exporter
// or the metrics core is disabled in sdkconfig.

#include "sdkconfig.h"

#if CONFIG_METRICS_ENABLED && CONFIG_METRICS_ENABLE_MQTT_EXPORTER

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "metrics/metrics.h"
#include "metrics/metrics_export_mqtt.h"
#include "mqtt_gw.h"

#include <cstdint>

namespace {

constexpr const char* TAG     = "metrics_mqtt";
constexpr const char* kTopic  = "metrics/snapshot";
constexpr size_t      kBufLen = 2048;

esp_timer_handle_t s_timer = nullptr;
// PSRAM: 2 KB snapshot scratch, touched once per publish interval from the
// esp_timer task (never ISR) — cold path, no reason to burn internal DRAM.
EXT_RAM_BSS_ATTR char s_buf[kBufLen];

void on_tick(void*) {
    // F45 (FINDINGS.md): the snapshot carries internal metrics + heap layout.
    // Only publish over a verified-TLS broker — never broadcast it broker-wide
    // in cleartext where anyone subscribed can read it (compounds Finding 10).
    if (!mqtt_gw_is_connected() || !mqtt_gw_is_secure()) return;
    metrics::update_memory_snapshot();
    const size_t n = metrics::mqtt_format_snapshot_json(s_buf, sizeof(s_buf));
    if (n == 0) {
        // The exporter returns 0 only when the snapshot did not fit
        // s_buf and was therefore truncated to invalid JSON — we skip
        // the publish (FINDINGS §8). With kBufLen = 2048 this should
        // never trip; if it does, a metric was added that blows the
        // buffer and the snapshot will silently stop publishing, so
        // surface it. Rate-limited to one line per ~5 min so a
        // chronically-oversized snapshot can't spam the log.
        static int64_t s_last_warn_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_warn_us > 300LL * 1000000LL) {
            s_last_warn_us = now_us;
            ESP_LOGW(TAG, "snapshot truncated (>%u B) — publish skipped; "
                          "raise kBufLen or trim metrics",
                     (unsigned)kBufLen);
        }
        return;
    }
    mqtt_gw_publish(kTopic, s_buf, n, 0, false);
}

}  // namespace

extern "C" void metrics_mqtt_publisher_start() {
    if (s_timer) return;
    const uint64_t period_us =
        static_cast<uint64_t>(CONFIG_METRICS_MQTT_INTERVAL_S) * 1000000ULL;
    esp_timer_create_args_t args{};
    args.callback = on_tick;
    args.name     = "metrics_mqtt";
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer create failed");
        return;
    }
    esp_timer_start_periodic(s_timer, period_us);
    ESP_LOGI(TAG, "publisher started, interval=%ds topic=%s",
             (int)CONFIG_METRICS_MQTT_INTERVAL_S, kTopic);
}

#else

extern "C" void metrics_mqtt_publisher_start() {}

#endif
