# Call media engine: libdatachannel (ICE/TURN, DTLS-SRTP, RTP/RTCP) with
# Opus audio and VP8 video, Qt-free so it's tested like the core.
#
# libdatachannel isn't packaged by MSYS2 or Ubuntu 24.04, so it's built from
# source at a pinned release. Only the submodules it needs are fetched
# (libjuice for ICE, usrsctp, plog); libSRTP, OpenSSL and nlohmann/json come
# from the system.
include(FetchContent)

set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_SRTP ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_JSON ON CACHE BOOL "" FORCE)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.6
    GIT_SHALLOW TRUE
    GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog
    EXCLUDE_FROM_ALL
)
# Build it static, into this library, whatever the rest of the project does.
set(_redvault_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(libdatachannel)
set(BUILD_SHARED_LIBS ${_redvault_saved_shared})

# Relay-only calls have to be relay-only on the wire (see the file).
include(patch_relay_only.cmake)
redvault_patch_relay_only("${libdatachannel_SOURCE_DIR}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)

add_library(redvault_rtc STATIC
    src/call_media.cpp
    src/codecs.cpp
    src/relay_sdp.cpp
    src/rtp.cpp
)
target_include_directories(redvault_rtc PUBLIC include PRIVATE src)
target_link_libraries(redvault_rtc
    PRIVATE LibDataChannel::LibDataChannelStatic PkgConfig::OPUS PkgConfig::VPX
)

if(MSVC)
    target_compile_options(redvault_rtc PRIVATE /W4 /permissive-)
else()
    target_compile_options(redvault_rtc PRIVATE -Wall -Wextra -Wpedantic)
endif()
