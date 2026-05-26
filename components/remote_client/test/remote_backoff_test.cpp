// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_backoff.h"
#include <cassert>
#include <cstdio>

int main() {
    // attempt == 0 returns 0 regardless of rnd / max
    assert(remote_backoff_compute(0, 60, 0)        == 0);
    assert(remote_backoff_compute(0, 60, 0xFFFFFFFF) == 0);

    // attempt == 1 returns 1 ± 0.2 — i.e. {0, 1} (integer truncation).
    // The contract is that the result is in [0, 1] for attempt=1.
    for (uint32_t r = 0; r < 1000; r++) {
        uint32_t v = remote_backoff_compute(1, 60, r);
        assert(v <= 1);
    }

    // attempt == 5 -> base = 16 s, jitter band = [12, 19] inclusive
    for (uint32_t r = 0; r < 1000; r++) {
        uint32_t v = remote_backoff_compute(5, 60, r);
        assert(v >= 12 && v <= 19);
    }

    // attempt >= 7 clamps to attempt 6 internally -> base 32 s
    // jitter band = [25, 38]
    for (uint32_t r = 0; r < 1000; r++) {
        uint32_t v = remote_backoff_compute(7, 60, r);
        assert(v >= 25 && v <= 38);
    }
    for (uint32_t r = 0; r < 1000; r++) {
        uint32_t v = remote_backoff_compute(255, 60, r);
        assert(v >= 25 && v <= 38);
    }

    // max_s cap enforced: small max forces clamp.
    // attempt=6, max_s=10 -> base clamps to 10, then jitter 8..12 maps
    // to delay 8..12, then post-jitter clamp caps at max_s=10.
    // Final band: [8, 10].
    for (uint32_t r = 0; r < 1000; r++) {
        uint32_t v = remote_backoff_compute(6, 10, r);
        assert(v >= 8 && v <= 10);
    }

    // Degenerate cap: max_s == 0 forces every delay to 0.
    for (uint8_t a = 1; a <= 8; a++) {
        for (uint32_t r = 0; r < 100; r++) {
            assert(remote_backoff_compute(a, 0, r) == 0);
        }
    }

    printf("remote_backoff_tests: ok\n");
    return 0;
}
