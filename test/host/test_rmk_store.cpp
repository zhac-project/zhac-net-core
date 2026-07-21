// SPDX-License-Identifier: AGPL-3.0-or-later
// Host tests for the rmk_store RainMaker exposed-device-set (pure table
// ops). NVS load/save is a thin wrapper over these (the host NVS stub is a
// no-op, so persistence itself is not unit-tested here).
#include "rmk_store.h"
#include <cstdio>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

int main() {
    printf("test_rmk_store\n");

    { printf("\nA add / dedup\n");
        RmkTable t{};
        CHECK(rmk_tbl_add(&t, 0xAA), "add 0xAA -> true");
        CHECK(rmk_tbl_add(&t, 0xBB), "add 0xBB -> true");
        CHECK(rmk_tbl_add(&t, 0xAA), "add duplicate 0xAA -> true (dedup)");
        CHECK(t.count == 2, "count == 2 after dedup (no growth)");
    }
    { printf("\nB cap at RMK_MAX_DEVS\n");
        RmkTable t{};
        for (size_t i = 0; i < RMK_MAX_DEVS; i++) rmk_tbl_add(&t, 0x1000 + i);
        CHECK(t.count == RMK_MAX_DEVS, "filled to cap");
        CHECK(!rmk_tbl_add(&t, 0x9999), "add past cap -> false");
        CHECK(t.count == RMK_MAX_DEVS, "count unchanged past cap");
    }
    { printf("\nC remove\n");
        RmkTable t{};
        rmk_tbl_add(&t, 0xAA); rmk_tbl_add(&t, 0xBB);
        CHECK(rmk_tbl_remove(&t, 0xAA), "remove present -> true");
        CHECK(t.count == 1, "count decremented");
        CHECK(!rmk_tbl_remove(&t, 0xAA), "remove already-gone -> false");
        CHECK(!rmk_tbl_remove(&t, 0xCC), "remove never-added -> false");
    }
    { printf("\nD contains\n");
        RmkTable t{};
        rmk_tbl_add(&t, 0xAA);
        CHECK(rmk_tbl_contains(&t, 0xAA), "contains present -> true");
        CHECK(!rmk_tbl_contains(&t, 0xBB), "contains absent -> false");
    }
    { printf("\nE reconcile\n");
        RmkTable t{};
        rmk_tbl_add(&t, 0xA); rmk_tbl_add(&t, 0xB); rmk_tbl_add(&t, 0xC);
        uint64_t live_ac[] = {0xA, 0xC};
        CHECK(rmk_tbl_reconcile(&t, live_ac, 2) == 1, "{A,B,C} vs live{A,C} drops B -> returns 1");
        CHECK(t.count == 2 && rmk_tbl_contains(&t,0xA) && rmk_tbl_contains(&t,0xC) && !rmk_tbl_contains(&t,0xB),
              "only A,C remain");
        CHECK(rmk_tbl_reconcile(&t, live_ac, 2) == 0, "nothing dead -> returns 0");
        CHECK(rmk_tbl_reconcile(&t, nullptr, 0) == 2, "empty live list drops all -> returns 2");
        CHECK(t.count == 0, "table empty after full reconcile");
    }

    printf("\n%s - %d failure(s)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail ? 1 : 0;
}
