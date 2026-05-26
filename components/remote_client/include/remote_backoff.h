// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

// Compute reconnect backoff delay in seconds, given the consecutive
// failure attempt count and the max cap. Result includes ±20% jitter
// derived from `rnd` (caller provides an entropy source — typically
// esp_random()).
//
// Contract:
//   attempt == 0   => 0 s (caller is in CONNECTING right after success)
//   attempt >= 1   => min(max_s, 2^(attempt-1)) * (0.8..1.2)
//   attempt clamped internally to 6 to prevent 2^N overflow.
//
// Pure: no IDF deps, host-testable.
uint32_t remote_backoff_compute(uint8_t attempt,
                                uint16_t max_s,
                                uint32_t rnd);
