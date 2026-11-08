# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

cmake_minimum_required(VERSION 3.24)

project(RedvaultDesktop VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REDVAULT_BUILD_TESTS "Build the core test suite" ON)
option(REDVAULT_BUILD_CLI "Build redvault_cli, a command-line check against a real server" ON)
option(REDVAULT_BUILD_APP "Build the Qt desktop app (needs Qt 6.5+)" ON)
option(REDVAULT_BUILD_TOOLS "Build redvault_release, which signs update manifests" ON)
option(REDVAULT_BUILD_CALLS "Build voice/video calls (libdatachannel, Opus, VP8) and group calls (LiveKit)" ON)
# Tor by default (docs/tor-by-default.md): one app, and everything except calls goes through
# the embedded Tor gateway (tor-core, Arti) to Redvault's onion service. REDVAULT_TOR embeds
# the gateway; REDVAULT_TOR_ONLY makes the onion service the only server (any other address
# is refused) at REDVAULT_ONION_API_BASE. A release build (REDVAULT_RELEASE_BUILD: the release
# preset and every packaging script) refuses to configure without both, at the official onion.
# Only a developer's local test build may set REDVAULT_TOR_ONLY=OFF (cmake/tor_settings.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tor_settings.cmake)
option(REDVAULT_TOR "Embed the Tor gateway (tor-core, needs Rust/cargo)" ON)
option(REDVAULT_TOR_ONLY "Only ever talk to the server through its Tor onion service (OFF: a local test build only)" ON)
set(REDVAULT_ONION_API_BASE "${REDVAULT_OFFICIAL_ONION_API_BASE}" CACHE STRING "The onion service's API base, http://<56 chars>.onion/redvaultmessages/api/")
option(REDVAULT_RELEASE_BUILD "A build to publish: Tor-only at the official onion service, or the configure step stops" OFF)
# Per-device encryption keys (docs/multi-device-threat-model.md): compiles crypto-core into
# the app. It is used when the server enables protocol 3 (/api/e2e/v3/config); messages
# then use device keys by default (docs/signal-integration/07-step1-default-path.md).
option(REDVAULT_E2E_V3 "Build per-device encryption keys (protocol 3): needs Rust/cargo" ON)
# rv-signal-v1 (docs/signal-integration/): links libsignal (AGPL-3.0-only, like this app).
# Release builds carry it; turn it off only for a test build.
option(REDVAULT_E2E_SIGNAL "Build Signal sessions (libsignal, AGPL-3.0-only) into the device keys" ON)
if(REDVAULT_E2E_SIGNAL AND NOT REDVAULT_E2E_V3)
    message(FATAL_ERROR "REDVAULT_E2E_SIGNAL needs REDVAULT_E2E_V3=ON")
endif()
# Where this version's source code is published (Settings > About, "Open-Source Licenses and Source Code").
# The app is under AGPL-3.0-only, so a published build should link to its source. Empty means not published
# yet: the app then says so and shows no link. "{version}" in the URL is replaced by the app version.
set(REDVAULT_SOURCE_CODE_URL "" CACHE STRING "https:// URL of this version's source code (empty: not published yet)")
# Where to report a problem getting the source. Empty: the app points to the contact address in the Terms of Service.
set(REDVAULT_SOURCE_CONTACT "" CACHE STRING "Email address or https:// URL for source-code requests (empty: the Terms of Service contact)")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/source_info.cmake)
redvault_check_source_settings("${REDVAULT_SOURCE_CODE_URL}" "${REDVAULT_SOURCE_CONTACT}")
redvault_check_tor_settings("${REDVAULT_TOR}" "${REDVAULT_TOR_ONLY}" "${REDVAULT_ONION_API_BASE}" "${REDVAULT_RELEASE_BUILD}")
if(NOT REDVAULT_TOR_ONLY)
    message(WARNING "REDVAULT_TOR_ONLY=OFF: a local test build that may talk to a server outside Tor. Never publish it "
                    "(the release preset and the packaging scripts refuse this setting).")
endif()

# libsodium: vcpkg ships a CMake config, MSYS2/Linux/Homebrew ship pkg-config.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    add_library(redvault_sodium ALIAS unofficial-sodium::sodium)
else()
    find_package(PkgConfig REQUIRED)
    # CVE-2025-69277 (crypto_core_ed25519_is_valid_point) is fixed upstream from 1.0.21. MSYS2 packages that, so
    # Windows requires it. Debian and Ubuntu keep the version at 1.0.18 and backport fixes under it, so on Linux
    # the number can't show either way. This code never calls that function; see README.md.
    if(WIN32)
        set(_redvault_sodium_minimum 1.0.21)
    else()
        set(_redvault_sodium_minimum 1.0.18)
    endif()
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=${_redvault_sodium_minimum})
    message(STATUS "libsodium ${SODIUM_VERSION} (minimum ${_redvault_sodium_minimum})")
    add_library(redvault_sodium ALIAS PkgConfig::SODIUM)
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_subdirectory(core)

if(REDVAULT_E2E_V3)
    add_subdirectory(e2ev3)
endif()

if(REDVAULT_TOR)
    add_subdirectory(tor)
endif()

if(REDVAULT_BUILD_CALLS)
    add_subdirectory(rtc)
    add_subdirectory(groupcall)
endif()

if(REDVAULT_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(REDVAULT_BUILD_TOOLS)
    add_subdirectory(tools/release)
    if(REDVAULT_E2E_V3)
        add_subdirectory(tools/v3_probe)
    endif()
    if(REDVAULT_TOR)
        add_subdirectory(tools/tor_routing_check)
    endif()
    if(REDVAULT_BUILD_CALLS)
        add_subdirectory(tools/rtc_interop)
        add_subdirectory(tools/groupcall_probe)
        add_subdirectory(tools/groupcall_e2e)
    endif()
endif()

if(REDVAULT_BUILD_APP)
    find_package(Qt6 6.5 COMPONENTS Quick QuickControls2 Widgets Svg Multimedia)
    if(Qt6_FOUND AND NOT REDVAULT_BUILD_CALLS)
        message(FATAL_ERROR "The desktop app includes calls: configure with REDVAULT_BUILD_CALLS=ON")
    endif()
    if(Qt6_FOUND)
        add_subdirectory(app)
    else()
        message(STATUS "Qt 6.5+ not found: skipping the desktop app (the core, CLI and tests still build)")
    endif()
endif()

if(REDVAULT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
