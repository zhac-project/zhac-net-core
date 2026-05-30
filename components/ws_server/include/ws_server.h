// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include "esp_http_server.h"

// Called on every text frame received from a client. `fd` identifies
// the sending socket so the handler can issue a point-reply via
// `ws_server_reply()`; broadcasts to every client use
// `ws_server_broadcast()` instead.
using WsRxCallback = void(*)(int fd, const char* data, size_t len);

void             ws_server_init();
void             ws_server_broadcast(const char* json, size_t len);
// Send `json` to a single client identified by its httpd socket fd.
// Intended for command responses correlated by `id` back to the sender.
void             ws_server_reply(int fd, const char* json, size_t len);
httpd_handle_t   ws_server_get_handle();
void             ws_server_set_rx_callback(WsRxCallback cb);
void             ws_server_set_api_token(const char* token);
int              ws_server_client_count();

// F18: per-fd auth state for first-message WS authentication. A socket may
// open unauthenticated when a token is configured; the dispatch layer permits
// only the `auth` command until ws_server_fd_set_authed() is called for it.
bool             ws_server_fd_is_authed(int fd);
void             ws_server_fd_set_authed(int fd);
// Q48: clear every client's authed flag (e.g. on token rotation) so sockets that
// authenticated with the now-stale token must re-auth with the new one.
void             ws_server_fd_deauth_all();

// ── Reply hook for sentinel fds ─────────────────────────────────────────
//
// Some callers (e.g. remote_client) deliver responses over a different
// transport than the local httpd. They register a hook against a
// reserved sentinel fd (outside the legal httpd range, e.g. < 0).
//
// When ws_server_reply() is called with a fd that matches a registered
// sentinel, the hook is invoked instead of httpd_ws_send_frame_async.
//
// Single-slot table; latest registration wins. Pass nullptr to unregister.
typedef void (*WsReplyHook)(const char* json, size_t len);
void ws_server_register_reply_hook(int sentinel_fd, WsReplyHook hook);
