// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "s3_internal.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "nvs.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ws_server.h"
#include "mqtt_gw.h"
#include "ArduinoJson.h"
#include "metrics/metrics.h"
#include "metrics/metrics_export_prometheus.h"
#include "api_handlers.h"

static const char* TAG = "rest_ops";

// F33 (FINDINGS.md): per-request large buffers (device/rule list + export
// responses, 8–16 KB) routed to PSRAM so repeated big allocations don't churn
// the tight internal DRAM heap — a fragmentation DoS under request floods.
// Falls back to the default heap if PSRAM is unavailable. Pair with plain
// free() (ESP-IDF's free() handles PSRAM allocations).
void* rest_big_alloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    return p;
}

// F33 (FINDINGS.md): body read with an early Content-Length cap. Rejects an
// oversized body with 413 *before* reading it (rather than truncating into a
// fixed buffer and failing the JSON parse later) and 400s an empty/failed
// recv. On any error it sends the response and returns -1, so callers just
// do `if (n < 0) return ESP_OK;`. `cap` is the buffer size including the NUL
// slot; at most cap-1 bytes are read. Returns bytes read on success.
int rest_body_recv(httpd_req_t* req, char* buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    if (req->content_len >= cap) {
        ESP_LOGW(TAG, "body %u B exceeds cap %u — 413",
                 (unsigned)req->content_len, (unsigned)cap);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "request body too large");
        return -1;
    }
    int total = 0;
    while ((size_t)total < cap - 1) {
        int r = httpd_req_recv(req, buf + total, (size_t)(cap - 1 - (size_t)total));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        total += r;
        if (req->content_len && (size_t)total >= req->content_len) break;
    }
    if (total <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return -1;
    }
    buf[total] = '\0';
    return total;
}

// Call api_fn, map its status to an HTTP response, send. Used by every
// REST handler that just forwards body → api_fn → JSON back.
esp_err_t rest_api_reply(httpd_req_t* req, ApiHandlerFn fn,
                          const char* body, size_t body_len,
                          char* buf, size_t cap, const char* err_label) {
    size_t n = 0;
    ApiStatus st = fn(body, body_len, buf, cap, &n);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_label);
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_label);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, (ssize_t)n);
}

// ── GET /api/status ───────────────────────────────────────────────────────
esp_err_t handle_get_status(httpd_req_t* req) {
    // No REQUIRE_AUTH: /api/status is intentionally unauthenticated as
    // the discovery endpoint (non-sensitive health info only).
    // Function-static in PSRAM: esp_http_server serialises all handlers on
    // its single worker task (one httpd instance — ws_server.cpp), so this
    // cannot be entered concurrently; keeps 2 KB off stack + internal DRAM.
    EXT_RAM_BSS_ATTR static char buf[2048];
    return rest_api_reply(req, api_status_get, nullptr, 0, buf, sizeof(buf), "status format");
}

// ── GET /metrics  (Prometheus text format) ────────────────────────────────
esp_err_t handle_get_metrics(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    if (!s_metrics_enabled) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "metrics disabled — enable in Settings");
    }

    // Function-static so we don't chew ~8 KB of handler stack. Safe
    // because esp_http_server serialises handler invocations on a
    // single task, so no two concurrent /metrics requests race this
    // buffer.
    EXT_RAM_BSS_ATTR static char buf[8192];
    int         pos = 0;

