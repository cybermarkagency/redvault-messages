# The desktop group-call test participant (desktop/tools/groupcall_e2e) built on its own for Linux: the core, the
# group-call wrapper around LiveKit's prebuilt liblivekit_ffi.so (downloaded and SHA-256 checked by groupcall's
# CMakeLists) and the tool, without the Qt app or the 1:1 call engine (libdatachannel).
#   cmake -S groupcall-e2e -B build -G Ninja   (core-build.sh's environment; see groupcall-e2e-build.sh)
cmake_minimum_required(VERSION 3.24)
project(RedvaultGroupcallE2eLinux LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DESKTOP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../desktop)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
add_library(redvault_sodium ALIAS PkgConfig::SODIUM)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

add_subdirectory(${DESKTOP_DIR}/core desktop-core)
add_subdirectory(${DESKTOP_DIR}/groupcall desktop-groupcall)

add_executable(redvault_groupcall_e2e ${DESKTOP_DIR}/tools/groupcall_e2e/main.cpp)
target_link_libraries(redvault_groupcall_e2e PRIVATE redvault_groupcall redvault_core)
redvault_copy_livekit_ffi(redvault_groupcall_e2e)
