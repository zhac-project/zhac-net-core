// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ws_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

// DS10 (DS_FINDINGS): cap on an inbound WS text frame. Covers rule DSL +
// script.check Lua source; frames beyond this are dropped (a misbehaving/hostile
// client can't force a huge per-frame allocation).
#define WS_RX_MAX (8 * 1024)

static const char*       TAG      = "ws_server";
static httpd_handle_t    s_server = nullptr;
static SemaphoreHandle_t s_mutex  = nullptr;
static WsRxCallback      s_rx_cb  = nullptr;
static char              s_api_token[33] = {};

#define MAX_WS_CLIENTS 3
static int  s_fds[MAX_WS_CLIENTS];
static bool s_fd_authed[MAX_WS_CLIENTS] = {};   // F18: per-fd WS auth state
static int  s_fd_count = 0;

// Single-entry hook table. Suitable while there's only one
// non-httpd transport (remote_client). Easy to extend to a small
// array if a second transport needs the same plumbing.
static int          s_hook_fd   = 0;       // 0 = unused (legal httpd fd value)
static WsReplyHook  s_hook_func = nullptr;

// Returns false when the client table is full (caller closes the socket —
// a slot-less socket would stay open, accept commands, yet never receive a
// single broadcast: a half-functional zombie).
static bool add_fd(int fd, bool authed) {
    bool added = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { s_fd_authed[i] = authed; xSemaphoreGive(s_mutex); return true; }
    }
    if (s_fd_count < MAX_WS_CLIENTS) {
        s_fds[s_fd_count]       = fd;
        s_fd_authed[s_fd_count] = authed;
        s_fd_count++;
        added = true;
        ESP_LOGI(TAG, "WS client added fd=%d authed=%d total=%d", fd, authed, s_fd_count);
    }
    xSemaphoreGive(s_mutex);
    return added;
}

static void remove_fd(int fd) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) {
            --s_fd_count;
            s_fds[i]       = s_fds[s_fd_count];
            s_fd_authed[i] = s_fd_authed[s_fd_count];   // F18: keep auth state aligned
            ESP_LOGI(TAG, "WS client removed fd=%d total=%d", fd, s_fd_count);
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

// F18: per-fd auth state for first-message WS authentication.
bool ws_server_fd_is_authed(int fd) {
    bool authed = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { authed = s_fd_authed[i]; break; }
    }
    xSemaphoreGive(s_mutex);
    return authed;
}
void ws_server_fd_set_authed(int fd) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (s_fds[i] == fd) { s_fd_authed[i] = true; break; }
    }
    xSemaphoreGive(s_mutex);
}
// Q48 (QWEN_FINDINGS triage): drop all per-fd auth so a token rotation actually
// invalidates live sessions — old-token sockets must re-auth with the new token.
void ws_server_fd_deauth_all() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) s_fd_authed[i] = false;
    xSemaphoreGive(s_mutex);
}

static esp_err_t ws_handler(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // F18 (FINDINGS.md): the socket may OPEN unauthenticated; auth is
        // enforced per-message in the dispatch layer. If the client presents a
        // valid token at handshake (X-Api-Key header, or the legacy ?token=
        // query) we mark it authed now — back-compat for non-browser clients,
        // which set headers without any URL leak. Browsers connect WITHOUT a
        // URL token and authenticate via a first {"cmd":"auth"} message,
        // keeping the long-lived token out of proxy/access logs.
        bool authed = (s_api_token[0] == '\0');   // no token configured → auth off
        if (!authed) {
            char key[64] = {};
            bool got_key = false;
            if (httpd_req_get_hdr_value_str(req, "X-Api-Key", key,
                                             sizeof(key)) == ESP_OK) {
                got_key = true;
            } else {
                size_t qlen = httpd_req_get_url_query_len(req);
                if (qlen > 0 && qlen < 256) {
                    char qbuf[256];
                    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
                        httpd_query_key_value(qbuf, "token", key, sizeof(key)) == ESP_OK) {
                        got_key = true;
                    }
                }
            }
            if (got_key && strlen(key) == 32) {
                // Constant-time compare: XOR-accumulate all 32 bytes.
                uint8_t diff = 0;
                for (int i = 0; i < 32; i++)
                    diff |= (uint8_t)key[i] ^ (uint8_t)s_api_token[i];
                authed = (diff == 0);
            }
        }
        if (!add_fd(fd, authed)) {
            // Table full: close the socket instead of leaving it half-alive
            // (it could send commands but would never see a broadcast).
            ESP_LOGW(TAG, "WS client limit (%d) reached, closing fd=%d",
                     MAX_WS_CLIENTS, fd);
            httpd_sess_trigger_close(s_server, fd);
        }
        return ESP_OK;
    }
    // DS10 (DS_FINDINGS): the old fixed 256-byte stack buffer truncated/rejected
    // any WS frame larger than 255 bytes (rule DSL, script.check Lua source) and
    // a 2 KB+ stack buffer would risk the httpd task's canary on its deep call
    // chains. Probe the length (max_len=0, no copy), then read the whole frame
    // into a right-sized PSRAM buffer.
    httpd_ws_frame_t pkt{};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) { remove_fd(fd); return ret; }
    // Control frames (PING/PONG/CLOSE) never reach this handler:
    // handle_ws_control_frames is false, so httpd answers PING with PONG and
    // replies to CLOSE itself (RFC 6455 5.5.1/5.5.2), then tears the session
    // down — our close_fn does the table cleanup.
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0 || !s_rx_cb) return ESP_OK;
    if (pkt.len > WS_RX_MAX) {
        ESP_LOGW(TAG, "ws frame %u B exceeds cap %u — dropped",
                 (unsigned)pkt.len, (unsigned)WS_RX_MAX);
        return ESP_OK;
    }
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(pkt.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) buf = static_cast<uint8_t*>(malloc(pkt.len + 1));
    if (!buf) return ESP_ERR_NO_MEM;
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK) {
        buf[pkt.len] = '\0';
        s_rx_cb(fd, reinterpret_cast<const char*>(buf), pkt.len);
    }
    free(buf);
    return ret;
}

