# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
idf_component_register(
    SRCS "rainmaker_gw.c" "rmk_typemap.c" "rmk_store.c" "rmk_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash zap_store
)

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
