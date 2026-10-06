// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
// Host FreeRTOS shim for the ws_server race test: REAL pthread mutexes (the test needs
// real threads, unlike the single-threaded shims in the other host harnesses).
#include <pthread.h>
#include <cstdint>
#include <cstdlib>
typedef int      BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define configASSERT(x) do { if (!(x)) abort(); } while (0)
