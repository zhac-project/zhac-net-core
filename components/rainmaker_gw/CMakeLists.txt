# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# esp_event: rainmaker_gw.c registers handlers on the RainMaker event bases
# directly (esp_event_handler_register) — unconditionally required, it's a
# built-in IDF component, not gated on anything.
#
# event_bus (PRIV_REQUIRES, not REQUIRES): rmk_bridge.c (Task 16) subscribes
# to shadow-change events via event_bus_subscribe()/event_bus_drain_handle().
# PRIV_REQUIRES rather than REQUIRES because event_bus.h is used only inside
# this component's own .c files — no event_bus type (Event/EventType/
# EventHandler/...) ever appears in a public header here (rmk_bridge.h only
# pulls in esp_err.h + rmk_typemap.h) — so there is nothing for a REQUIRES-
# style public propagation to carry to this component's own consumers.
# Caught by the flag-ON build: omitting it produced exactly the fatal error
# ESP-IDF's own component-dependency checker predicts for this situation
# ("rmk_bridge.c ... includes event_bus.h ... event_bus component(s) is not
# in the requirements list of rainmaker_gw ... add event_bus to
# PRIV_REQUIRES") — `event_bus.h: No such file or directory` on the first
# build attempt, fixed here on the second.
idf_component_register(
    SRCS "rainmaker_gw.c" "rmk_typemap.c" "rmk_store.c" "rmk_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash zap_store esp_event
    PRIV_REQUIRES event_bus
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

# rmk_bridge.c (Task 16) subscribes directly to event_bus for shadow-change
# events. event_bus.h (components/event_bus/include/event_bus.h) is
# C++-only syntax for the same reason as zap_store.h above (`enum class
# EventType`, `std::function`-typed EventHandler) — a genuine C translation
# unit can neither #include it nor build the EventHandler it wants.
# Same fix, same non-effect on the public contract: rmk_bridge.h's own
# `extern "C"` guard keeps this file's exported symbols
# (rmk_bridge_expose_device/_unexpose_device/_on_device_gone/_attach) at
# plain C linkage regardless of which language actually compiled the file.
set_source_files_properties("rmk_bridge.c" PROPERTIES LANGUAGE CXX)
