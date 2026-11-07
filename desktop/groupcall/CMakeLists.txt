# Group calls: LiveKit's prebuilt native client library (livekit-ffi), loaded
# at run time and driven with its protobuf protocol.
#
# LiveKit's C++ SDK needs MSVC, and this app is built with MinGW. The FFI
# library has a plain C interface, so it doesn't matter what built it:
#   - the binary comes from LiveKit's release, pinned by version and SHA-256;
#   - the protocol (.proto files) is vendored from the same release tag in
#     third_party/livekit-ffi/protocol and compiled with protoc;
#   - nothing links against the binary: groupcall/src/ffi_library.cpp opens it
#     with LoadLibrary / dlopen, so the app starts without it.
#
# Offline builds: put the release's zip anywhere and pass
#   -DREDVAULT_LIVEKIT_FFI_ZIP=/path/to/ffi-<os>-x86_64.zip
# (its SHA-256 is still checked).

set(REDVAULT_LIVEKIT_FFI_VERSION "0.12.83")
set(REDVAULT_LIVEKIT_FFI_URL_BASE
    "https://github.com/livekit/rust-sdks/releases/download/livekit-ffi/v${REDVAULT_LIVEKIT_FFI_VERSION}")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _redvault_arch)
if(NOT _redvault_arch MATCHES "^(amd64|x86_64|x64)$")
    message(FATAL_ERROR "Group calls ship LiveKit's library for x86-64 only (this is ${CMAKE_SYSTEM_PROCESSOR}).")
endif()
if(WIN32)
    set(_redvault_ffi_asset "ffi-windows-x86_64.zip")
    set(_redvault_ffi_sha256 "518b6f6952b440f11f6c1c5e36e5dad68de249e29a76b77c81eda82579634308")
    set(_redvault_ffi_file "livekit_ffi.dll")
elseif(UNIX AND NOT APPLE)
    set(_redvault_ffi_asset "ffi-linux-x86_64.zip")
    set(_redvault_ffi_sha256 "de56c227dc62d4b5a6cf58453ba76cc6fd7847e4e740daad3b79e91c54ed3f7b")
    set(_redvault_ffi_file "liblivekit_ffi.so")
else()
    message(FATAL_ERROR "Group calls are built for Windows and Linux.")
endif()

set(_redvault_ffi_dir "${CMAKE_BINARY_DIR}/_deps/livekit-ffi-${REDVAULT_LIVEKIT_FFI_VERSION}")
set(REDVAULT_LIVEKIT_FFI_LIBRARY "${_redvault_ffi_dir}/${_redvault_ffi_file}" CACHE INTERNAL "")
set(REDVAULT_LIVEKIT_FFI_LICENSE "${_redvault_ffi_dir}/LICENSE.md" CACHE INTERNAL "")
if(NOT EXISTS "${REDVAULT_LIVEKIT_FFI_LIBRARY}")
    file(MAKE_DIRECTORY "${_redvault_ffi_dir}")
    set(_redvault_ffi_zip "${_redvault_ffi_dir}/${_redvault_ffi_asset}")
    if(REDVAULT_LIVEKIT_FFI_ZIP)
        file(COPY_FILE "${REDVAULT_LIVEKIT_FFI_ZIP}" "${_redvault_ffi_zip}")
        file(SHA256 "${_redvault_ffi_zip}" _redvault_ffi_actual)
        if(NOT _redvault_ffi_actual STREQUAL _redvault_ffi_sha256)
            message(FATAL_ERROR "${REDVAULT_LIVEKIT_FFI_ZIP} isn't LiveKit's ${_redvault_ffi_asset} ${REDVAULT_LIVEKIT_FFI_VERSION} (SHA-256 ${_redvault_ffi_actual}).")
        endif()
    else()
        message(STATUS "Downloading LiveKit's media library ${REDVAULT_LIVEKIT_FFI_VERSION} (${_redvault_ffi_asset})")
        file(DOWNLOAD "${REDVAULT_LIVEKIT_FFI_URL_BASE}/${_redvault_ffi_asset}" "${_redvault_ffi_zip}"
            EXPECTED_HASH SHA256=${_redvault_ffi_sha256}
            TLS_VERIFY ON
            SHOW_PROGRESS)
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_redvault_ffi_zip}" DESTINATION "${_redvault_ffi_dir}")
    file(REMOVE "${_redvault_ffi_zip}")
    if(NOT EXISTS "${REDVAULT_LIVEKIT_FFI_LIBRARY}")
        message(FATAL_ERROR "${_redvault_ffi_asset} doesn't contain ${_redvault_ffi_file}.")
    endif()
endif()

# Copies the library next to a built program, where Ffi looks for it first.
function(redvault_copy_livekit_ffi target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REDVAULT_LIVEKIT_FFI_LIBRARY}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# --- The protocol ---
# The package's own CMake config knows about the Abseil libraries protobuf's headers need;
# older distributions only have the FindProtobuf module.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(Threads REQUIRED)

set(_redvault_proto_dir "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/livekit-ffi/protocol")
set(_redvault_protos
    audio_frame capture data_stream data_track e2ee ffi handle participant
    room rpc stats track track_publication video_frame)
set(_redvault_proto_files "")
foreach(_name IN LISTS _redvault_protos)
    list(APPEND _redvault_proto_files "${_redvault_proto_dir}/${_name}.proto")
endforeach()

add_library(redvault_livekit_proto STATIC ${_redvault_proto_files})
protobuf_generate(
    TARGET redvault_livekit_proto
    LANGUAGE cpp
    PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto"
    IMPORT_DIRS "${_redvault_proto_dir}")
target_include_directories(redvault_livekit_proto SYSTEM PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/proto")
target_link_libraries(redvault_livekit_proto PUBLIC protobuf::libprotobuf)
if(NOT MSVC)
    # Generated code: not ours to keep warning-free.
    target_compile_options(redvault_livekit_proto PRIVATE -w)
endif()

# --- The wrapper ---
add_library(redvault_groupcall STATIC
    src/ffi_library.cpp
    src/ffi.cpp
    src/audio_mixer.cpp
    src/room.cpp
    src/session.cpp
)
target_include_directories(redvault_groupcall PUBLIC include PRIVATE src)
target_link_libraries(redvault_groupcall
    PUBLIC redvault_core
    PRIVATE redvault_livekit_proto
)
if(UNIX)
    target_link_libraries(redvault_groupcall PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if(MSVC)
    target_compile_options(redvault_groupcall PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_groupcall PRIVATE -Wall -Wextra -Wpedantic)
endif()
