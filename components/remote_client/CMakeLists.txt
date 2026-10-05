# SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# IDF runs this file twice: once for early dependency expansion (when
# CONFIG_* variables are still empty) and again after Kconfig has been
# resolved. Both invocations must use a SINGLE idf_component_register()
# call with identical REQUIRES/PRIV_REQUIRES; otherwise the dependency
# graph baked at expansion time misses the deps and source compilation
# fails with "X.h: No such file" (the include dirs of PRIV_REQUIRES are
# never wired up).
#
# So: switch the SRCS list, not the call itself. When Kconfig is off the
# SRCS list holds only the two pure files named below (remote_allow.cpp,
# remote_xfer.cpp); the client, state machine, backoff and NVS layer are
# left out. The public header in include/ uses inline no-op stubs when the
# Kconfig is off, so callers compile clean.

# remote_allow.cpp and remote_xfer.cpp are pure and the wired dispatcher (zhac-wired-core ws_bridge.cpp)
# calls them too -- the script-name check, the staging slot, the page cutter -- so they build in both
# Kconfig states.
set(remote_srcs "remote_allow.cpp" "remote_xfer.cpp")
if(CONFIG_ZHAC_REMOTE_CLIENT_ENABLE)
    list(APPEND remote_srcs
        "remote_client.cpp"
        "remote_state.cpp"
        "remote_backoff.cpp"
        "remote_nvs.cpp")
endif()

idf_component_register(
    SRCS ${remote_srcs}
    INCLUDE_DIRS "include"
    # REQUIRES = exposed via the public header (remote_client.h). Keep
    # minimal: only types/macros that show up in include/.
    REQUIRES esp_common
    # PRIV_REQUIRES = used only by .cpp sources of this component. IDF 6
    # is strict about cross-component header reachability: a #include
    # resolving via REQUIRES from another component is rejected here,
    # the dependency must be private.
    PRIV_REQUIRES esp_websocket_client
                  esp_app_format   # esp_app_get_description: fw_version in the auth frame
                  mbedtls esp-tls
                  nvs_flash
                  ws_server
                  zap_common
                  hap_json
                  metrics
                  arduinojson
                  esp_event esp_wifi esp_netif
                  freertos
)