static const httpd_uri_t s_ws_uri = {
    .uri                      = "/ws",
    .method                   = HTTP_GET,
    .handler                  = ws_handler,
    .user_ctx                 = nullptr,
    .is_websocket             = true,
    // false → httpd auto-replies PONG to PING and CLOSE to CLOSE (the old
    // `true` setting claimed control-frame handling but swallowed PING
    // without ever sending a PONG, so keepalive-probing clients timed out).
    // Session teardown after CLOSE runs through close_fn below.
    .handle_ws_control_frames = false,
    .supported_subprotocol    = nullptr,
};

// Registered as httpd_config_t::close_fn — fires on EVERY terminated session
// (plain HTTP and WS alike; graceful CLOSE, TCP RST, keepalive death, LRU
// purge). Without it an abnormal disconnect never reached remove_fd and the
// 3-slot client table leaked until a later broadcast send happened to fail.
// remove_fd() is a no-op for fds that never joined the WS table, so plain
// HTTP sockets pass straight through. With a custom close_fn installed the
// server no longer close()s the socket itself — we must do it here.
static void ws_httpd_close_fn(httpd_handle_t hd, int sockfd) {
    (void)hd;
    remove_fd(sockfd);
    close(sockfd);
}

void ws_server_init() {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.server_port         = 80;
    // Cap is CONFIG_LWIP_MAX_SOCKETS (10). httpd keeps one listen socket
    // aside, so we can safely request up to 9. Use the whole pool — the
    // SPA's editor chunk drags in several lazy imports; parallel fetches
    // from mobile browsers (up to 6) plus the persistent /ws connection
    // easily outruns a smaller pool.
    cfg.max_open_sockets    = 9;
    cfg.max_uri_handlers    = 48;
    cfg.uri_match_fn        = httpd_uri_match_wildcard;
    cfg.stack_size          = 12288; // 8 K was tight: api_rule_list -> hap_roundtrip ->
                                     // SPI exchange -> peer-dispatch BULK callback ->
                                     // ws_event_broadcast -> ws_server_broadcast ->
                                     // lwip_send chain trips the canary on a single
                                     // WS request. 12 K covers the worst observed
                                     // path (httpd thread tracing through master_send
                                     // and back into ws send) plus headroom for
                                     // future handler additions.
    // Mobile browsers (Safari/Chrome on iOS/Android) open up to 6 parallel
    // TCP connections to the same host for speculative fetches + asset
    // prefetch. LRU eviction re-purposes the oldest idle connection for
    // new accepts so bursts clear cleanly even if the pool fills.
    cfg.lru_purge_enable    = true;
    // TCP keepalive — without this, a half-open connection (mobile
    // browser tabbed away on a flaky link, NAT idle-timeout dropping
    // the path) is only detected when the next ws_broadcast fails
    // with ECONNRESET / ENOTCONN. Sending probes after 10 s of idle
    // catches the death within ~30 s and frees the socket pool slot.
    cfg.keep_alive_enable   = true;
    cfg.keep_alive_idle     = 10;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count    = 3;
    // F33 (FINDINGS.md): bound slow / half-open clients (slowloris) so a few
    // trickle connections can't pin the small (9) socket pool indefinitely.
    cfg.recv_wait_timeout   = 10;
    cfg.send_wait_timeout   = 10;
    // Slot cleanup on ANY socket close (see ws_httpd_close_fn above).
    cfg.close_fn            = ws_httpd_close_fn;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    httpd_register_uri_handler(s_server, &s_ws_uri);
    ESP_LOGI(TAG, "ws_server started on :80/ws");
}

