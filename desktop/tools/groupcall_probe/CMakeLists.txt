# Manual check of group call media against a real LiveKit server (see main.cpp). Not shipped.
add_executable(redvault_groupcall_probe main.cpp)
target_link_libraries(redvault_groupcall_probe PRIVATE redvault_groupcall redvault_core)
redvault_copy_livekit_ffi(redvault_groupcall_probe)
