# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# A desktop computer for the end-to-end run against the real server (run.mjs), development only.
add_executable(redvault_v3_probe main.cpp)
target_link_libraries(redvault_v3_probe PRIVATE redvault_e2ev3 redvault_core)

if(MSVC)
    target_compile_options(redvault_v3_probe PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_v3_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()
