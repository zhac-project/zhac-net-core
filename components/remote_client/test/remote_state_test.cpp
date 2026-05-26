// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_state.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static void check(RemoteState cur, RemoteEvent ev, RemoteState expected) {
    RemoteState got = remote_state_next(cur, ev);
    if (got != expected) {
        printf("FAIL: state %d + ev %d -> %d (expected %d)\n",
               cur, ev, got, expected);
        std::abort();
    }
}

int main() {
    // From DISABLED
    check(REMOTE_STATE_DISABLED,        REMOTE_EV_ENABLE,
          REMOTE_STATE_IDLE_NO_WIFI);
    check(REMOTE_STATE_DISABLED,        REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);
    check(REMOTE_STATE_DISABLED,        REMOTE_EV_WIFI_UP,
          REMOTE_STATE_DISABLED);

    // From IDLE_NO_WIFI
    check(REMOTE_STATE_IDLE_NO_WIFI,    REMOTE_EV_WIFI_UP,
          REMOTE_STATE_CONNECTING);
    check(REMOTE_STATE_IDLE_NO_WIFI,    REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);
    check(REMOTE_STATE_IDLE_NO_WIFI,    REMOTE_EV_WIFI_DOWN,
          REMOTE_STATE_IDLE_NO_WIFI);

    // From CONNECTING
    check(REMOTE_STATE_CONNECTING,      REMOTE_EV_WSS_CONNECT,
          REMOTE_STATE_AUTHENTICATING);
    check(REMOTE_STATE_CONNECTING,      REMOTE_EV_WSS_ERROR,
          REMOTE_STATE_BACKOFF);
    check(REMOTE_STATE_CONNECTING,      REMOTE_EV_WIFI_DOWN,
          REMOTE_STATE_IDLE_NO_WIFI);
    check(REMOTE_STATE_CONNECTING,      REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);

    // From AUTHENTICATING
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_AUTH_OK,
          REMOTE_STATE_READY);
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_AUTH_FAIL,
          REMOTE_STATE_BACKOFF);
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_AUTH_TIMEOUT,
          REMOTE_STATE_BACKOFF);
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_WSS_ERROR,
          REMOTE_STATE_BACKOFF);
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_WIFI_DOWN,
          REMOTE_STATE_IDLE_NO_WIFI);
    check(REMOTE_STATE_AUTHENTICATING,  REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);

    // From READY
    check(REMOTE_STATE_READY,           REMOTE_EV_WSS_ERROR,
          REMOTE_STATE_BACKOFF);
    check(REMOTE_STATE_READY,           REMOTE_EV_WIFI_DOWN,
          REMOTE_STATE_IDLE_NO_WIFI);
    check(REMOTE_STATE_READY,           REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);
    check(REMOTE_STATE_READY,           REMOTE_EV_AUTH_OK,
          REMOTE_STATE_READY); // benign re-auth ok

    // From BACKOFF
    check(REMOTE_STATE_BACKOFF,         REMOTE_EV_BACKOFF_DONE,
          REMOTE_STATE_CONNECTING);
    check(REMOTE_STATE_BACKOFF,         REMOTE_EV_WIFI_DOWN,
          REMOTE_STATE_IDLE_NO_WIFI);
    check(REMOTE_STATE_BACKOFF,         REMOTE_EV_DISABLE,
          REMOTE_STATE_DISABLED);

    // Stringify
    assert(std::strcmp(remote_state_name(REMOTE_STATE_DISABLED), "DISABLED") == 0);
    assert(std::strcmp(remote_state_name(REMOTE_STATE_READY),    "READY")    == 0);

    printf("remote_state_tests: ok\n");
    return 0;
}
