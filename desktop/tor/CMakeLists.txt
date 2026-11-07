# Tor-only messaging (docs/tor-routing-plan.md): tor-core (Rust, Arti) built
# by cargo into the build tree and linked as a shared library, the same way
# on Windows (redvault_tor.dll next to the program) and Linux
# (libredvault_tor.so in lib/, which the AppImage carries).
#
# Always the release profile: Arti's directory checks are far too slow
# unoptimized. Needs Rust (cargo) on the build machine; --locked keeps the
# dependency versions exactly as reviewed (tor-core/Cargo.lock), including
# the patched saturating-time (tor-core/patches, see PATCHED.md).

set(REDVAULT_TOR_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tor-core" CACHE PATH "tor-core crate")
find_program(REDVAULT_CARGO cargo
    HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    REQUIRED)

set(_tor_target_dir "${CMAKE_BINARY_DIR}/tor-core-target")
set(_tor_out "${_tor_target_dir}/release")
if(WIN32)
    set(REDVAULT_TOR_LIBRARY "${_tor_out}/redvault_tor.dll" CACHE INTERNAL "")
    set(_tor_implib "${_tor_out}/libredvault_tor.dll.a")
    set(_tor_outputs "${REDVAULT_TOR_LIBRARY}" "${_tor_implib}")
else()
    set(REDVAULT_TOR_LIBRARY "${_tor_out}/libredvault_tor.so" CACHE INTERNAL "")
    set(_tor_outputs "${REDVAULT_TOR_LIBRARY}")
endif()

file(GLOB_RECURSE _tor_sources CONFIGURE_DEPENDS
    "${REDVAULT_TOR_CORE_DIR}/src/*.rs"
    "${REDVAULT_TOR_CORE_DIR}/patches/*.rs")
add_custom_command(
    OUTPUT ${_tor_outputs}
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${_tor_target_dir}"
            "${REDVAULT_CARGO}" build --release --locked --lib --features ffi
            --manifest-path "${REDVAULT_TOR_CORE_DIR}/Cargo.toml"
    DEPENDS ${_tor_sources} "${REDVAULT_TOR_CORE_DIR}/Cargo.toml" "${REDVAULT_TOR_CORE_DIR}/Cargo.lock"
    COMMENT "Building the Tor gateway (tor-core, Arti) with cargo"
    USES_TERMINAL
    VERBATIM)
add_custom_target(redvault_tor_build DEPENDS ${_tor_outputs})

add_library(redvault_tor SHARED IMPORTED GLOBAL)
if(WIN32)
    set_target_properties(redvault_tor PROPERTIES IMPORTED_LOCATION "${REDVAULT_TOR_LIBRARY}" IMPORTED_IMPLIB "${_tor_implib}")
else()
    set_target_properties(redvault_tor PROPERTIES IMPORTED_LOCATION "${REDVAULT_TOR_LIBRARY}" IMPORTED_NO_SONAME TRUE)
endif()
set_target_properties(redvault_tor PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${REDVAULT_TOR_CORE_DIR}/include")
add_dependencies(redvault_tor redvault_tor_build)

# Copies the library next to a built program (Windows looks there; Linux builds use the rpath below).
function(redvault_copy_tor target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REDVAULT_TOR_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    if(UNIX AND NOT APPLE)
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN" "${_tor_out}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
    endif()
endfunction()
