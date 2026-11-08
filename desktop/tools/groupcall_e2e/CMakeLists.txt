# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# End-to-end check of a group call participant against a real server, without the GUI (see main.cpp). Not shipped.
add_executable(redvault_groupcall_e2e main.cpp)
target_link_libraries(redvault_groupcall_e2e PRIVATE redvault_groupcall redvault_core)
redvault_copy_livekit_ffi(redvault_groupcall_e2e)
