# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Release tooling: the offline signing key and update manifests (see main.cpp).
add_executable(redvault_release main.cpp)
target_link_libraries(redvault_release PRIVATE redvault_core)

if(MSVC)
    target_compile_options(redvault_release PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_release PRIVATE -Wall -Wextra -Wpedantic)
endif()