#define METRIC(name, help, type, fmt, val) \
    pos += snprintf(buf + pos, (int)sizeof(buf) - pos, \
        "# HELP " name " " help "\n# TYPE " name " " type "\n" name " " fmt "\n", (val))

    METRIC("zhac_heap_free_bytes",     "Free heap on S3",            "gauge", "%" PRIu32, esp_get_free_heap_size());
    METRIC("zhac_heap_min_free_bytes", "Minimum free heap since boot","gauge", "%" PRIu32, esp_get_minimum_free_heap_size());
    METRIC("zhac_uptime_seconds",      "S3 uptime in seconds",        "counter","%" PRIu64, esp_timer_get_time() / 1000000ULL);
    METRIC("zhac_wifi_connected",      "1 if WiFi is connected",      "gauge", "%d", (int)s_wifi_connected.load(std::memory_order_acquire));
    METRIC("zhac_p4_synced",           "1 if S3-P4 HAP link is up",   "gauge", "%d", (int)s_synced.load(std::memory_order_acquire));
    METRIC("zhac_mqtt_connected",      "1 if MQTT broker is connected","gauge", "%d", (int)mqtt_gw_is_connected());
    METRIC("zhac_ws_clients",          "Active WebSocket clients",     "gauge", "%d", ws_server_client_count());
    METRIC("zhac_device_count",        "Zigbee devices known to P4",   "gauge", "%" PRIu16, s_p4_device_count.load(std::memory_order_relaxed));
    METRIC("zhac_p4_uptime_seconds",   "P4 uptime in seconds",         "counter","%" PRIu32, s_p4_uptime_s.load(std::memory_order_relaxed));
    METRIC("zhac_p4_psram_free_bytes", "Free PSRAM on P4",             "gauge", "%" PRIu32, s_p4_psram_free.load(std::memory_order_relaxed));
    METRIC("zhac_p4_proto_mask",       "Bitmask of running protocol backends","gauge", "%lu", (unsigned long)s_p4_proto_mask.load(std::memory_order_relaxed));
    METRIC("zhac_alert_log_count",     "Alerts in ring buffer",        "gauge", "%u", (unsigned)s_alert_log_count);
    uint32_t s3_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    METRIC("zhac_psram_free_bytes",    "Free PSRAM on S3",             "gauge", "%" PRIu32, s3_psram);
#undef METRIC

    // Append metrics-engine output (sampled timers / counters / values).
    // Existing handcoded gauges above use bare names; the engine always
    // emits suffixed names (_count / _sum / _min / _max / _avg / _last),
    // so they never collide.
    metrics::update_memory_snapshot();
    const int remaining = (int)sizeof(buf) - pos;
    if (remaining > 1) {
        const size_t n = metrics::prometheus_format(buf + pos,
                                                     (size_t)remaining,
                                                     "zhac");
        pos += (int)n;
    }

    // Append cached P4 metrics. Populated asynchronously by the hap
    // bridge task every HAP_METRICS_REFRESH_MS; zero-length when the
    // first response hasn't arrived yet (fresh boot / P4 restart).
    const int remaining_p4 = (int)sizeof(buf) - pos;
    if (remaining_p4 > 1) {
        const size_t n = hap_bridge_copy_p4_metrics(buf + pos,
                                                      (size_t)remaining_p4);
        pos += (int)n;
    }

    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    return httpd_resp_send(req, buf, pos);
}

// ── GET /api/alerts ───────────────────────────────────────────────────────
esp_err_t handle_get_alerts(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    // Static PSRAM scratch — safe: single httpd worker task serialises
    // handlers (see /metrics note above).
    EXT_RAM_BSS_ATTR static char buf[1024];
    return rest_api_reply(req, api_alerts_get, nullptr, 0, buf, sizeof(buf), "alerts");
}

// ── GET /api/logs — recent log lines captured from esp_log_set_vprintf ──
esp_err_t handle_get_logs(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    // 32 KB fits comfortably in PSRAM and covers ~128 × 192 bytes worth
    // of encoded log lines plus JSON overhead. Allocate rather than stack
    // to keep TaskHTTP's stack usage bounded.
    const size_t cap = 32 * 1024;
    char* buf = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!buf) buf = static_cast<char*>(malloc(cap));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "log alloc failed");
        return ESP_OK;
    }
    size_t n = 0;
    ApiStatus st = api_logs_get(nullptr, 0, buf, cap, &n);
    httpd_resp_set_type(req, "application/json");
    if (st != API_OK) {
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "logs");
        return ESP_OK;
    }
    httpd_resp_send(req, buf, (ssize_t)n);
    heap_caps_free(buf);
    return ESP_OK;
}

// ── GET /api/diagnostics/unhandled ───────────────────────────────────────
// Snapshot of the P4-side unhandled-frame ring. Each entry identifies a
// (cluster, attr_or_cmd, cluster_specific) tuple the ZCL pipeline
// couldn't decode. Used by the Diagnostics UI tab to prioritize which
// converter-body extraction patterns to teach the pipeline next.
esp_err_t handle_get_diagnostics_unhandled(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    // Static PSRAM scratch — safe: single httpd worker task serialises
    // handlers (see /metrics note above).
    EXT_RAM_BSS_ATTR static char buf[2048];
    return rest_api_reply(req, api_diagnostics_unhandled_get, nullptr, 0,
                           buf, sizeof(buf), "diag");
}

