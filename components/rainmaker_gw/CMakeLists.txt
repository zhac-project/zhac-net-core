# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# esp_event: rainmaker_gw.c registers handlers on the RainMaker event bases
# directly (esp_event_handler_register) — unconditionally required, it's a
# built-in IDF component, not gated on anything.
idf_component_register(
    SRCS "rainmaker_gw.c" "rmk_typemap.c" "rmk_store.c" "rmk_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash zap_store esp_event
)

# esp_rainmaker (managed dep espressif/esp_rainmaker, registered under
# component name "espressif__esp_rainmaker") is rules-gated in
# main/idf_component.yml: resolved into the build at all only when
# CONFIG_ZHAC_RAINMAKER_ENABLE=y. REQUIRES/PRIV_REQUIRES above can NOT be
# conditioned on a CONFIG_ value — component requirements are expanded in an
# early CMake pass, before Kconfig is loaded (ESP-IDF build-system docs,
# "Component Requirements": "The values of REQUIRES and PRIV_REQUIRES should
# not depend on any configuration options"). Confirmed the hard way first:
# an `if(CONFIG_ZHAC_RAINMAKER_ENABLE) list(APPEND ... esp_rainmaker)
# endif()` ahead of idf_component_register() silently added nothing to the
# resolved reqs (per build/project_description.json) even though
# sdkconfig.cmake already had the flag as "y" — CONFIG_ is simply not
# meaningful yet at that pass. idf_component_optional_requires() (IDF's
# documented answer for this exact "late" case; tools/cmake/component.cmake)
# runs AFTER idf_component_register(), once both Kconfig and the build's
# resolved component list are known, and links the given component only if
# it is actually part of THIS build — a no-op rather than a configure error
# on a flag-off build, where espressif__esp_rainmaker was never resolved at
# all. PRIVATE: no esp_rmaker_* type ever appears in rainmaker_gw.h.
idf_component_optional_requires(PRIVATE espressif__esp_rainmaker)

# rainmaker_gw.c calls zap_store's zhac_uplink_get() to read the runtime
# uplink selector. zap_store.h's include chain (zap_common.h: <cstdint>,
# `enum class`, `static constexpr`) is C++-only syntax, and zap_store.cpp
# defines zhac_uplink_get() with plain (mangled) C++ linkage — no
# `extern "C"` anywhere in that component. A genuine C translation unit can
# neither #include zap_store.h nor link against zhac_uplink_get(). Compiling
# this one source as C++ (despite its .c name, kept so it matches the
# lifecycle-plan's file list) resolves both problems; rainmaker_gw.h's own
# `extern "C"` guard means this component's PUBLIC API (rainmaker_gw_init/
# _state/_node_id) still exports plain C-linkage symbols either way, so
# nothing about the contract callers (main.cpp) see changes.
set_source_files_properties("rainmaker_gw.c" PROPERTIES LANGUAGE CXX)
