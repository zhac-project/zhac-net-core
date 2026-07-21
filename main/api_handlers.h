// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Shared command handlers — used by every client-facing transport
// (REST today, WebSocket in Phase 2 of the SPA migration plan at
// `docs/plans/2026-04-22-unified-api.md`). MQTT stays independent
// with its own pub/sub idioms.
//
// Each handler:
//   - takes a request body (may be empty for args-less commands,
//     e.g. GET-style status endpoints),
//   - fills a caller-provided response buffer with the JSON body,
//   - returns an `ApiStatus` code — the REST/WS transports translate
//     this into an HTTP status code / a `{ok:false,err:...}` frame.
//
// Handlers are pure business logic. Auth, rate-limiting, and wire
// framing are the transport's job.
//
// Exceptions left as REST-only (not migrated here):
//   - handle_get_metrics        — Prometheus plaintext, not JSON.
//   - handle_get_static         — SPIFFS streaming, not a command.
//   - handle_post_scripts_bulk  — body can be up to HAP_MAX_PAYLOAD,
//                                 too large for a shared stack-alloc
//                                 response buffer without allocator
//                                 coupling. Stays in rest_rules.cpp.

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : uint8_t {
    API_OK                 = 0,
    API_BAD_REQUEST        = 1,
    API_NOT_FOUND          = 2,
    API_INTERNAL_ERROR     = 3,
    API_METHOD_NOT_ALLOWED = 4,
} ApiStatus;

// Function-pointer alias matching every api_* handler's signature.
typedef ApiStatus (*ApiHandlerFn)(const char*, size_t, char*, size_t, size_t*);

