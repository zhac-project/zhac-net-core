// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Host tests for the dgm_store per-device group-membership mirror (pure table
// ops). NVS load/save is a thin wrapper over these (the host NVS stub is a
// no-op, so persistence itself is not unit-tested here).
#include "dgm_store.h"
#include "json_buf.h"
#include <cstdio>
#include <cstring>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

int main() {
    printf("test_dgm_store\n");

    { printf("\nA add / dedup / list\n");
        DgmTable t{};
        CHECK(dgm_tbl_add(t, 0xAA, 101), "add 101 -> changed");
        CHECK(dgm_tbl_add(t, 0xAA, 102), "add 102 -> changed");
        CHECK(!dgm_tbl_add(t, 0xAA, 101), "add duplicate 101 -> no change");
        uint16_t out[16]; uint8_t n = dgm_tbl_list(t, 0xAA, out, 16);
        CHECK(n == 2, "list count == 2");
        CHECK((out[0] == 101 && out[1] == 102) || (out[0] == 102 && out[1] == 101), "both gids present");
        CHECK(dgm_tbl_list(t, 0xBB, out, 16) == 0, "unknown device -> empty list");
    }
    { printf("\nB remove; empty device dropped\n");
        DgmTable t{};
        dgm_tbl_add(t, 0xAA, 101); dgm_tbl_add(t, 0xAA, 102);
        CHECK(dgm_tbl_remove(t, 0xAA, 101), "remove 101 -> changed");
        uint16_t out[16];
        CHECK(dgm_tbl_list(t, 0xAA, out, 16) == 1 && out[0] == 102, "only 102 left");
        CHECK(!dgm_tbl_remove(t, 0xAA, 999), "remove nonexistent gid -> no change");
        CHECK(dgm_tbl_remove(t, 0xAA, 102), "remove last -> changed");
        CHECK(t.n == 0, "device entry dropped when empty");
        CHECK(!dgm_tbl_remove(t, 0xCC, 1), "remove from unknown device -> no change");
    }
    { printf("\nC per-device gid cap\n");
        DgmTable t{};
        for (int i = 0; i < DGM_MAX_GIDS; i++) dgm_tbl_add(t, 0xAA, (uint16_t)(200 + i));
        uint16_t out[DGM_MAX_GIDS];
        CHECK(dgm_tbl_list(t, 0xAA, out, DGM_MAX_GIDS) == DGM_MAX_GIDS, "filled to cap");
        CHECK(!dgm_tbl_add(t, 0xAA, 999), "add past cap -> no change");
        CHECK(dgm_tbl_list(t, 0xAA, out, DGM_MAX_GIDS) == DGM_MAX_GIDS, "still at cap");
    }
    { printf("\nD multi-device independence\n");
        DgmTable t{};
        dgm_tbl_add(t, 0x1, 101); dgm_tbl_add(t, 0x2, 101); dgm_tbl_add(t, 0x2, 102);
        uint16_t out[16];
        CHECK(dgm_tbl_list(t, 0x1, out, 16) == 1, "dev1 has 1");
        CHECK(dgm_tbl_list(t, 0x2, out, 16) == 2, "dev2 has 2 (independent)");
        CHECK(t.n == 2, "2 device entries");
    }
    { printf("\nE set (reconcile) / forget\n");
        DgmTable t{};
        dgm_tbl_add(t, 0xAA, 101);
        uint16_t gids[] = {201, 202, 202, 203};  // dup 202
        CHECK(dgm_tbl_set(t, 0xAA, gids, 4), "set replaces list");
        uint16_t out[16];
        CHECK(dgm_tbl_list(t, 0xAA, out, 16) == 3, "set deduped to 3");
        CHECK(dgm_tbl_set(t, 0xAA, nullptr, 0), "set count 0 -> changed");
        CHECK(t.n == 0, "set 0 dropped the device");
        dgm_tbl_add(t, 0xAA, 101); dgm_tbl_add(t, 0xBB, 1);
        CHECK(dgm_tbl_forget(t, 0xAA), "forget present -> true");
        CHECK(!dgm_tbl_forget(t, 0xAA), "forget absent -> false");
        CHECK(t.n == 1 && dgm_tbl_list(t, 0xBB, out, 16) == 1, "other device intact");
    }

    { printf("\nE dgm_all_json inversion (by gid, ascending, multi-member)\n");
        DgmTable t = {};
        dgm_tbl_add(t, 0xAA, 108); dgm_tbl_add(t, 0xAA, 101);   // AA in 101 + 108
        dgm_tbl_add(t, 0xBB, 101);                              // BB in 101
        char buf[512];
        size_t n = dgm_all_json(t, buf, sizeof buf);
        buf[n] = '\0';   // JsonWriter is length-delimited (no guaranteed NUL); our
                         // oversized test buffer has slack, so terminate it ourselves.
        CHECK(n > 0, "wrote output");
        CHECK(strstr(buf, "\"gid\":101") != nullptr, "has gid 101");
        CHECK(strstr(buf, "\"gid\":108") != nullptr, "has gid 108");
        CHECK(strstr(buf, "\"gid\":101") < strstr(buf, "\"gid\":108"), "gids ascending");
        CHECK(strstr(buf, "0x00000000000000aa") != nullptr, "member AA present");
        CHECK(strstr(buf, "0x00000000000000bb") != nullptr, "member BB present");
        CHECK(strstr(buf, "{\"gid\":108,\"members\":[{\"ieee\":\"0x00000000000000aa\"}]}") != nullptr,
              "gid 108 has exactly member AA");
    }
    { printf("\nF dgm_all_json empty table\n");
        DgmTable t = {};
        char buf[64];
        size_t n = dgm_all_json(t, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "{\"groups\":[]}") == 0, "empty table -> {\"groups\":[]}");
    }

    { printf("\nG dgm_all_json distinct-gid cap\n");
        DgmTable t = {};
        // Fill with far more distinct gids than DGM_MAX_GROUPS (64 devs x 16 gids
        // = 1024 distinct), then expect the output capped to exactly DGM_MAX_GROUPS.
        uint16_t g = 1;
        for (uint8_t d = 0; d < DGM_MAX_DEVS; d++)
            for (uint8_t k = 0; k < DGM_MAX_GIDS; k++)
                dgm_tbl_add(t, (uint64_t)(0x1000 + d), g++);
        char buf[8192];
        size_t n = dgm_all_json(t, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(n > 0, "cap: wrote output (fit in buffer)");
        int cnt = 0; for (const char* p = buf; (p = strstr(p, "\"gid\":")) != nullptr; p++) cnt++;
        CHECK(cnt == DGM_MAX_GROUPS, "capped at DGM_MAX_GROUPS distinct groups");
    }

    printf("\n%s - %d failure(s)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail ? 1 : 0;
}
