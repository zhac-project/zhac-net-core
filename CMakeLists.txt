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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zhac-main-core)
