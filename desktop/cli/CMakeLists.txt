# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

add_executable(redvault_cli main.cpp)
target_link_libraries(redvault_cli PRIVATE redvault_core)

if(MSVC)
    target_compile_options(redvault_cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_cli PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
