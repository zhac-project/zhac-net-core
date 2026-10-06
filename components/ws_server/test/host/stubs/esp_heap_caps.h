// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT   0
static inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
