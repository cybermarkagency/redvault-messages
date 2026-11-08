# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Manual check of group call media against a real LiveKit server (see main.cpp). Not shipped.
add_executable(redvault_groupcall_probe main.cpp)
target_link_libraries(redvault_groupcall_probe PRIVATE redvault_groupcall redvault_core)
redvault_copy_livekit_ffi(redvault_groupcall_probe)