// ── POST /api/permit_join ─────────────────────────────────────────────────
esp_err_t handle_post_permit_join(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static int64_t s_last_us = 0;
    RATE_LIMIT(req, s_last_us, 5LL * 1000000LL);

    char body[64] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        return ESP_OK;
    }
    if (received > 0) body[received] = '\0';

    char rsp[64];
    size_t rsp_len = 0;
    const ApiStatus st = api_zigbee_permit_join(body, (size_t)received,
                                                  rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// ── POST /api/zigbee/reset — Zigbee factory reset ────────────────────────
esp_err_t handle_post_zigbee_reset(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_zigbee_reset(nullptr, 0, rsp, sizeof(rsp), &rsp_len);
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// ── POST /api/zigbee/settings — channel / network key persistence ───────
// Changes are persisted on P4 (nvs `zigbee_cfg`) and APPLY on the next
// factory reset — not live. The response includes the current stored
// values so the UI can refresh.
esp_err_t handle_post_zigbee_settings(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char body[128] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }

    char rsp[128];
    size_t rsp_len = 0;
    ApiStatus st = api_zigbee_settings_set(body, (size_t)received,
                                             rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "zigbee settings");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// ── POST /api/ota ─────────────────────────────────────────────────────────
esp_err_t handle_post_ota(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static int64_t s_last_us = 0;
    RATE_LIMIT(req, s_last_us, 60LL * 1000000LL);
    char body[300] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_ota_s3(body, (size_t)received, rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, rsp, (ssize_t)rsp_len);
}

// ── POST /api/p4-ota ──────────────────────────────────────────────────────
esp_err_t handle_post_p4_ota(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static int64_t s_last_us = 0;
    RATE_LIMIT(req, s_last_us, 60LL * 1000000LL);
    char body[300] = {};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[len] = '\0';

    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_ota_p4(body, (size_t)len, rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "p4-ota");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// ── POST /api/wifi ────────────────────────────────────────────────────────
esp_err_t handle_post_wifi(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static int64_t s_last_us = 0;
    RATE_LIMIT(req, s_last_us, 30LL * 1000000LL);
    char body[256] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    char rsp[64];
    size_t rsp_len = 0;
    ApiStatus st = api_wifi_connect(body, (size_t)received,
                                     rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi connect");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    // api_wifi_connect arms a 1 s deferred esp_restart.
    return ESP_OK;
}

// ── POST /api/settings ────────────────────────────────────────────────────
esp_err_t handle_post_settings(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char body[256] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }

    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_settings_set(body, (size_t)received,
                                     rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "settings");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// ── GET /api/wifi/status ──────────────────────────────────────────────────
esp_err_t handle_get_wifi_status(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    SET_CORS_HEADERS(req);
    char buf[200];
    return rest_api_reply(req, api_wifi_status, nullptr, 0, buf, sizeof(buf), "wifi status");
}

// ── GET /api/wifi/scan ───────────────────────────────────────────────────
esp_err_t handle_get_wifi_scan(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static int64_t s_last_scan_us = 0;
    RATE_LIMIT(req, s_last_scan_us, 10LL * 1000000LL);

    // Static PSRAM scratch — safe: single httpd worker task serialises
    // handlers (see /metrics note above).
    EXT_RAM_BSS_ATTR static char buf[2048];
    return rest_api_reply(req, api_wifi_scan, nullptr, 0, buf, sizeof(buf), "wifi scan");
}

// ── DELETE /api/wifi ─────────────────────────────────────────────────────
esp_err_t handle_delete_wifi(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_wifi_disconnect(nullptr, 0, rsp, sizeof(rsp), &rsp_len);
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi forget");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    // api_wifi_disconnect arms a 1 s deferred wifi_mgr_forget_and_reboot.
    return ESP_OK;
}

// ── SPIFFS static file server ─────────────────────────────────────────────

// Canonicalize a POSIX-style path in-place: resolve . and .. segments.
// Returns false if .. would escape the root (path traversal attempt).
static bool path_normalize(char* path, size_t buf_size) {
    char tmp[256];
    size_t plen = strlen(path);
    if (plen >= sizeof(tmp)) return false;
    memcpy(tmp, path, plen + 1);
    const char* segs[16];
    uint8_t depth = 0;
    char* s = tmp;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char* end = s;
        while (*end && *end != '/') end++;
        bool at_end = (*end == '\0');
        *end = '\0';
        if (strcmp(s, ".") == 0) {
            // skip
        } else if (strcmp(s, "..") == 0) {
            if (depth == 0) return false;
            depth--;
        } else {
            if (depth >= 16) return false;
            segs[depth++] = s;
        }
        s = at_end ? end : end + 1;
    }
    size_t pos = 0;
    for (uint8_t i = 0; i < depth && pos < buf_size - 1; i++) {
        path[pos++] = '/';
        size_t slen = strlen(segs[i]);
        if (pos + slen >= buf_size) return false;
        memcpy(path + pos, segs[i], slen);
        pos += slen;
    }
    if (pos == 0) { path[0] = '/'; path[1] = '\0'; }
    else path[pos] = '\0';
    return true;
}

static const char* mime_for_path(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    return "application/octet-stream";
}

static const char* cache_for_path(const char* path) {
    if (strstr(path, "/assets/")) {
        return "public, max-age=31536000, immutable";
    }
    return "no-cache";
}

esp_err_t handle_get_static(httpd_req_t* req) {
    // No REQUIRE_AUTH: WebUI static assets (HTML/CSS/JS) need to load
    // before the user has a token to send.
    SET_CORS_HEADERS(req);
    char path[256];
    const char* uri = req->uri;
    if (!uri || !uri[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty uri");
        return ESP_OK;
    }
    if (strcmp(uri, "/") == 0) uri = "/index.html";
    size_t uri_len = strlen(uri);
    // Reject oversized URIs early — captive portal probes can send very long URIs
    if (uri_len >= sizeof(path) - 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uri too long");
        return ESP_OK;
    }
    // Manual concat — avoids snprintf format-truncation warning (URI can be up to CONFIG_HTTPD_MAX_URI_LEN)
    memcpy(path, "/spiffs", 7);
    memcpy(path + 7, uri, uri_len + 1);
    if (!path_normalize(path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    if (strncmp(path, "/spiffs/", 8) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        // SPA fallback: unknown paths without a file extension (or
        // explicitly *.html) are browser navigations — serve index.html
        // so the client-side router can take over. Asset paths with a
        // different extension (.js, .css, .png, …) must 404 — falling
        // back to index.html would mislabel HTML as application/javascript
        // and the browser would fail to parse it.
        const char* ext = strrchr(path, '.');
        const char* slash = strrchr(path, '/');
        const bool ext_after_slash = ext && (!slash || ext > slash);
        const bool html_like = !ext_after_slash || strcmp(ext, ".html") == 0;
        if (!html_like) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
            return ESP_OK;
        }
        f = fopen("/spiffs/index.html", "r");
        if (!f) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
            return ESP_OK;
        }
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", cache_for_path(path));
    } else {
        httpd_resp_set_type(req, mime_for_path(path));
        httpd_resp_set_hdr(req, "Cache-Control", cache_for_path(path));
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ── OPTIONS /* — CORS preflight ──────────────────────────────────────────
// Wildcard handler for browser preflight (OPTIONS) requests.
// Returns 204 No Content with CORS headers so the browser can proceed.
static esp_err_t handle_options_cors(httpd_req_t* req) {
    SET_CORS_HEADERS(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ── task_http — HTTP server start + URI registration ─────────────────────
void task_http(void*) {
    auth_init();
    ESP_LOGI(TAG, "TaskHTTP started");

    httpd_handle_t server = ws_server_get_handle();
    if (!server) {
        ESP_LOGE(TAG, "TaskHTTP: no httpd handle — HTTP REST disabled");
        vTaskDelete(nullptr);
        return;
    }

    // S-F4 (docs/OPTIMIZATIONS.md): one table-driven registration loop
    // replaces 38 hand-rolled `static httpd_uri_t` literals + 38
    // `httpd_register_uri_handler` calls. Order is preserved because
    // ESP-IDF's `httpd_uri_match_wildcard` only matches a TRAILING `*`
    // (see `httpd_uri.c:24`), and method-based matching relies on POST
    // sub-route dispatchers being registered BEFORE same-prefix
    // GET/PUT/DELETE siblings. Adding a route here = one row.
    struct RestRoute {
        const char*     uri;
        httpd_method_t  method;
        esp_err_t     (*fn)(httpd_req_t*);
    };
    static const RestRoute kRoutes[] = {
        // — status / observability —
        { "/api/status",                  HTTP_GET,     handle_get_status                 },
        { "/api/alerts",                  HTTP_GET,     handle_get_alerts                 },
        { "/api/logs",                    HTTP_GET,     handle_get_logs                   },
        { "/api/diagnostics/unhandled",   HTTP_GET,     handle_get_diagnostics_unhandled  },
        { "/metrics",                     HTTP_GET,     handle_get_metrics                },
        // — system —
        { "/api/settings",                HTTP_POST,    handle_post_settings              },
        { "/api/ota",                     HTTP_POST,    handle_post_ota                   },
        { "/api/p4-ota",                  HTTP_POST,    handle_post_p4_ota                },
        // — wifi —
        { "/api/wifi",                    HTTP_POST,    handle_post_wifi                  },
        { "/api/wifi",                    HTTP_DELETE,  handle_delete_wifi                },
        { "/api/wifi/scan",               HTTP_GET,     handle_get_wifi_scan              },
        { "/api/wifi/status",             HTTP_GET,     handle_get_wifi_status            },
        // — zigbee —
        { "/api/permit_join",             HTTP_POST,    handle_post_permit_join           },
        { "/api/zigbee/reset",            HTTP_POST,    handle_post_zigbee_reset          },
        { "/api/zigbee/settings",         HTTP_POST,    handle_post_zigbee_settings       },
        // — rules —
        { "/api/rules",                   HTTP_GET,     handle_get_rules                  },
        { "/api/rules",                   HTTP_POST,    handle_post_rules                 },
        { "/api/rules",                   HTTP_DELETE,  handle_delete_rules               },
        { "/api/rules",                   HTTP_PUT,     handle_put_rules                  },
        { "/api/rules/*",                 HTTP_PUT,     handle_put_rule_dsl               },
        // — scripts —
        { "/api/scripts",                 HTTP_GET,     handle_get_scripts                },
        { "/api/scripts",                 HTTP_POST,    handle_post_scripts_bulk          },
        { "/api/scripts/*",               HTTP_GET,     handle_get_script_by_id           },
        { "/api/scripts/*",               HTTP_POST,    handle_post_script                },
        { "/api/scripts/*",               HTTP_DELETE,  handle_delete_script              },
        // — devices —
        { "/api/devices",                 HTTP_GET,     handle_get_devices                },
        // POST sub-route MUST come before GET/PUT/DELETE wildcard siblings
        // (method-based match resolves first). Inner dispatcher in
        // `rest_devices.cpp` inspects the URI suffix (`/bind`, `/unbind`,
        // `/interview`).
        { "/api/devices/*",               HTTP_POST,    handle_post_device_subroute       },
        { "/api/devices/*",               HTTP_GET,     handle_get_device_by_id           },
        { "/api/devices/*",               HTTP_PUT,     handle_put_device                 },
        { "/api/devices/*",               HTTP_DELETE,  handle_delete_device              },
        // — groups — same POST-sub-route-first rule as devices.
        { "/api/groups",                  HTTP_GET,     handle_get_groups                 },
        { "/api/groups",                  HTTP_POST,    handle_post_groups                },
        { "/api/groups/*/cmd",            HTTP_POST,    handle_post_group_cmd             },
        { "/api/groups/*",                HTTP_GET,     handle_get_group_by_id            },
        { "/api/groups/*",                HTTP_PUT,     handle_put_group                  },
        { "/api/groups/*",                HTTP_DELETE,  handle_delete_group               },
        // — CORS preflight (must be registered before the static catch-all) —
        { "/api/*",                       HTTP_OPTIONS, handle_options_cors               },
        // — SPIFFS catch-all — MUST be last (wildcard catches everything).
        { "/*",                           HTTP_GET,     handle_get_static                 },
    };
    static_assert(sizeof(kRoutes) / sizeof(kRoutes[0]) == 38,
                  "S-F4: REST route count drift — update api_routes.def too?");

    for (const auto& r : kRoutes) {
        httpd_uri_t h = {
            .uri                      = r.uri,
            .method                   = r.method,
            .handler                  = r.fn,
            .user_ctx                 = nullptr,
            .is_websocket             = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol    = nullptr,
        };
        if (httpd_register_uri_handler(server, &h) != ESP_OK)
            ESP_LOGE(TAG, "register failed: %s (method=%d)", r.uri, (int)r.method);
    }
    ESP_LOGI(TAG, "REST: %zu routes registered", sizeof(kRoutes) / sizeof(kRoutes[0]));

    vTaskDelete(nullptr);
}
