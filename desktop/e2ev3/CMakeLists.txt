# Per-device encryption keys ("protocol 3", docs/multi-device-threat-model.md):
# the shared engine in crypto-core (Rust, vodozemac) built by cargo with its C
# ABI (feature "ffi") into the build tree and linked statically. Development
# only: the code is compiled in, but nothing uses it unless the server answers
# /e2e/v3/config with enabled=true (E2E_V3_ENABLED, off by default).
#
# Needs Rust (cargo) on the build machine; --locked keeps the dependency
# versions exactly as reviewed (crypto-core/Cargo.lock).

set(REDVAULT_E2E_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../crypto-core" CACHE PATH "crypto-core crate")
find_program(REDVAULT_CARGO cargo
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    REQUIRED)

set(_e2e_target_dir "${CMAKE_BINARY_DIR}/crypto-core-target")
# rv-signal-v1 adds libsignal (AGPL-3.0-only) to the engine: development only (see the top-level option).
if(REDVAULT_E2E_SIGNAL)
    set(_e2e_features "ffi signal")
    message(WARNING "REDVAULT_E2E_SIGNAL is ON: this build links libsignal (AGPL-3.0-only). Do not distribute it until the release gates in docs/legal/REDVAULT_AGPL_COMPLIANCE_REPORT.md are cleared.")
else()
    set(_e2e_features "ffi")
endif()
set(REDVAULT_E2E_LIBRARY "${_e2e_target_dir}/release/libredvault_e2ee.a")

file(GLOB_RECURSE _e2e_sources CONFIGURE_DEPENDS "${REDVAULT_E2E_CORE_DIR}/src/*.rs")
add_custom_command(
    OUTPUT "${REDVAULT_E2E_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${_e2e_target_dir}"
            "${REDVAULT_CARGO}" build --release --locked --lib --features "${_e2e_features}"
            --manifest-path "${REDVAULT_E2E_CORE_DIR}/Cargo.toml"
    DEPENDS ${_e2e_sources} "${REDVAULT_E2E_CORE_DIR}/Cargo.toml" "${REDVAULT_E2E_CORE_DIR}/Cargo.lock"
    COMMENT "Building the device-key engine (crypto-core, vodozemac) with cargo"
    USES_TERMINAL
    VERBATIM)
add_custom_target(redvault_e2e_build DEPENDS "${REDVAULT_E2E_LIBRARY}")

add_library(redvault_e2e STATIC IMPORTED GLOBAL)
set_target_properties(redvault_e2e PROPERTIES
    IMPORTED_LOCATION "${REDVAULT_E2E_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${REDVAULT_E2E_CORE_DIR}/include")
if(WIN32)
    # What a Rust static library needs from the system (the list build-all.sh links the C++ driver with).
    set_property(TARGET redvault_e2e PROPERTY INTERFACE_LINK_LIBRARIES ws2_32 userenv ntdll bcrypt advapi32)
else()
    set_property(TARGET redvault_e2e PROPERTY INTERFACE_LINK_LIBRARIES dl pthread m)
endif()
add_dependencies(redvault_e2e redvault_e2e_build)

# The C++ side: key storage, the server calls and the device logic, a port of
# ui/src/lib/e2eV3. Qt-free, like redvault_core.
add_library(redvault_e2ev3 STATIC
    src/engine.cpp
    src/fs_util.cpp
    src/key_vault.cpp
    src/device_store.cpp
    src/v3_server.cpp
    src/device.cpp
    src/pairing.cpp
    src/chat.cpp
)
target_include_directories(redvault_e2ev3 PUBLIC include)
if(REDVAULT_E2E_SIGNAL)
    target_compile_definitions(redvault_e2ev3 PUBLIC REDVAULT_HAS_E2E_SIGNAL=1)
endif()
target_link_libraries(redvault_e2ev3 PUBLIC redvault_core redvault_e2e)
if(WIN32)
    target_link_libraries(redvault_e2ev3 PRIVATE crypt32)
elseif(UNIX)
    target_link_libraries(redvault_e2ev3 PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
    target_compile_options(redvault_e2ev3 PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_e2ev3 PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
