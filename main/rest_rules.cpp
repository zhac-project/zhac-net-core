// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "s3_internal.h"
#include "api_handlers.h"
#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "hap_json.h"
#include "hap_protocol.h"
#include "groups_store.h"
#include "ArduinoJson.h"

// ── Rule helpers (kept for the bulk-scripts handler that stays REST-only)

static uint16_t uri_last_id(httpd_req_t* req) {
    const char* slash = strrchr(req->uri, '/');
    return slash ? (uint16_t)strtoul(slash + 1, nullptr, 10) : 0;
}

// Extract the last path segment (e.g. "motion" from "/api/scripts/motion"),
// strip any query string, and copy into `out` (must hold cap bytes). Returns
// true on non-empty valid-shaped name (plain printable chars, length <=
// cap-1). Full sanitization happens on P4 via the Lua script-cache
// name_ok validator.
static bool uri_last_name(httpd_req_t* req, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char* slash = strrchr(req->uri, '/');
    if (!slash) return false;
    const char* seg = slash + 1;
    size_t n = 0;
    while (seg[n] && seg[n] != '?' && n < cap - 1) {
        char c = seg[n];
        if (c <= ' ' || c == '/') return false;
        out[n] = c;
        n++;
    }
    if (n == 0) return false;
    out[n] = '\0';
    return true;
}

// ── Rule REST handlers ────────────────────────────────────────────────────

esp_err_t handle_get_rules(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    TRY_CHANNEL(req, s_rule_req_mutex);
    xSemaphoreGive(s_rule_req_mutex);

    char* buf = static_cast<char*>(malloc(16 * 1024));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    ApiStatus st = api_rule_list(nullptr, 0, buf, 16 * 1024, &n);
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

esp_err_t handle_post_rules(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char body[512] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    char* buf = static_cast<char*>(malloc(4 * 1024));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t rsp_len = 0;
    ApiStatus st = api_rule_create(body, (size_t)received, buf, 4 * 1024, &rsp_len);
    if (st == API_BAD_REQUEST) {
        // api_rule_create populates `buf` with the rule-rejection payload
        // when the DSL fails to parse; extract err for the 400 message.
        JsonDocument rd;
        const char* err = "rule rejected";
        if (rsp_len > 0 && deserializeJson(rd, buf, rsp_len) == DeserializationError::Ok) {
            err = rd["err"] | "rule rejected";
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "rule rejected: %s", err);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
        return ESP_OK;
    }
    if (st != API_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)rsp_len);
    free(buf);
    return ESP_OK;
}

