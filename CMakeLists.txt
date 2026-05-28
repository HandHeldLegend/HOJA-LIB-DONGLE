# HOJA-LIB-DONGLE - USB HID input report builder and descriptor data for embedded gamepads using HOJA LIB Dongle definitions.
# Copyright (c) 2026 Hand Held Legend, LLC. Author: Mitchell Cairns. SPDX-License-Identifier: MIT-0
#
# Portable C: no SDK or chip-specific link dependencies. Link your toolchain's C library when you
# build the final executable or wrapper library.

add_library(hoja_lib_dongle STATIC
    ${CMAKE_CURRENT_LIST_DIR}/dongle.c
)

target_include_directories(hoja_lib_dongle PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
)

set_target_properties(hoja_lib_dongle PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
