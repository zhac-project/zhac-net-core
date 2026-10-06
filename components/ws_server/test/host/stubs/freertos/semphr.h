// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
#include <atomic>
#include <cerrno>
typedef pthread_mutex_t* SemaphoreHandle_t;
// Test hook: the next N xSemaphoreTake() calls fail, i.e. the lock is "not taken".
inline int g_take_fails = 0;
// Takes by the task that already owns the mutex. A real (non-recursive) FreeRTOS mutex blocks forever
// there; this error-checking pthread mutex reports it instead of hanging, the take "fails", and it is
// counted here so a test can assert it never happens.
inline std::atomic<int> g_self_deadlocks{0};
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* m = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t) {
    if (g_take_fails > 0) { g_take_fails--; return pdFALSE; }
    const int rc = pthread_mutex_lock(m);
    if (rc == EDEADLK) g_self_deadlocks++;
    return rc == 0 ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0 ? pdTRUE : pdFALSE; }