httpd_handle_t ws_server_get_handle() {
    return s_server;
}

void ws_server_set_rx_callback(WsRxCallback cb) {
    s_rx_cb = cb;
}

void ws_server_set_api_token(const char* token) {
    strncpy(s_api_token, token ? token : "", sizeof(s_api_token) - 1);
    s_api_token[sizeof(s_api_token) - 1] = '\0';
}

void ws_server_register_reply_hook(int sentinel_fd, WsReplyHook hook) {
    // No mutex needed: registration happens once at init, before
    // any traffic reaches ws_server_reply.
    s_hook_fd   = sentinel_fd;
    s_hook_func = hook;
}

int ws_server_client_count() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_fd_count;
    xSemaphoreGive(s_mutex);
    return n;
}

// Task currently inside ws_server_broadcast()'s send loop, nullptr when none.
// The log pipeline checks this (ws_server_in_broadcast) and drops its WS-sink
// fan-out for lines logged from within the broadcast path — defence-in-depth
// against TX-path re-entry (e.g. esp_http_server's own ESP_LOG* inside
// httpd_ws_send_frame_async). Single writer: only the ws_bridge TX worker
// calls ws_server_broadcast.
static std::atomic<TaskHandle_t> s_broadcast_task{nullptr};

bool ws_server_in_broadcast() {
    return s_broadcast_task.load(std::memory_order_acquire) ==
           xTaskGetCurrentTaskHandle();
}

void ws_server_broadcast(const char* json, size_t len) {
    if (!s_server || !s_mutex) return;

    // Snapshot fd list outside the send loop so we don't hold the mutex
    // while calling potentially blocking httpd API.
    //
    // Auth gate: when a token is configured (token set ⇔ auth enabled — main
    // clears it via ws_server_set_api_token(nullptr) on disable), sockets that
    // opened /ws but never authenticated must NOT passively receive events
    // (device states, alerts, log lines). The auth handshake reply itself is a
    // targeted ws_server_reply(), never a broadcast, so gating here cannot
    // break the {"cmd":"auth"} exchange.
    int  fds[MAX_WS_CLIENTS];
    int  count = 0;
    const bool auth_on = (s_api_token[0] != '\0');
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_fd_count; i++) {
        if (auth_on && !s_fd_authed[i]) continue;
        fds[count++] = s_fds[i];
    }
    xSemaphoreGive(s_mutex);

    httpd_ws_frame_t pkt{};
    pkt.type    = HTTPD_WS_TYPE_TEXT;
    pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    pkt.len     = len;

    // NO ESP_LOG inside the send loop — with the WS log sink enabled a log
    // line re-enters the WS TX path, and logging a failure against a dead fd
    // used to recurse (log → sink → broadcast → fail → log → …) until stack
    // overflow. Collect failures, remove the dead fds FIRST, then log.
    int       failed_fd[MAX_WS_CLIENTS];
    esp_err_t failed_err[MAX_WS_CLIENTS];
    int       n_failed = 0;

    configASSERT(s_broadcast_task.load(std::memory_order_acquire) == nullptr); // single-broadcaster invariant (TX worker only)
    s_broadcast_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // NOTE: no early returns between the guard stores — in_broadcast must clear.
    for (int i = 0; i < count; i++) {
        esp_err_t ret = httpd_ws_send_frame_async(s_server, fds[i], &pkt);
        if (ret != ESP_OK) {
            failed_fd[n_failed]  = fds[i];
            failed_err[n_failed] = ret;
            n_failed++;
        }
    }
    s_broadcast_task.store(nullptr, std::memory_order_release);

    for (int i = 0; i < n_failed; i++) {
        remove_fd(failed_fd[i]);   // cleanup first…
        ESP_LOGW(TAG, "ws_broadcast: send failed fd=%d err=%d (removed)",
                 failed_fd[i], failed_err[i]);   // …log after — bounded: the
        // fd is already gone, so a re-broadcast of this line can't re-fail.
    }
}

void ws_server_reply(int fd, const char* json, size_t len) {
    // Sentinel-fd fast path: route the reply to a registered hook
    // (e.g. remote_client) instead of the local httpd send. The
    // sentinel is chosen outside the legal httpd fd range.
    if (s_hook_func && fd == s_hook_fd) {
        s_hook_func(json, len);
        return;
    }
    if (!s_server || fd < 0) return;
    httpd_ws_frame_t pkt{};
    pkt.type    = HTTPD_WS_TYPE_TEXT;
    pkt.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    pkt.len     = len;
    esp_err_t ret = httpd_ws_send_frame_async(s_server, fd, &pkt);
    if (ret != ESP_OK) {
        // Same ordering rule as the broadcast loop: drop the fd before
        // touching the log pipeline.
        remove_fd(fd);
        ESP_LOGW(TAG, "ws_reply: send failed fd=%d err=%d (removed)", fd, ret);
    }
}
