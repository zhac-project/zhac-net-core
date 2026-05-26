// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : uint8_t {
    REMOTE_STATE_DISABLED        = 0,
    REMOTE_STATE_IDLE_NO_WIFI    = 1,
    REMOTE_STATE_CONNECTING      = 2,
    REMOTE_STATE_AUTHENTICATING  = 3,
    REMOTE_STATE_READY           = 4,
    REMOTE_STATE_BACKOFF         = 5,
} RemoteState;

typedef enum : uint8_t {
    REMOTE_EV_ENABLE       = 0,
    REMOTE_EV_DISABLE      = 1,
    REMOTE_EV_WIFI_UP      = 2,
    REMOTE_EV_WIFI_DOWN    = 3,
    REMOTE_EV_WSS_CONNECT  = 4,
    REMOTE_EV_WSS_ERROR    = 5,
    REMOTE_EV_AUTH_OK      = 6,
    REMOTE_EV_AUTH_FAIL    = 7,
    REMOTE_EV_AUTH_TIMEOUT = 8,
    REMOTE_EV_BACKOFF_DONE = 9,
} RemoteEvent;

RemoteState remote_state_next(RemoteState cur, RemoteEvent ev);
const char* remote_state_name(RemoteState s);

#ifdef __cplusplus
}
#endif