esp_err_t handle_delete_rules(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char body[64] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    char buf[512];
    size_t n = 0;
    ApiStatus st = api_rule_delete(body, (size_t)received, buf, sizeof(buf), &n);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

esp_err_t handle_put_rules(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char body[128] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    char buf[512];
    size_t n = 0;
    ApiStatus st = api_rule_enable(body, (size_t)received, buf, sizeof(buf), &n);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// PUT /api/rules/{id} — replace DSL of an existing rule
esp_err_t handle_put_rule_dsl(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    uint16_t rule_id = uri_last_id(req);
    if (rule_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid id");
        return ESP_OK;
    }
    static char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    // Inject the URL-path id into the body so api_rule_update sees it.
    JsonDocument doc;
    if (deserializeJson(doc, body, (size_t)received)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    doc["id"] = rule_id;
    char args[600];
    size_t args_len = serializeJson(doc, args, sizeof(args));
    if (args_len == 0 || args_len >= sizeof(args)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "args");
        return ESP_OK;
    }

    char* buf = static_cast<char*>(malloc(4 * 1024));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t rsp_len = 0;
    ApiStatus st = api_rule_update(args, args_len, buf, 4 * 1024, &rsp_len);
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

// ── Script REST handlers ──────────────────────────────────────────────────

esp_err_t handle_get_scripts(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    TRY_CHANNEL(req, s_script_req_mutex);
    xSemaphoreGive(s_script_req_mutex);

    char* buf = static_cast<char*>(malloc(16 * 1024));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    ApiStatus st = api_script_list(nullptr, 0, buf, 16 * 1024, &n);
    httpd_resp_set_type(req, "application/json");
    if (st != API_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scripts");
        return ESP_OK;
    }
    httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return ESP_OK;
}

// GET /api/scripts/{name} — fetch script content
esp_err_t handle_get_script_by_id(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char name[HAP_SCRIPT_NAME_MAX + 1];
    if (!uri_last_name(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    char args[HAP_SCRIPT_NAME_MAX + 32];
    int args_len = snprintf(args, sizeof(args), "{\"name\":\"%s\"}", name);
    char* buf = static_cast<char*>(malloc(HAP_SCRIPT_MAX_SRC + 256));
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t rsp_len = 0;
    ApiStatus st = api_script_read(args, (size_t)args_len,
                                    buf, HAP_SCRIPT_MAX_SRC + 256, &rsp_len);
    if (st == API_BAD_REQUEST) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
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

// DELETE /api/scripts/{name} — remove script file
esp_err_t handle_delete_script(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char name[HAP_SCRIPT_NAME_MAX + 1];
    if (!uri_last_name(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    char args[HAP_SCRIPT_NAME_MAX + 32];
    int args_len = snprintf(args, sizeof(args), "{\"name\":\"%s\"}", name);
    char buf[512];
    size_t rsp_len = 0;
    ApiStatus st = api_script_delete(args, (size_t)args_len,
                                      buf, sizeof(buf), &rsp_len);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)rsp_len);
    return ESP_OK;
}

// POST /api/scripts/{name} — save single script (raw .be text body).
// Body is raw source (not JSON), so the REST wrapper wraps it into
// {"name":..., "src":...} before handing to the api helper.
esp_err_t handle_post_script(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    char name[HAP_SCRIPT_NAME_MAX + 1];
    if (!uri_last_name(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    char* src_buf = static_cast<char*>(malloc(HAP_SCRIPT_MAX_SRC + 1));
    if (!src_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    int received = httpd_req_recv(req, src_buf, HAP_SCRIPT_MAX_SRC);
    if (received <= 0) {
        free(src_buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    src_buf[received] = '\0';

    // Build args JSON: {"name":"...","src":"..."}.
    char* args = static_cast<char*>(malloc(HAP_SCRIPT_MAX_SRC + 128));
    if (!args) {
        free(src_buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    JsonDocument doc;
    doc["name"] = name;
    doc["src"]  = src_buf;
    size_t args_len = serializeJson(doc, args, HAP_SCRIPT_MAX_SRC + 128);
    free(src_buf);
    if (args_len == 0 || args_len >= HAP_SCRIPT_MAX_SRC + 128) {
        free(args);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode");
        return ESP_OK;
    }

    char buf[512];
    size_t rsp_len = 0;
    ApiStatus st = api_script_write(args, args_len, buf, sizeof(buf), &rsp_len);
    free(args);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "P4 timeout");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)rsp_len);
    return ESP_OK;
}

// POST /api/scripts — bulk save: [{"name":"motion","src":"..."},...]
// STAYS REST-only: the request body can be up to HAP_MAX_PAYLOAD bytes,
// and the handler streams each entry through SCRIPT_WRITE under a single
// TRY_CHANNEL. A shared api_* helper would need a matching streaming
// contract; not worth the plumbing until WebSocket actually needs bulk
// imports.
esp_err_t handle_post_scripts_bulk(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    TRY_CHANNEL(req, s_script_req_mutex);

    char*    body    = static_cast<char*>(malloc(HAP_MAX_PAYLOAD));
    uint8_t* hap_buf = static_cast<uint8_t*>(malloc(HAP_MAX_PAYLOAD));
    if (!body || !hap_buf) {
        free(body); free(hap_buf);
        xSemaphoreGive(s_script_req_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, body, HAP_MAX_PAYLOAD - 1);
    if (received <= 0) {
        free(body); free(hap_buf);
        xSemaphoreGive(s_script_req_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    body[received] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body, (size_t)received)) {
        free(body); free(hap_buf);
        xSemaphoreGive(s_script_req_mutex);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    free(body);  // ArduinoJson 7 copies strings into its own storage; body no longer needed

    uint16_t written = 0;
    uint16_t failed  = 0;
    for (JsonObject obj : doc.as<JsonArray>()) {
        const char* name = obj["name"] | (const char*)nullptr;
        const char* src  = obj["src"]  | (const char*)nullptr;
        if (!name || name[0] == '\0' || !src || src[0] == '\0') { failed++; continue; }
        if (strlen(name) > HAP_SCRIPT_NAME_MAX)   { failed++; continue; }
        if (strlen(src)  >= HAP_SCRIPT_MAX_SRC)   { failed++; continue; }

        uint16_t hap_len = 0;
        if (!hap_json_encode_script_write(hap_buf, HAP_MAX_PAYLOAD, &hap_len, name, src)) {
            failed++; continue;
        }
        bool ok = hap_roundtrip(HapMsgType::SCRIPT_WRITE, hap_buf, hap_len,
                                 s_script_rsp_sem, 5000);
        if (ok) written++; else failed++;
    }

    free(hap_buf);
    xSemaphoreGive(s_script_req_mutex);

    char resp[64];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"written\":%u,\"failed\":%u}", written, failed);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, n);
}

// ── Groups REST handlers ──────────────────────────────────────────────────

// Extract the numeric group id from a URL path. For /api/groups/42/cmd
// the id is the penultimate segment; for /api/groups/42 it's the last.
static uint16_t uri_group_id(httpd_req_t* req, bool penultimate) {
    if (!req || !req->uri[0]) return 0;
    const char* uri = req->uri;
    const char* last = strrchr(uri, '/');
    if (!last) return 0;
    if (!penultimate) return (uint16_t)atoi(last + 1);
    const char* prev = last - 1;
    while (prev > uri && *prev != '/') prev--;
    return (uint16_t)atoi(prev + 1);
}

// Inject {"id":N} into an optional body (or synthesize a minimal body
// when there is none). Returns number of bytes written (not counting NUL).
static int make_group_args(const char* body, int body_len,
                            uint16_t id, char* out, size_t out_cap) {
    JsonDocument doc;
    if (body && body_len > 0) {
        if (deserializeJson(doc, body, (size_t)body_len)) return -1;
    }
    doc["id"] = id;
    size_t n = serializeJson(doc, out, out_cap);
    if (n == 0 || n >= out_cap) return -1;
    out[n] = '\0';
    return (int)n;
}

// GET /api/groups
esp_err_t handle_get_groups(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    EXT_RAM_BSS_ATTR static char buf[4096];
    size_t n = 0;
    ApiStatus st = api_group_list(nullptr, 0, buf, sizeof(buf), &n);
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "groups");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// POST /api/groups — create: {"name":"...","members":[...]}
esp_err_t handle_post_groups(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    static char body[1024];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body"); return ESP_OK; }
    body[received] = '\0';

    static char resp[256];
    size_t n = 0;
    ApiStatus st = api_group_create(body, (size_t)received,
                                      resp, sizeof(resp), &n);
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, (ssize_t)n);
    return ESP_OK;
}

// GET /api/groups/:id
esp_err_t handle_get_group_by_id(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    uint16_t id = uri_group_id(req, false);
    char args[32];
    int args_len = snprintf(args, sizeof(args), "{\"id\":%u}", id);
    static char buf[256];
    size_t n = 0;
    ApiStatus st = api_group_get(args, (size_t)args_len, buf, sizeof(buf), &n);
    if (st == API_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "group");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// PUT /api/groups/:id — update name/members
esp_err_t handle_put_group(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    uint16_t id = uri_group_id(req, false);
    static char body[1024];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body"); return ESP_OK; }
    body[received] = '\0';

    static char args[1200];
    int args_len = make_group_args(body, received, id, args, sizeof(args));
    if (args_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    static char buf[256];
    size_t n = 0;
    ApiStatus st = api_group_update(args, (size_t)args_len,
                                      buf, sizeof(buf), &n);
    if (st == API_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    if (st == API_BAD_REQUEST) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// DELETE /api/groups/:id
esp_err_t handle_delete_group(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    uint16_t id = uri_group_id(req, false);
    char args[32];
    int args_len = snprintf(args, sizeof(args), "{\"id\":%u}", id);
    char buf[32];
    size_t n = 0;
    ApiStatus st = api_group_delete(args, (size_t)args_len,
                                      buf, sizeof(buf), &n);
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)n);
    return ESP_OK;
}

// POST /api/groups/:id/cmd — {"key":"state","val":1} fan-out to all members
esp_err_t handle_post_group_cmd(httpd_req_t* req) {
    REQUIRE_AUTH(req);
    uint16_t id = uri_group_id(req, true);
    static char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body"); return ESP_OK; }
    body[received] = '\0';

    static char args[192];
    int args_len = make_group_args(body, received, id, args, sizeof(args));
    if (args_len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_OK;
    }

    static char resp[64];
    size_t n = 0;
    ApiStatus st = api_group_cmd(args, (size_t)args_len, resp, sizeof(resp), &n);
    if (st == API_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "group not found or empty");
        return ESP_OK;
    }
    if (st != API_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "group cmd");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, (ssize_t)n);
    return ESP_OK;
}
