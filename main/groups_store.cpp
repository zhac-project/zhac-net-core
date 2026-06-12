// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// groups_store.cpp — NVS persistence layer for Zigbee device groups
#include "groups_store.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>

static constexpr const char* GRP_NVS_NS = "zhac_grp";

// ── Cross-task serialisation (FINDINGS §6, P4-T29) ─────────────────────────
// `group.list` is remote-allow-listed, so it runs on BOTH the single httpd
// task (local REST/WS) AND task_remote (cloud-invoked). Those two contexts can
// therefore touch this store concurrently: a httpd `group.create`/`update`/
// `delete` mutating the bitmap+blobs while task_remote walks the same NVS
// namespace for a `group.list`, and — worse — the non-atomic next_id()→save()
// in api_group_create lets two concurrent creators pick the SAME slot id and
// clobber each other. NVS handles are not a substitute for this: each op opens
// its own handle, so the bitmap read-modify-write in save()/delete() is not
// atomic against a concurrent reader/writer.
//
// Mirror the wifi-scan fix (P2-T16): one store-wide mutex held across each
// whole operation, so every bitmap+blob sequence is atomic across task
// contexts. RECURSIVE so a compound op (grp_create = next_id + save) can hold
// the lock across both sub-calls without the next_id→save TOCTOU that let two
// creators race onto the same slot. Lazily created (first call is in
// single-task boot context).
static SemaphoreHandle_t s_grp_mtx = nullptr;
static inline void grp_lock_init() {
    if (!s_grp_mtx) s_grp_mtx = xSemaphoreCreateRecursiveMutex();
}
namespace {
struct GrpLockGuard {
    GrpLockGuard()  { grp_lock_init(); if (s_grp_mtx) xSemaphoreTakeRecursive(s_grp_mtx, portMAX_DELAY); }
    ~GrpLockGuard() { if (s_grp_mtx) xSemaphoreGiveRecursive(s_grp_mtx); }
};
}  // namespace

// Public lock accessors so a caller that loads into a SHARED scratch buffer
// (api_group_list's file-static `all[]`, reachable from both httpd and
// task_remote) can hold the store lock across BOTH the grp_load_all and the
// serialise-from-the-buffer loop — otherwise a second group.list on the other
// task could refill the same static mid-serialisation (torn read). Recursive,
// so the grp_load_all inside the held region re-takes cheaply.
void grp_store_lock()   { grp_lock_init(); if (s_grp_mtx) xSemaphoreTakeRecursive(s_grp_mtx, portMAX_DELAY); }
void grp_store_unlock() { if (s_grp_mtx) xSemaphoreGiveRecursive(s_grp_mtx); }

static nvs_handle_t grp_open(nvs_open_mode_t mode) {
    nvs_handle_t h = 0;
    nvs_open(GRP_NVS_NS, mode, &h);
    return h;
}

static void grp_key(char* out, uint16_t id) { snprintf(out, 8, "g%04x", id); }

static uint32_t grp_get_bmp(nvs_handle_t h) {
    uint32_t bmp = 0;
    nvs_get_u32(h, "bmp", &bmp);
    return bmp;
}

// Write a JSON-escaped string into buf[pos..cap), returning bytes written.
// Handles: backslash, double-quote, control chars (\n, \r, \t), and other <0x20 as \u00XX.
static size_t json_write_str(const char* s, char* buf, size_t pos, size_t cap) {
    // DS14/Q14 (findings): the previous loop guarded only `pos < cap` at the top
    // but the 2-byte escapes below write TWO bytes — a 1-byte overflow past `cap`
    // when pos == cap-1 on an escaped char at the boundary. Check the exact byte
    // count needed per character before writing.
    size_t start = pos;
    for (const char* p = s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
            if (pos + 2 > cap) break;
            buf[pos++] = '\\';
            buf[pos++] = (c == '"')  ? '"' :
                         (c == '\\') ? '\\' :
                         (c == '\n') ? 'n' :
                         (c == '\r') ? 'r' : 't';
        } else if (c < 0x20) {
            if (pos + 6 > cap) break;   // \u00XX = 6 bytes
            int n = snprintf(buf + pos, cap - pos, "\\u%04x", c);
            if (n < 0) break;
            pos += (size_t)n;
        } else {
            if (pos + 1 > cap) break;
            buf[pos++] = (char)c;
        }
    }
    return pos - start;
}

