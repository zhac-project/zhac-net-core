// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_state.h"

extern "C" RemoteState remote_state_next(RemoteState cur, RemoteEvent ev) {
    // EV_DISABLE always wins.
    if (ev == REMOTE_EV_DISABLE) return REMOTE_STATE_DISABLED;
    // EV_WIFI_DOWN drops everything except DISABLED back to IDLE_NO_WIFI.
    if (ev == REMOTE_EV_WIFI_DOWN) {
        return (cur == REMOTE_STATE_DISABLED) ? REMOTE_STATE_DISABLED
                                              : REMOTE_STATE_IDLE_NO_WIFI;
    }
    switch (cur) {
        case REMOTE_STATE_DISABLED:
            if (ev == REMOTE_EV_ENABLE) return REMOTE_STATE_IDLE_NO_WIFI;
            return REMOTE_STATE_DISABLED;
        case REMOTE_STATE_IDLE_NO_WIFI:
            if (ev == REMOTE_EV_WIFI_UP) return REMOTE_STATE_CONNECTING;
            return REMOTE_STATE_IDLE_NO_WIFI;
        case REMOTE_STATE_CONNECTING:
            if (ev == REMOTE_EV_WSS_CONNECT) return REMOTE_STATE_AUTHENTICATING;
            if (ev == REMOTE_EV_WSS_ERROR)   return REMOTE_STATE_BACKOFF;
            return REMOTE_STATE_CONNECTING;
        case REMOTE_STATE_AUTHENTICATING:
            if (ev == REMOTE_EV_AUTH_OK)       return REMOTE_STATE_READY;
            if (ev == REMOTE_EV_AUTH_FAIL)     return REMOTE_STATE_BACKOFF;
            if (ev == REMOTE_EV_AUTH_TIMEOUT)  return REMOTE_STATE_BACKOFF;
            if (ev == REMOTE_EV_WSS_ERROR)     return REMOTE_STATE_BACKOFF;
            return REMOTE_STATE_AUTHENTICATING;
        case REMOTE_STATE_READY:
            if (ev == REMOTE_EV_WSS_ERROR) return REMOTE_STATE_BACKOFF;
            // EV_AUTH_OK on READY is the benign re-auth ack: stay ready.
            return REMOTE_STATE_READY;
        case REMOTE_STATE_BACKOFF:
            if (ev == REMOTE_EV_BACKOFF_DONE) return REMOTE_STATE_CONNECTING;
            return REMOTE_STATE_BACKOFF;
    }
    return cur;
}

extern "C" const char* remote_state_name(RemoteState s) {
    switch (s) {
        case REMOTE_STATE_DISABLED:       return "DISABLED";
        case REMOTE_STATE_IDLE_NO_WIFI:   return "IDLE_NO_WIFI";
        case REMOTE_STATE_CONNECTING:     return "CONNECTING";
        case REMOTE_STATE_AUTHENTICATING: return "AUTHENTICATING";
        case REMOTE_STATE_READY:          return "READY";
        case REMOTE_STATE_BACKOFF:        return "BACKOFF";
    }
    return "?";
}
