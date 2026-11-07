# Checks REDVAULT_SOURCE_CODE_URL and REDVAULT_SOURCE_CONTACT (see the top-level CMakeLists.txt), which become
# the source link and contact in Settings > About, "Open-Source Licenses and Source Code". A bad value stops the
# configure step rather than shipping a broken or non-https link. Also runs as a script, for the tests:
#   cmake -DREDVAULT_SOURCE_CODE_URL=... -DREDVAULT_SOURCE_CONTACT=... -P source_info.cmake

function(redvault_check_source_settings url contact)
    if(NOT url STREQUAL "" AND
       NOT url MATCHES "^https://[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]+)?(/[A-Za-z0-9._~%/+=@{}-]*)?$")
        message(FATAL_ERROR "REDVAULT_SOURCE_CODE_URL must be an https:// URL (host and path only: letters, digits "
                            "and . _ ~ % / + = @ { } -), or empty while the source isn't published. Got: \"${url}\"")
    endif()
    if(NOT contact STREQUAL "" AND
       NOT contact MATCHES "^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]+$" AND
       NOT contact MATCHES "^https://[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]+)?(/[A-Za-z0-9._~%/+=@-]*)?$")
        message(FATAL_ERROR "REDVAULT_SOURCE_CONTACT must be an email address or an https:// URL, or empty to "
                            "use the Terms of Service contact. Got: \"${contact}\"")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    redvault_check_source_settings("${REDVAULT_SOURCE_CODE_URL}" "${REDVAULT_SOURCE_CONTACT}")
    message(STATUS "Source settings accepted")
endif()
