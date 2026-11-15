# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Development only (docs/tor-sealed-fallback.md, "Live"): the sealed backup route over the real Tor network, with the
# app's own transports, an onion that can't answer and the gateway's pinned key. One anonymous GET; no account.
add_executable(redvault_sealed_route_check main.cpp)
target_link_libraries(redvault_sealed_route_check PRIVATE redvault_e2ev3 redvault_core redvault_tor nlohmann_json::nlohmann_json)
redvault_copy_tor(redvault_sealed_route_check)

if(MSVC)
    target_compile_options(redvault_sealed_route_check PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_sealed_route_check PRIVATE -Wall -Wextra -Wpedantic)
endif()
