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
