// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "s3_internal.h"
#include "api_handlers.h"
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "ArduinoJson.h"

// Extract the IEEE from /api/devices/{ieee}[/...] paths. Returns the
// offset/length span so the caller can build an args JSON object.
// `sub_out` is filled with the trailing segment after the ieee, or empty.
static bool uri_parse_device(httpd_req_t* req,
                              char* ieee_out, size_t ieee_cap,
                              char* sub_out,  size_t sub_cap) {
    if (!req || !req->uri[0]) return false;
    const char* uri = req->uri;
    // Find the segment after "/api/devices/". Use the last two slashes.
    const char* last = strrchr(uri, '/');
    if (!last) return false;
    // Determine whether there is a sub-path (last two slashes differ and
    // this segment is a known keyword).
    const char* tail = last + 1;
    bool has_sub = false;
    if (strcmp(tail, "options") == 0 || strcmp(tail, "attrs") == 0 ||
        strcmp(tail, "bind")    == 0 || strcmp(tail, "unbind") == 0 ||
        strcmp(tail, "interview") == 0) {
        has_sub = true;
    }

    const char* ieee_start;
    const char* ieee_end;
    if (has_sub) {
        // ieee is the preceding segment.
        const char* prev = last - 1;
        while (prev > uri && *prev != '/') prev--;
        ieee_start = prev + 1;
        ieee_end   = last;
        if (sub_out && sub_cap) {
            strncpy(sub_out, tail, sub_cap - 1);
            sub_out[sub_cap - 1] = '\0';
        }
    } else {
        ieee_start = last + 1;
        ieee_end   = uri + strlen(uri);
        // Strip query string.
        const char* q = strchr(ieee_start, '?');
        if (q) ieee_end = q;
        if (sub_out && sub_cap) sub_out[0] = '\0';
    }

    size_t n = (size_t)(ieee_end - ieee_start);
    if (n == 0 || n >= ieee_cap) return false;
    memcpy(ieee_out, ieee_start, n);
    ieee_out[n] = '\0';
    return true;
}

// Build a JSON args body that merges URI-derived fields (ieee, sub, etc.)
// with the optional request body. When the body is empty we emit a minimal
// object; when the body is non-empty JSON, we inject the extra keys by
// reconstructing through ArduinoJson. Returns true on success.
static bool build_args_json(const char* body, int body_len,
                             const char* ieee_str, const char* sub,
                             bool unbind, bool hard,
                             char* out, size_t out_cap) {
    JsonDocument doc;
    if (body && body_len > 0) {
        DeserializationError err = deserializeJson(doc, body, (size_t)body_len);
        if (err) return false;
    }
    if (ieee_str && ieee_str[0]) doc["ieee"] = ieee_str;
    if (sub && sub[0]) doc["sub"] = sub;
    if (unbind) doc["unbind"] = true;
    if (hard)   doc["hard"]   = true;
    size_t n = serializeJson(doc, out, out_cap);
    if (n == 0 || n >= out_cap) return false;
    out[n] = '\0';
    return true;
}

