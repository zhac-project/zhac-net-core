// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "FreeRTOS.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>

// A mutex with a rank: its creation order. ws_server_init() makes s_mutex (the fd table lock, a leaf) first and
// s_tx_mutex second, so the only legal nesting is later-created -> earlier-created: s_tx_mutex, then s_mutex.
struct SemRec { pthread_mutex_t m; int rank; };
typedef SemRec* SemaphoreHandle_t;
constexpr int kMaxRank = 8;

// Test hook: the next N xSemaphoreTake() calls fail, i.e. the lock is "not taken".
inline int g_take_fails = 0;
// Takes by the task that already owns the mutex. A real (non-recursive) FreeRTOS mutex blocks forever
// there; this error-checking pthread mutex reports it instead of hanging, the take "fails", and it is
// counted here so a test can assert it never happens.
inline std::atomic<int> g_self_deadlocks{0};
// Lock-order check, made BEFORE the take blocks: a task that takes a mutex of HIGHER rank than one it already
// holds is a violation (two tasks doing it in opposite orders deadlock, and a deadlock only hangs). It is
// counted, and aborts the process unless a test turns that off.
inline std::atomic<int> g_lock_order_violations{0};
inline bool g_abort_on_lock_order = true;
inline thread_local int t_held[kMaxRank];
inline thread_local int t_nheld = 0;
// Scheduler model. Per rank: tasks blocked in a take, completed takes, completed gives.
inline std::atomic<int> g_waiting[kMaxRank] = {};
inline std::atomic<int> g_takes[kMaxRank]   = {};
inline std::atomic<int> g_gives[kMaxRank]   = {};
// While set to a rank, the task that gives a mutex of that rank WHILE ANOTHER TASK IS BLOCKED ON IT stays put until
// that waiter has run its whole critical section (its own give). That is what FreeRTOS does when the waiter has the
// higher priority: it takes the mutex at the give and preempts the giver before the giver runs another line.
inline std::atomic<int> g_preempt_rank{-1};

inline std::atomic<int> g_next_rank{0};
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    auto* s = new SemRec;
    s->rank = g_next_rank++;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&s->m, &a);
    pthread_mutexattr_destroy(&a);
    return s;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
    if (g_take_fails > 0) { g_take_fails--; return pdFALSE; }
    for (int i = 0; i < t_nheld; i++) {
        if (t_held[i] < s->rank) {
            g_lock_order_violations++;
            if (g_abort_on_lock_order) {
                fprintf(stderr, "lock order violation: rank %d taken while holding rank %d\n", s->rank, t_held[i]);
                abort();
            }
        }
    }
    int rc = pthread_mutex_trylock(&s->m);
    if (rc == EBUSY) {                      // held by another task (this one blocks) or by this one (it would deadlock)
        g_waiting[s->rank]++;
        rc = pthread_mutex_lock(&s->m);
        g_waiting[s->rank]--;
    }
    if (rc == EDEADLK) g_self_deadlocks++;
    if (rc != 0) return pdFALSE;
    if (t_nheld < kMaxRank) t_held[t_nheld++] = s->rank;
    g_takes[s->rank]++;
    return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    for (int i = 0; i < t_nheld; i++)
        if (t_held[i] == s->rank) { t_held[i] = t_held[--t_nheld]; break; }
    const bool preempt = g_preempt_rank == s->rank && g_waiting[s->rank] > 0;
    const int  gives   = g_gives[s->rank];
    const bool ok      = pthread_mutex_unlock(&s->m) == 0;
    if (ok && preempt)
        for (int i = 0; i < 2000 && g_gives[s->rank] == gives; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    g_gives[s->rank]++;
    return ok ? pdTRUE : pdFALSE;
}
