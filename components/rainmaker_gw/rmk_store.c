// SPDX-License-Identifier: AGPL-3.0-or-later
// rmk_store — RainMaker exposed-device set. Pure table ops (host-tested) +
// a thin NVS-backed store (one "set" blob in the "zhac_rmk" namespace),
// split the same way as dgm_store: the NVS half carries no #ifdef guard —
// it compiles unconditionally against either the real ESP-IDF nvs/freertos
// headers (firmware) or the host stubs under test/host/stubs (host tests),
// where NVS is a no-op so persistence itself is not unit-tested there.
#include "rmk_store.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

// ── Pure table operations ────────────────────────────────────────────────
bool rmk_tbl_add(RmkTable* t, uint64_t ieee) {
    for (size_t i = 0; i < t->count; i++) if (t->ieee[i] == ieee) return true;  // dedup
    if (t->count >= RMK_MAX_DEVS) return false;                                 // table full
    t->ieee[t->count++] = ieee;
    return true;
}

bool rmk_tbl_remove(RmkTable* t, uint64_t ieee) {
    for (size_t i = 0; i < t->count; i++) {
        if (t->ieee[i] != ieee) continue;
        t->ieee[i] = t->ieee[t->count - 1];   // swap-with-last
        t->count--;
        return true;
    }
    return false;
}

bool rmk_tbl_contains(const RmkTable* t, uint64_t ieee) {
    for (size_t i = 0; i < t->count; i++) if (t->ieee[i] == ieee) return true;
    return false;
}

static bool live_contains(const uint64_t* live, size_t nlive, uint64_t ieee) {
    for (size_t i = 0; i < nlive; i++) if (live[i] == ieee) return true;
    return false;
}

size_t rmk_tbl_reconcile(RmkTable* t, const uint64_t* live, size_t nlive) {
    size_t dropped = 0;
    size_t i = 0;
    while (i < t->count) {
        if (live_contains(live, nlive, t->ieee[i])) {
            i++;
            continue;
        }
        t->ieee[i] = t->ieee[t->count - 1];   // swap-with-last
        t->count--;
        dropped++;
        // do not advance i — re-check the entry swapped into this slot
    }
    return dropped;
}

// ── NVS-backed store ─────────────────────────────────────────────────────
#define RMK_NS  "zhac_rmk"
#define RMK_KEY "set"

static SemaphoreHandle_t s_mtx = NULL;
static void rmk_lock_init(void) { if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex(); }

void rmk_store_init(void) { rmk_lock_init(); }

bool rmk_store_load(RmkTable* out) {
    rmk_lock_init();
    if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);

    memset(out, 0, sizeof(*out));
    bool ok = false;
    nvs_handle_t h = 0;
    if (nvs_open(RMK_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(*out);
        esp_err_t err = nvs_get_blob(h, RMK_KEY, out, &sz);
        nvs_close(h);
        if (err == ESP_OK && sz == sizeof(*out)) {
            ok = true;
        } else {
            memset(out, 0, sizeof(*out));
        }
    }
    if (out->count > RMK_MAX_DEVS) out->count = RMK_MAX_DEVS;  // clamp a corrupt/stale blob

    if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
    return ok;
}

bool rmk_store_save(const RmkTable* t) {
    rmk_lock_init();
    if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);

    bool ok = false;
    nvs_handle_t h = 0;
    if (nvs_open(RMK_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, RMK_KEY, t, sizeof(*t));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        ok = (err == ESP_OK);
    }

    if (s_mtx) xSemaphoreGiveRecursive(s_mtx);
    return ok;
}
