# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Tor by default (docs/tor-by-default.md): checks REDVAULT_TOR, REDVAULT_TOR_ONLY, REDVAULT_ONION_API_BASE and
# REDVAULT_RELEASE_BUILD (see the top-level CMakeLists.txt). A release build (the release preset and every
# packaging script set REDVAULT_RELEASE_BUILD) is Tor-only at the official onion service, or the configure
# step stops. Also runs as a script, for the tests:
#   cmake -DREDVAULT_TOR=ON -DREDVAULT_TOR_ONLY=ON -DREDVAULT_ONION_API_BASE=... -DREDVAULT_RELEASE_BUILD=ON -P tor_settings.cmake

# The live server's onion service (GET /api/public/routing on the website answers it).
set(REDVAULT_OFFICIAL_ONION_API_BASE "http://4cydjhdc3lmhg7l6ykjont7dms2alunglmnlznlmjjwkyc67m5h325yd.onion/redvaultmessages/api/")

function(redvault_check_tor_settings tor tor_only onion_base release)
    if(tor_only AND NOT tor)
        message(FATAL_ERROR "REDVAULT_TOR_ONLY needs REDVAULT_TOR (the embedded Tor gateway)")
    endif()
    # CMake regexes have no {n}: a v3 onion name is 56 base32 characters.
    set(_onion_name "")
    if(onion_base MATCHES "^http://([a-z2-7]+)\\.onion/")
        set(_onion_name "${CMAKE_MATCH_1}")
    endif()
    string(LENGTH "${_onion_name}" _onion_length)
    if(tor_only AND NOT _onion_length EQUAL 56)
        message(FATAL_ERROR "REDVAULT_TOR_ONLY needs REDVAULT_ONION_API_BASE=http://<v3 onion>/redvaultmessages/api/. Got: \"${onion_base}\"")
    endif()
    if(release)
        if(NOT tor OR NOT tor_only)
            message(FATAL_ERROR "A release build always goes through Tor: REDVAULT_TOR and REDVAULT_TOR_ONLY must be ON "
                                "(REDVAULT_TOR_ONLY=OFF is for a local test build only)")
        endif()
        if(NOT onion_base STREQUAL REDVAULT_OFFICIAL_ONION_API_BASE)
            message(FATAL_ERROR "A release build talks only to ${REDVAULT_OFFICIAL_ONION_API_BASE}. Got: \"${onion_base}\"")
        endif()
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    redvault_check_tor_settings("${REDVAULT_TOR}" "${REDVAULT_TOR_ONLY}" "${REDVAULT_ONION_API_BASE}" "${REDVAULT_RELEASE_BUILD}")
    message(STATUS "Tor settings accepted")
endif()
