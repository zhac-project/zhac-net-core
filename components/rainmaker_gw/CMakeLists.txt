# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# esp_event: rainmaker_gw.c registers handlers on the RainMaker event bases
# directly (esp_event_handler_register) — unconditionally required, it's a
# built-in IDF component, not gated on anything.
#
# NOTE (Task 16 history): an earlier cut of rmk_bridge.c subscribed to
# event_bus and needed a PRIV_REQUIRES event_bus here (added when the
# flag-ON build's own component-dependency checker caught the missing
# `#include "event_bus.h"` path). Code review then proved the event_bus
# subscription could never fire on this target — device_shadow (the only
# producer of the events it subscribed to) is never initialized or fed
# anywhere in net-core — so the fix commit rehooked rmk_bridge.c at
# main/hap_bridge.cpp's direct HapMsgType::BULK_STATE_UPDATE fan-out
# instead (see rmk_bridge.c's own file banner) and dropped the event_bus
# dependency along with the subscription/drain-task code that needed it.
# zcl_attribute.h (ValType, still needed by rmk_bridge.c) is reachable via
# zap_store's own public REQUIRES on zap_common below — no replacement
# dependency needed here.
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

# rmaker_work_queue (Task 21 fix): rmk_bridge.c's OUT-direction report now
# defers its one blocking esp_rmaker_* SDK call to the RainMaker work-queue
# task via esp_rmaker_work_queue_add_task() instead of running it inline on
# task_hap (see rmk_bridge_on_attr_update's own comment for the full
# rationale). esp_rainmaker itself already depends on this component
# transitively, but declaring it directly here — same
# idf_component_optional_requires PRIVATE pattern as the line above, for
# the same Kconfig-gated-resolution reason — is the correct/explicit thing
# to depend on what is actually #included, not what happens to be reachable
# through another component's own dependency graph.
idf_component_optional_requires(PRIVATE espressif__rmaker_work_queue)

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

# rmk_bridge.c (Task 16) needs zcl_attribute.h (zap_common) for the ValType
# enum (VAL_INT/VAL_BOOL/VAL_STR/VAL_FLOAT/VAL_NONE) used by its OUT
# handler — that header is C++-only syntax for the same reason as
# zap_store.h above (`#include <cstdint>` etc.), independent of whichever
# specific feature pulls it in (this file no longer needs event_bus.h for
# it — see the NOTE above — but zcl_attribute.h alone already forces the
# issue). The file's own code (bare `RmkBridgeDev`/`RmkBridgeParam`
# struct-as-type-name without a typedef, `nullptr`) is C++-only regardless
# of any header. Same fix, same non-effect on the public contract:
# rmk_bridge.h's own `extern "C"` guard keeps this file's exported symbols
# (rmk_bridge_expose_device/_unexpose_device/_on_device_gone/_active/
# _on_attr_update/_attach) at plain C linkage regardless of which language
# actually compiled the file.
set_source_files_properties("rmk_bridge.c" PROPERTIES LANGUAGE CXX)
