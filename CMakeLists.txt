# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)

# Resolve shared-components dir (zhac-components/components). Three paths:
#   1. $IDF_COMPONENT_OVERRIDE_PATH  (set by platform justfile, or manually).
#   2. ../zhac-components/components (split: sibling or platform submodule).
#   3. ../../components              (legacy monorepo layout).
# ESP Component Manager also resolves the shared deps via main/idf_component.yml.
if(DEFINED ENV{IDF_COMPONENT_OVERRIDE_PATH})
    set(EXTRA_COMPONENT_DIRS "$ENV{IDF_COMPONENT_OVERRIDE_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../zhac-components/components")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../zhac-components/components")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../components")
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
else()
    message(FATAL_ERROR "shared components dir not found — set IDF_COMPONENT_OVERRIDE_PATH or clone zhac-components as a sibling")
endif()

# Firmware version baked into esp_app_desc.version (shown in the web UI Info
# block). Derived from git: the release tag on a clean checkout (e.g.
# v2026061501), tag-commits-ghash when ahead, or the bare hash if untagged.
# Must be set before project().
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE ZHAC_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT ZHAC_GIT_VERSION)
    set(ZHAC_GIT_VERSION "unknown")
endif()
set(PROJECT_VER "${ZHAC_GIT_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zhac-net-core)

# cbor/Picolibc compile fix, required by the RainMaker managed dependency.
# Gated on CONFIG_ZHAC_RAINMAKER_ENABLE (components/rainmaker_gw/Kconfig;
# default n) so a flag-off build never references managed_components at
# all here — with the flag off, espressif/esp_rainmaker (and therefore
# espressif/cbor) isn't even a resolved dependency: its entry in
# main/idf_component.yml is itself rules-gated on this same Kconfig symbol.
#
# esp_rainmaker -> rmaker_common unconditionally pulls in the registry
# component espressif/cbor ("~0.6" — only one published version has ever
# been released, so there is no alternate version to pin around this).
# That component's OWN CMakeLists.txt force-compiles
# tinycbor/src/open_memstream.c with -D__linux__ — no Kconfig gate, no
# build option. That file's #elif __linux__ branch needs
# fopencookie()/cookie_io_functions_t, a glibc/Newlib-ism that Picolibc
# (this project's libc on IDF v6.0 — switching to Newlib to dodge this is
# not an option we take) does not provide, which is fatal under this
# project's -Werror.
#
# The same file has an #ifdef __APPLE__ branch, checked *before* the
# #elif __linux__ one (so it wins even with __linux__ still also defined),
# that instead calls
# funopen(cookie, NULL, write_to_buffer, NULL, close_buffer) — no
# fopencookie/cookie_io_functions_t needed. ESP-IDF's own
# esp_libc/platform_include/stdio.h supplies a funopen() declaration
# whenever CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY is on (Picolibc's own
# default per the v6.0 migration guide), typed
# `int (*readfn)(void*, char*, int)` / `int (*writefn)(void*, const char*, int)`
# / `int (*closefn)(void*)` — an exact match for write_to_buffer's /
# close_buffer's shapes there. Appending __APPLE__ to just this one file's
# COMPILE_DEFINITIONS redirects it onto that already-existing, compatible
# code path — no new component, no adapter code, nothing else to link.
#
# Must run here, after project(): set_property(SOURCE ... DIRECTORY ...)
# requires DIRECTORY to already be a registered CMake subdirectory (i.e.
# espressif__cbor's own CMakeLists.txt must already have been
# add_subdirectory'd), which every component's is by the time project()
# returns. Component discovery order among siblings isn't guaranteed, so
# this can't safely live in a component's own CMakeLists.txt.
#
# Revisit if espressif/cbor ever ships a version bump: this fails loudly
# at compile time (open_memstream.c reverting to the fopencookie branch),
# never silently.
if(CONFIG_ZHAC_RAINMAKER_ENABLE)
    set(_zhac_cbor_dir "${CMAKE_SOURCE_DIR}/managed_components/espressif__cbor")
    set(_zhac_cbor_memstream_src "${_zhac_cbor_dir}/tinycbor/src/open_memstream.c")

    if(EXISTS "${_zhac_cbor_memstream_src}")
        set_property(
            SOURCE "${_zhac_cbor_memstream_src}"
            DIRECTORY "${_zhac_cbor_dir}"
            APPEND PROPERTY COMPILE_DEFINITIONS
            "__APPLE__"
        )
    else()
        # Not fatal: e.g. the very first configure pass before the
        # component manager has fetched managed_components yet. idf.py
        # build always reconfigures, so a subsequent pass picks this up.
        message(STATUS "rainmaker_gw: ${_zhac_cbor_memstream_src} not found yet, "
                        "skipping open_memstream.c __APPLE__ redirect this pass")
    endif()
endif()
