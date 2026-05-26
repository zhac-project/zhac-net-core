// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Admin command handlers for the remote_client. Only compiled when
// CONFIG_ZHAC_REMOTE_CLIENT_ENABLE is on (added to main/CMakeLists.txt
// SRCS list via `if(CONFIG_...)`). NEVER allow-listed for the remote
// channel itself (see remote_allow.cpp permanent excludes comment).

#include "sdkconfig.h"

#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE

#include "api_handlers.h"
#include "remote_client.h"
#include "remote_nvs.h"
#include "remote_state.h"
#include "ArduinoJson.h"
#include <cstdio>
#include <cstring>

extern "C" ApiStatus api_remote_status(const char* /*body*/, size_t /*body_len*/,
                                        char* rsp_buf, size_t rsp_cap,
                                        size_t* rsp_len) {
    RemoteStatusSnap snap{};
    remote_client_get_status(&snap);

    int n = snprintf(rsp_buf, rsp_cap,
        "{\"enabled\":%s,\"state\":\"%s\",\"connected_since\":%u,"
        "\"last_event_at\":%u,\"rtt_ms\":%u,"
        "\"tx_drops\":%u,\"auth_fails\":%u}",
        snap.enabled ? "true" : "false",
        remote_state_name((RemoteState)snap.state),
        (unsigned)snap.connected_since,
        (unsigned)snap.last_event_at,
        (unsigned)snap.rtt_ms,
        (unsigned)snap.tx_drops,
        (unsigned)snap.auth_fails);

    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_remote_connect(const char* body, size_t body_len,
                                         char* rsp_buf, size_t rsp_cap,
                                         size_t* rsp_len) {
    JsonDocument doc;
    if (deserializeJson(doc, body, body_len)) return API_BAD_REQUEST;

    const char* url   = doc["url"]       | "";
    const char* token = doc["token"]     | "";
    const char* devid = doc["device_id"] | "";

    if (!url[0] || !token[0]) return API_BAD_REQUEST;
    if (strlen(url) >= REMOTE_NVS_URL_MAX) return API_BAD_REQUEST;
    if (strlen(token) >= REMOTE_NVS_TOKEN_MAX) return API_BAD_REQUEST;

    if (!remote_nvs_save(true, url, token, devid)) return API_INTERNAL_ERROR;
    remote_client_enable();

    int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":true}");
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    *rsp_len = (size_t)n;
    return API_OK;
}

extern "C" ApiStatus api_remote_disconnect(const char* body, size_t body_len,
                                            char* rsp_buf, size_t rsp_cap,
                                            size_t* rsp_len) {
    bool forget = false;
    if (body && body_len > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, body, body_len)) {
            forget = doc["forget"] | false;
        }
    }
    remote_client_disable(forget);

    int n = snprintf(rsp_buf, rsp_cap, "{\"ok\":true,\"forget\":%s}",
                     forget ? "true" : "false");
    if (n < 0 || (size_t)n >= rsp_cap) return API_INTERNAL_ERROR;
    *rsp_len = (size_t)n;
    return API_OK;
}

#endif  // CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