// GET /api/devices — fetch all devices from P4
esp_err_t handle_get_devices(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    // F-01 v2: hap_roundtrip_v2 admits concurrent callers up to its
    // own waiter-slot cap; the old REST-level mutex probe is gone.
    // HOTFIX: device.list is now paged + reassembled into one full
    // {"devices":[...]}; a large fleet overflows the old 16 KB buffer, so
    // size it like the WS path (~40 KB ≈ 125 devices). Past that the
    // accumulator truncates with a logged warning rather than mid-JSON.
    char* buf = static_cast<char*>(rest_big_alloc(40 * 1024));   // F33: PSRAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    ApiStatus st = api_device_list(nullptr, 0, buf, 40 * 1024, &n);
    if (st != API_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return ESP_OK;
}

// GET /api/devices/{ieee}[/options]
esp_err_t handle_get_device_by_id(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char ieee_str[24] = {};
    char sub[16]      = {};
    if (!uri_parse_device(req, ieee_str, sizeof(ieee_str), sub, sizeof(sub))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_OK;
    }
    char args[128];
    if (!build_args_json(nullptr, 0, ieee_str, sub, false, false,
                           args, sizeof(args))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
        return ESP_OK;
    }

    char* buf = static_cast<char*>(rest_big_alloc(8 * 1024));   // F33: PSRAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    ApiStatus st = api_device_get(args, strlen(args), buf, 8 * 1024, &n);
    if (st == API_BAD_REQUEST) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return ESP_OK;
}

// POST /api/devices/{ieee}/bind or /unbind
esp_err_t handle_post_device_bind(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char ieee_str[24] = {};
    char sub[16]      = {};
    if (!uri_parse_device(req, ieee_str, sizeof(ieee_str), sub, sizeof(sub))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
        return ESP_OK;
    }
    bool is_unbind = (strcmp(sub, "unbind") == 0);

    char body[256];
    int received = rest_body_recv(req, body, sizeof(body));   // F33: 413 on oversize body
    if (received < 0) return ESP_OK;                           // helper already replied
    body[received] = '\0';

    char args[384];
    if (!build_args_json(body, received, ieee_str, nullptr, is_unbind, false,
                           args, sizeof(args))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    char rsp[64];
    size_t rsp_len = 0;
    ApiStatus st = api_device_bind(args, strlen(args), rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bind");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// DELETE /api/devices/{ieee}[?hard=true]
esp_err_t handle_delete_device(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char ieee_str[24] = {};
    char sub[16]      = {};
    if (!uri_parse_device(req, ieee_str, sizeof(ieee_str), sub, sizeof(sub))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_OK;
    }

    bool hard = false;
    {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 128) {
            char qbuf[128];
            if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
                char valbuf[8];
                if (httpd_query_key_value(qbuf, "hard", valbuf, sizeof(valbuf)) == ESP_OK) {
                    hard = (strcmp(valbuf, "true") == 0 || strcmp(valbuf, "1") == 0);
                }
            }
        }
    }

    char args[96];
    if (!build_args_json(nullptr, 0, ieee_str, nullptr, false, hard,
                           args, sizeof(args))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
        return ESP_OK;
    }

    char rsp[32];
    size_t rsp_len = 0;
    ApiStatus st = api_device_delete(args, strlen(args),
                                       rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// PUT /api/devices/{ieee} — rename (or /attrs, /options variants).
esp_err_t handle_put_device(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char ieee_str[24] = {};
    char sub[16]      = {};
    if (!uri_parse_device(req, ieee_str, sizeof(ieee_str), sub, sizeof(sub))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_OK;
    }

    char body[256] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    // For sub==options the original REST handler stored the raw body
    // verbatim to NVS. We preserve that by passing the ieee + a
    // `raw_body` sidecar field through the args JSON.
    char args[384];
    if (strcmp(sub, "options") == 0) {
        // Synthesize args: { ieee, sub: "options", raw: <original body> }
        // Validate body is a JSON object first — matches the old handler's
        // `body[0] != '{'` check.
        if (body[0] != '{') {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "body must be JSON object");
            return ESP_OK;
        }
        // Parse to allow api helper to read occupancy_timeout/flood_protection/etc.
        // and pass the raw body string as "__raw" for NVS storage.
        JsonDocument doc;
        if (deserializeJson(doc, body, (size_t)received)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
            return ESP_OK;
        }
        doc["ieee"]  = ieee_str;
        doc["sub"]   = sub;
        doc["__raw"] = body;  // preserve verbatim copy for NVS write
        size_t n = serializeJson(doc, args, sizeof(args));
        if (n == 0 || n >= sizeof(args)) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "args");
            return ESP_OK;
        }
        args[n] = '\0';
    } else {
        if (!build_args_json(body, received, ieee_str, sub, false, false,
                               args, sizeof(args))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
            return ESP_OK;
        }
    }

    char* buf = static_cast<char*>(rest_big_alloc(8 * 1024));   // F33: PSRAM
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t rsp_len = 0;
    // Route to the domain-specific handler. The ws dispatch calls these
    // directly; REST uses `sub` in the URI path as the selector.
    ApiHandlerFn fn = api_device_rename;
    if (strcmp(sub, "attrs")   == 0) fn = api_device_attr_set;
    if (strcmp(sub, "options") == 0) fn = api_device_options_set;
    ApiStatus st = fn(args, strlen(args), buf, 8 * 1024, &rsp_len);
    if (st == API_BAD_REQUEST) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)rsp_len);
    free(buf);
    return ESP_OK;
}

// POST /api/devices/:ieee/interview — re-trigger full interview sequence
esp_err_t handle_post_interview(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char ieee_str[24] = {};
    char sub[16]      = {};
    if (!uri_parse_device(req, ieee_str, sizeof(ieee_str), sub, sizeof(sub))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ieee");
        return ESP_OK;
    }

    char args[96];
    if (!build_args_json(nullptr, 0, ieee_str, nullptr, false, false,
                           args, sizeof(args))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri");
        return ESP_OK;
    }

    char rsp[80];
    size_t rsp_len = 0;
    ApiStatus st = api_device_reinterview(args, strlen(args),
                                           rsp, sizeof(rsp), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "interview");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, rsp, (ssize_t)rsp_len);
    return ESP_OK;
}

// POST /api/devices/* dispatcher — ESP-IDF's httpd_uri_match_wildcard
// (src/httpd_uri.c:24) only matches a TRAILING `*`/`?`. Patterns like
// `/api/devices/*/bind` silently never match any real URI. Register one
// trailing-star handler and route internally by URI suffix.
esp_err_t handle_post_device_subroute(httpd_req_t* req) {
    const char* uri = req->uri;
    size_t uri_len  = strlen(uri);
    auto ends_with = [&](const char* suf) {
        size_t sl = strlen(suf);
        return uri_len >= sl && strcmp(uri + uri_len - sl, suf) == 0;
    };
    if (ends_with("/bind"))      return handle_post_device_bind(req);
    if (ends_with("/unbind"))    return handle_post_device_bind(req);
    if (ends_with("/interview")) return handle_post_interview(req);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown device sub-route");
    return ESP_OK;
}
