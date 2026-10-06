// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Host FreeRTOS shim for the ws_server race test: REAL pthread mutexes (the test needs
// real threads, unlike the single-threaded shims in the other host harnesses).
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
typedef int      BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
// configASSERT aborts, as on the device. A test can turn the abort off to observe an assert:
// the failure is counted either way.
inline std::atomic<int> g_assert_failures{0};
inline bool g_assert_aborts = true;
#define configASSERT(x) do { if (!(x)) { g_assert_failures++; if (g_assert_aborts) abort(); } } while (0)
