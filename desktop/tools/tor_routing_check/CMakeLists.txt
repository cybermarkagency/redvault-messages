# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Development only (docs/tor-by-default.md, "Live check"): one anonymous GET of <onion API base>public/routing
# through the embedded Tor gateway and the app's own Tor-only transport.
add_executable(redvault_tor_routing_check main.cpp)
target_link_libraries(redvault_tor_routing_check PRIVATE redvault_core redvault_tor nlohmann_json::nlohmann_json)
target_compile_definitions(redvault_tor_routing_check PRIVATE REDVAULT_ONION_API_BASE="${REDVAULT_ONION_API_BASE}")
redvault_copy_tor(redvault_tor_routing_check)

if(MSVC)
    target_compile_options(redvault_tor_routing_check PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_tor_routing_check PRIVATE -Wall -Wextra -Wpedantic)
endif()
