// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
typedef pthread_mutex_t* SemaphoreHandle_t;
// Test hook: the next N xSemaphoreTake() calls fail, i.e. the lock is "not taken".
inline int g_take_fails = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* m = new pthread_mutex_t;
    pthread_mutex_init(m, nullptr);
    return m;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t) {
    if (g_take_fails > 0) { g_take_fails--; return pdFALSE; }
    return pthread_mutex_lock(m) == 0 ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0 ? pdTRUE : pdFALSE; }
