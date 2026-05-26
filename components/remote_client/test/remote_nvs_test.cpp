// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_nvs.h"
#include "remote_nvs_kv.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    kv_reset_for_test();

    // 1. Fresh load: enabled=false, url+token empty, devid defaulted.
    {
        bool en = true;
        char url[REMOTE_NVS_URL_MAX]    = "uninit";
        char tok[REMOTE_NVS_TOKEN_MAX]  = "uninit";
        char did[REMOTE_NVS_DEVID_MAX]  = "uninit";
        assert(remote_nvs_load(&en, url, sizeof(url),
                                tok, sizeof(tok),
                                did, sizeof(did)));
        assert(en == false);
        assert(url[0] == 0);
        assert(tok[0] == 0);
        assert(std::strcmp(did, "TEST123456") == 0);
    }

    // 2. Save + reload round-trip.
    {
        assert(remote_nvs_save(true,
                               "wss://example.com/v1",
                               "abcdef0123456789",
                               "MYDEV"));
        bool en = false;
        char url[REMOTE_NVS_URL_MAX]    = "";
        char tok[REMOTE_NVS_TOKEN_MAX]  = "";
        char did[REMOTE_NVS_DEVID_MAX]  = "";
        assert(remote_nvs_load(&en, url, sizeof(url),
                                tok, sizeof(tok),
                                did, sizeof(did)));
        assert(en == true);
        assert(std::strcmp(url, "wss://example.com/v1")  == 0);
        assert(std::strcmp(tok, "abcdef0123456789")      == 0);
        assert(std::strcmp(did, "MYDEV")                 == 0);
    }

    // 3. Partial save preserves prior values (empty strings = keep).
    {
        assert(remote_nvs_save(false, "", "", ""));
        bool en = true;
        char url[REMOTE_NVS_URL_MAX]    = "";
        char tok[REMOTE_NVS_TOKEN_MAX]  = "";
        char did[REMOTE_NVS_DEVID_MAX]  = "";
        assert(remote_nvs_load(&en, url, sizeof(url),
                                tok, sizeof(tok),
                                did, sizeof(did)));
        assert(en == false);                                   // flipped
        assert(std::strcmp(url, "wss://example.com/v1") == 0); // preserved
        assert(std::strcmp(tok, "abcdef0123456789")     == 0); // preserved
        assert(std::strcmp(did, "MYDEV")                == 0); // preserved
    }

    // 4. forget_creds clears url+token, keeps enabled flag.
    {
        assert(remote_nvs_save(true, "wss://x/y", "tok2", ""));
        assert(remote_nvs_forget_creds());
        bool en = false;
        char url[REMOTE_NVS_URL_MAX]    = "";
        char tok[REMOTE_NVS_TOKEN_MAX]  = "";
        char did[REMOTE_NVS_DEVID_MAX]  = "";
        assert(remote_nvs_load(&en, url, sizeof(url),
                                tok, sizeof(tok),
                                did, sizeof(did)));
        assert(en == true);   // enabled survived
        assert(url[0] == 0);  // erased
        assert(tok[0] == 0);  // erased
    }

    printf("remote_nvs_tests: ok\n");
    return 0;
}
