# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Proof of concept only: the desktop core (desktop/core, unchanged) over the
# embedded Tor gateway. Builds the core in its own binary dir, so the desktop
# app's build is untouched.
#
#   cmake -S . -B build -G Ninja -DRV_TOR_LIB_DIR=<cargo target>/debug
#   cmake --build build
cmake_minimum_required(VERSION 3.24)
project(RedvaultTorPoc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DESKTOP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../desktop)
set(RV_TOR_LIB_DIR "" CACHE PATH "Cargo output dir holding the redvault_tor library")
if(NOT RV_TOR_LIB_DIR)
    message(FATAL_ERROR "Set RV_TOR_LIB_DIR to tor-core/target/debug (or release)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
add_library(redvault_sodium ALIAS PkgConfig::SODIUM)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_subdirectory(${DESKTOP_DIR}/core desktop-core)

add_library(redvault_tor SHARED IMPORTED)
if(WIN32)
    set_target_properties(redvault_tor PROPERTIES
        IMPORTED_LOCATION ${RV_TOR_LIB_DIR}/redvault_tor.dll
        IMPORTED_IMPLIB ${RV_TOR_LIB_DIR}/libredvault_tor.dll.a)
else()
    set_target_properties(redvault_tor PROPERTIES IMPORTED_LOCATION ${RV_TOR_LIB_DIR}/libredvault_tor.so)
endif()
set_target_properties(redvault_tor PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

add_executable(tor_e2e_driver tor_e2e_driver.cpp tor_curl_transport.cpp)
target_link_libraries(tor_e2e_driver PRIVATE redvault_core redvault_tor CURL::libcurl)
if(WIN32)
    target_link_libraries(tor_e2e_driver PRIVATE ws2_32)
    add_custom_command(TARGET tor_e2e_driver POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RV_TOR_LIB_DIR}/redvault_tor.dll $<TARGET_FILE_DIR:tor_e2e_driver>)
else()
    set_target_properties(tor_e2e_driver PROPERTIES BUILD_RPATH ${RV_TOR_LIB_DIR})
endif()
