// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Shared event-envelope builder ────────────────────────────────────────
// ONE place that formats the `{"event":"<name>","data":<payload>}` frame
// broadcast to local WS clients (ws_bridge `ws_event_broadcast`) AND mirrored
// to the cloud peer (remote_client `publish_event`). Previously the two sites
// open-coded the same snprintf with DIVERGENT buffer caps (4 KB local vs 8 KB
// cloud) — the local-vs-cloud truncation asymmetry FINDINGS §5 flagged: a
// coalesced batch could fit the cloud frame yet be replaced by `data:null`
// locally (or vice-versa). Folding the format here keeps the wire shape and
// the overflow contract identical; each caller still passes its own `cap`
// (sized for its transport), but the framing can no longer drift.
//
// `name` is an internal, fixed event identifier (e.g. "attr.bulk") — NOT
// client-controlled — so it is spliced verbatim (no escape). `payload_json`
// is already well-formed JSON produced by an api_* handler and is spliced as
// a value. On overflow / bad args the frame degrades to a parseable
// `{"event":"<name>","data":null,"trunc":true}` rather than emitting broken
// JSON. Returns bytes written (excluding NUL), or 0 if even the degraded
// frame does not fit `cap`.
//
// `static inline` (header-only, no emitted symbol, available in BOTH Kconfig
// states) so the no-op-stub build still sees one definition and there is no
// ODR / cross-component-link concern.
#include <cstdio>
static inline size_t ws_event_envelope_build(char* out, size_t cap,
                                             const char* name,
                                             const char* payload_json,
                                             size_t payload_len) {
    if (!out || cap == 0) return 0;
    int n;
    if (payload_json && payload_len > 0) {
        n = snprintf(out, cap, "{\"event\":\"%s\",\"data\":%.*s}",
                     name ? name : "", (int)payload_len, payload_json);
    } else {
        n = snprintf(out, cap, "{\"event\":\"%s\",\"data\":null}",
                     name ? name : "");
    }
    if (n > 0 && (size_t)n < cap) return (size_t)n;
    // Degrade to a still-parseable frame so a peer never receives a truncated
    // object. If even this does not fit, report failure (0) to the caller.
    n = snprintf(out, cap, "{\"event\":\"%s\",\"data\":null,\"trunc\":true}",
                 name ? name : "");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Sentinel fd used as `int fd` in `ws_server_reply`-style calls when
// the response is to be routed to the remote socket instead of a
// local httpd fd. Chosen well outside the legal fd range
// (httpd fds are >= 0; any negative value works as a sentinel).
// -32768 is the smallest 16-bit signed value, comfortably outside
// any plausible socket-fd range while staying within `int` on all
// platforms IDF targets.
//
// The ws_server reply hook (Task 7) dispatches this sentinel to
// `remote_client_send_reply` instead of `httpd_ws_send_frame_async`.
#define REMOTE_VIRTUAL_FD ((int)(-32768))

// Read-only status snapshot used by api_remote_status (Task 9).
// Declared in BOTH Kconfig states so callers can hold the struct
// type unconditionally; the no-op stub below zeroes it when off.
typedef struct {
    bool     enabled;          // NVS enabled flag
    uint8_t  state;            // RemoteState enum value (see remote_state.h)
    uint32_t connected_since;  // unix seconds, 0 if not connected
    uint32_t last_event_at;    // unix seconds of last successful tx
    uint16_t rtt_ms;           // last app-level ping RTT, 0 if unknown
    uint32_t tx_drops;         // counter
    uint32_t auth_fails;       // counter
} RemoteStatusSnap;

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE

// Spawn task_remote + register the ws_server reply hook for the
// REMOTE_VIRTUAL_FD sentinel. Reads NVS to decide whether to actually
// connect at boot. Safe to call exactly once from app_main.
void remote_client_init(void);

// Request a transition into the active state machine. Idempotent.
// Triggered from api_remote_connect after NVS is updated.
void remote_client_enable(void);

// Request a transition into DISABLED. Idempotent. Drains and frees
// the TX queue; calls esp_websocket_client_destroy.
// If `forget_creds` is true, also erases url/token from NVS.
void remote_client_disable(bool forget_creds);

// Atomic-load test, used by the ws_event_broadcast hot-path to decide
// whether to bother with the publish call. <2 cycles when false.
bool remote_client_is_running(void);

// Hot path. Called from ws_event_broadcast AFTER ws_server_broadcast.
// Behaviour:
//   - if !is_running()         : return immediately.
//   - if !remote_event_allowed : return immediately.
//   - if state != READY        : return immediately (event dropped — the
//                                next reconnect bootstrap supersedes).
//   - else                     : enqueue a PSRAM-allocated copy of the
//                                full envelope onto s_tx_queue; the
//                                task_remote loop drains and sends.
//                                Overflow -> drop oldest + free + log.
void remote_client_publish_event(const char* name,
                                 const char* payload_json,
                                 size_t payload_len);

// Reply hook target registered with ws_server (Task 7).
// Forwards the formatted JSON envelope onto the active outbound socket.
// No-op if the socket is not in READY state.
void remote_client_send_reply(const char* json, size_t len);

void remote_client_get_status(RemoteStatusSnap* out);

#else  // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE not set

// Inline no-op stubs. The compiler eliminates these at -O2; no
// symbols are emitted. Callers can use the same names unconditionally.

static inline void remote_client_init(void) {}
static inline void remote_client_enable(void) {}
static inline void remote_client_disable(bool /*forget_creds*/) { (void)0; }
static inline bool remote_client_is_running(void) { return false; }
static inline void remote_client_publish_event(const char* /*name*/,
                                               const char* /*payload_json*/,
                                               size_t /*payload_len*/) {}
static inline void remote_client_send_reply(const char* /*json*/, size_t /*len*/) {}
static inline void remote_client_get_status(RemoteStatusSnap* out) {
    if (out) *out = (RemoteStatusSnap){};
}

#endif // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE

#ifdef __cplusplus
}
#endif
