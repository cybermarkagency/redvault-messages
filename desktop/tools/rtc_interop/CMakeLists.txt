# Manual interop check against a real browser (see main.cpp). Not shipped.
add_executable(redvault_rtc_interop main.cpp)
target_link_libraries(redvault_rtc_interop PRIVATE redvault_rtc redvault_core)
if(WIN32)
    target_link_libraries(redvault_rtc_interop PRIVATE ws2_32)
endif()