uint16_t grp_next_id() {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    uint32_t bmp = h ? grp_get_bmp(h) : 0;
    if (h) nvs_close(h);
    for (uint8_t i = 0; i < GRP_MAX_GROUPS; i++) {
        if (!(bmp & (1u << i))) return (uint16_t)(i + 1);
    }
    return 0;  // all slots full
}

uint16_t grp_load_all(GrpRecord* out, uint16_t max) {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    if (!h) return 0;
    uint32_t bmp = grp_get_bmp(h);
    uint16_t loaded = 0;
    for (uint8_t i = 0; i < GRP_MAX_GROUPS && loaded < max; i++) {
        if (!(bmp & (1u << i))) continue;
        uint16_t id = (uint16_t)(i + 1);
        char key[8]; grp_key(key, id);
        GrpRecord r{};
        size_t sz = sizeof(r);
        if (nvs_get_blob(h, key, &r, &sz) == ESP_OK && r.id == id)
            out[loaded++] = r;
    }
    nvs_close(h);
    return loaded;
}

bool grp_save(const GrpRecord& r) {
    if (r.id == 0 || r.id > GRP_MAX_GROUPS) return false;
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READWRITE);
    if (!h) return false;
    char key[8]; grp_key(key, r.id);
    esp_err_t err = nvs_set_blob(h, key, &r, sizeof(r));
    if (err == ESP_OK) {
        uint32_t bmp = grp_get_bmp(h);
        bmp |= (1u << (r.id - 1));
        nvs_set_u32(h, "bmp", bmp);
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool grp_create(GrpRecord& r) {
    // Atomic id-allocation + persist: hold the store lock across BOTH the
    // next_id() probe and the save() so two concurrent creators (httpd +
    // task_remote) cannot read the same free slot and clobber one another.
    // (Recursive mutex — the inner grp_next_id/grp_save re-take it cheaply.)
    GrpLockGuard guard;
    r.id = grp_next_id();
    if (r.id == 0) return false;   // table full
    return grp_save(r);
}

bool grp_delete(uint16_t id) {
    if (id == 0 || id > GRP_MAX_GROUPS) return false;
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READWRITE);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    nvs_erase_key(h, key);
    uint32_t bmp = grp_get_bmp(h);
    bmp &= ~(1u << (id - 1));
    nvs_set_u32(h, "bmp", bmp);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool grp_find(uint16_t id, GrpRecord& out) {
    GrpLockGuard guard;
    nvs_handle_t h = grp_open(NVS_READONLY);
    if (!h) return false;
    char key[8]; grp_key(key, id);
    size_t sz = sizeof(out);
    esp_err_t err = nvs_get_blob(h, key, &out, &sz);
    nvs_close(h);
    return err == ESP_OK && out.id == id;
}

size_t grp_to_json(const GrpRecord& r, char* buf, size_t cap) {
    size_t pos = 0;
    pos += snprintf(buf + pos, cap - pos, "{\"id\":%u,\"name\":\"", r.id);
    json_write_str(r.name, buf, pos, cap);
    pos += snprintf(buf + pos, cap - pos, "\",\"members\":[");
    for (uint8_t i = 0; i < r.member_count && pos < cap; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, cap - pos, "{\"ieee\":\"0x%016llX\",\"ep\":%u}",
                        (unsigned long long)r.members[i].ieee, r.members[i].ep);
    }
    pos += snprintf(buf + pos, cap - pos, "]}");
    return pos < cap ? pos : 0;
}
