# HOJA-LIB-DONGLE - USB HID input report builder and descriptor data for embedded gamepads using HOJA LIB Dongle definitions.
# Copyright (c) 2026 Hand Held Legend, LLC. Author: Mitchell Cairns. SPDX-License-Identifier: MIT-0
#
# Portable C: no SDK or chip-specific link dependencies. Link your toolchain's C library when you
# build the final executable or wrapper library.

add_library(hoja_lib_dongle STATIC
    ${CMAKE_CURRENT_LIST_DIR}/dongle.c
    ${CMAKE_CURRENT_LIST_DIR}/dongle_gamepad.c
    ${CMAKE_CURRENT_LIST_DIR}/dongle_host.c
)

target_include_directories(hoja_lib_dongle PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
)

set_target_properties(hoja_lib_dongle PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# Debug configuration. A consuming project can override these from its own
# CMakeLists.txt simply by setting the variables BEFORE add_subdirectory(), e.g.
#   set(DONGLE_LIB_DEBUG_GAMEPADSTATS 1)
#   add_subdirectory(external/HOJA-LIB-DONGLE)
# or on the command line with -DDONGLE_LIB_DEBUG_GAMEPADSTATS=1.
# Defaults match the header fallbacks (logging on, stats off).
if(NOT DEFINED DONGLE_LIB_DEBUG_LOG)
    set(DONGLE_LIB_DEBUG_LOG 1)
endif()
if(NOT DEFINED DONGLE_LIB_DEBUG_GAMEPADSTATS)
    set(DONGLE_LIB_DEBUG_GAMEPADSTATS 0)
endif()

# PUBLIC so the definitions apply to the library's own sources (where they are
# actually used). Definitions set on a consuming executable target do NOT reach
# these sources, since the library is compiled as its own target.
target_compile_definitions(hoja_lib_dongle PUBLIC
    DONGLE_LIB_DEBUG_LOG=${DONGLE_LIB_DEBUG_LOG}
    DONGLE_LIB_DEBUG_GAMEPADSTATS=${DONGLE_LIB_DEBUG_GAMEPADSTATS}
)