// ── Status / system ──────────────────────────────────────────────────────
// GET /api/status — args-less. Reply buffer must hold >= 2048 bytes.
ApiStatus api_status_get(const char* body, size_t body_len,
                          char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/alerts — args-less. Reply buffer >= 1024 bytes.
ApiStatus api_alerts_get(const char* body, size_t body_len,
                          char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/logs — args-less. Reply buffer >= 32 KB.
ApiStatus api_logs_get(const char* body, size_t body_len,
                        char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/diagnostics/unhandled — args-less. Reply buffer >= 2048 bytes.
ApiStatus api_diagnostics_unhandled_get(const char* body, size_t body_len,
                                         char* rsp_buf, size_t rsp_cap,
                                         size_t* rsp_len);

// POST /api/settings — args: JSON blob with any subset of settings.
ApiStatus api_settings_set(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/system/token/rotate — args-less. Generates a fresh
// 32-hex-char API token, persists to NVS, applies to ws_server.
// Existing WS clients are dropped on next bad-key check; SPA must
// reconnect with the new token. Reply: {"token":"..."}.
ApiStatus api_token_rotate(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── WiFi ─────────────────────────────────────────────────────────────────
// GET /api/wifi/status
ApiStatus api_wifi_status(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/wifi/scan — reply buffer >= 2048 bytes.
ApiStatus api_wifi_scan(const char* body, size_t body_len,
                         char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/wifi — args: {"ssid":"...","pass":"..."} — reboots the device.
ApiStatus api_wifi_connect(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// DELETE /api/wifi — forget credentials + reboot.
ApiStatus api_wifi_disconnect(const char* body, size_t body_len,
                               char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── OTA ──────────────────────────────────────────────────────────────────
// POST /api/ota — args: {"url":"http..."}.
ApiStatus api_ota_s3(const char* body, size_t body_len,
                      char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/p4-ota — args: {"url":"http..."}.
ApiStatus api_ota_p4(const char* body, size_t body_len,
                      char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── Zigbee control ───────────────────────────────────────────────────────
// Body:  {"duration":N}   (optional, defaults to 254)
// Reply: {"ok":true}
ApiStatus api_zigbee_permit_join(const char* body, size_t body_len,
                                  char* rsp_buf, size_t rsp_cap,
                                  size_t* rsp_len);

// Query the current local view of the permit-join window.
// Reply: {"ok":true,"data":{"open":bool,"remaining_sec":int,"duration":int}}
// Tracks what we last requested — authoritative state lives on P4 but
// this is sufficient for UI status display.
ApiStatus api_zigbee_permit_join_status(const char* body, size_t body_len,
                                         char* rsp_buf, size_t rsp_cap,
                                         size_t* rsp_len);

// POST /api/zigbee/reset — no args, triggers factory reset.
ApiStatus api_zigbee_reset(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/zigbee/settings — {"channel":11..26,"net_key_hex":"...","regenerate":bool}
ApiStatus api_zigbee_settings_set(const char* body, size_t body_len,
                                    char* rsp_buf, size_t rsp_cap,
                                    size_t* rsp_len);

// ── Devices ──────────────────────────────────────────────────────────────
// GET /api/devices
ApiStatus api_device_list(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/devices/{ieee} — args: {"ieee":"0x..."} (+ optional "sub":"options").
ApiStatus api_device_get(const char* body, size_t body_len,
                          char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/devices/{ieee}/bind — args: {"ieee":"...","unbind":bool,...}.
ApiStatus api_device_bind(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// DELETE /api/devices/{ieee} — args: {"ieee":"...","hard":bool}.
ApiStatus api_device_delete(const char* body, size_t body_len,
                             char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// WS device.rename / PUT /api/devices/{ieee} — args {ieee, name}.
ApiStatus api_device_rename(const char* body, size_t body_len,
                             char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// WS device.attr.set / PUT /api/devices/{ieee} with sub="attrs" —
// args {ieee, key, value, ep?, cluster?, attr?}. Accepts `val` as a
// legacy alias for `value`.
ApiStatus api_device_attr_set(const char* body, size_t body_len,
                               char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// Core of api_device_attr_set (Phase 4 extraction, Task 15) — takes
// already-parsed/validated fields instead of a JSON body so a non-JSON
// caller (Task 17's RainMaker param-write path) can drive the identical
// SET_ATTRIBUTE→P4 pipeline. Builds HapSetAttrReq{ieee,ep,cluster,attr,
// val,key}, runs the same 3000 ms SET_ATTRIBUTE roundtrip, and decodes
// the ack's "ok" into *cmd_ok_out. Does not encode a reply body — that
// stays the caller's job.
//
// Returns API_INTERNAL_ERROR (and leaves *cmd_ok_out untouched) if the
// request failed to encode or the P4 roundtrip failed/timed out — no
// reply is implied then, matching api_device_attr_set's original
// early-return paths. Returns API_OK once an ack was received;
// *cmd_ok_out reports whether the device accepted the command.
//
// `key` must already be validated to fit HapSetAttrReq::key (<=23 chars,
// NUL-terminated) — this function does not re-check.
ApiStatus device_attr_set_core(uint64_t ieee, const char* key, int32_t val,
                                uint8_t ep, uint16_t cluster, uint16_t attr,
                                bool* cmd_ok_out);

// WS device.options.set / PUT /api/devices/{ieee} with sub="options" —
// args {ieee, occupancy_timeout?, debounce_ms?, flood_protection?, throttle_ms?}.
ApiStatus api_device_options_set(const char* body, size_t body_len,
                                  char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// P4-T29: force the debounced device-options NVS commit to land now. Called
// from reboot/OTA paths so a staged-but-not-yet-flushed device.options.set
// write is made durable before restart. No-op when nothing is pending.
void api_device_opt_flush_now(void);

// POST /api/devices/{ieee}/interview — args: {"ieee":"..."}.
ApiStatus api_device_reinterview(const char* body, size_t body_len,
                                  char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/device/configure — args: {"ieee":"..."}. Re-fires
// `run_configure` on P4 without redoing the full interview. Use after
// firmware updates a def's reports[] / config_steps[] for a paired
// device that doesn't otherwise re-trigger configure.
ApiStatus api_device_configure(const char* body, size_t body_len,
                                char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── Rules ────────────────────────────────────────────────────────────────
ApiStatus api_rule_list(const char* body, size_t body_len,
                         char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

ApiStatus api_rule_create(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

ApiStatus api_rule_delete(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

ApiStatus api_rule_enable(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// PUT /api/rules/{id} — args: {"id":N,"dsl":"...","name":"..."}.
ApiStatus api_rule_update(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── Scripts ──────────────────────────────────────────────────────────────
ApiStatus api_script_list(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/scripts/{name} — args: {"name":"..."}.
ApiStatus api_script_read(const char* body, size_t body_len,
                           char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// DELETE /api/scripts/{name} — args: {"name":"..."}.
ApiStatus api_script_delete(const char* body, size_t body_len,
                             char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/scripts/{name} — args: {"name":"...","src":"..."}.
// (Note: plaintext bulk upload is handled at the transport layer — see
// api_handlers.cpp header comment.)
ApiStatus api_script_write(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/scripts/{name}/run — args: {"name":"..."}. Fires the
// named script as a fresh Lua coroutine on P4 and returns the ACK.
ApiStatus api_script_run(const char* body, size_t body_len,
                          char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// WS script.check — args: {"name":"...","src":"<lua>"}. Parses the
// source on P4 without executing it; returns {"ok","err","line"}.
ApiStatus api_script_check(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── Groups ───────────────────────────────────────────────────────────────
ApiStatus api_group_list(const char* body, size_t body_len,
                          char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

ApiStatus api_group_create(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// GET /api/groups/{id} — args: {"id":N}.
ApiStatus api_group_get(const char* body, size_t body_len,
                         char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// PUT /api/groups/{id} — args: {"id":N,...body...}.
ApiStatus api_group_update(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// DELETE /api/groups/{id} — args: {"id":N}.
ApiStatus api_group_delete(const char* body, size_t body_len,
                            char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// POST /api/groups/{id}/cmd — args: {"id":N,"key":"state","val":1}.
ApiStatus api_group_cmd(const char* body, size_t body_len,
                         char* rsp_buf, size_t rsp_cap, size_t* rsp_len);

// ── Remote (premium feature, Kconfig-gated) ──────────────────────────────
#ifdef CONFIG_ZHAC_REMOTE_CLIENT_ENABLE
ApiStatus api_remote_status(const char* body, size_t body_len,
                             char* rsp_buf, size_t rsp_cap, size_t* rsp_len);
ApiStatus api_remote_connect(const char* body, size_t body_len,
                              char* rsp_buf, size_t rsp_cap, size_t* rsp_len);
ApiStatus api_remote_disconnect(const char* body, size_t body_len,
                                 char* rsp_buf, size_t rsp_cap, size_t* rsp_len);
#endif

#ifdef __cplusplus
}
#endif
