// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_backoff.h"

uint32_t remote_backoff_compute(uint8_t attempt,
                                uint16_t max_s,
                                uint32_t rnd) {
    if (attempt == 0) return 0;
    if (attempt > 6)  attempt = 6;

    // base = 2^(attempt-1) — 1, 2, 4, 8, 16, 32
    uint32_t base = 1u << (attempt - 1);
    if (base > max_s) base = max_s;

    // jitter factor in tenths: 8..12  ->  0.8x..1.2x
    // (rnd % 5) yields 0..4; offset to 8..12.
    uint32_t factor_tenths = 8u + (rnd % 5);
    uint32_t delay = (base * factor_tenths) / 10u;

    // Clamp against max_s after jitter (1.2x can exceed cap).
    if (delay > max_s) delay = max_s;
    return delay;
}
