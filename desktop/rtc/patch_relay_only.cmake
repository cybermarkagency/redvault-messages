# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

# Makes libdatachannel's Relay policy true on the wire. Included after FetchContent has the sources.
#
# TransportPolicy::Relay only stops local candidates being announced. libjuice
# still pairs its direct socket with every remote candidate (see
# agent_add_candidate_pairs_for_remote: "local non-relayed candidates are
# undifferentiated for sending"), so a peer that lists an address gets STUN
# checks from this computer's real IP, and the type in the candidate doesn't
# matter: "typ relay" on a home address gets the same pair. Filtering the SDP
# can't catch that, because the type is only a label the sender wrote.
#
# With this patch a relay-only agent has no direct pair at all: every check goes
# through its TURN allocation, and a binding request that reaches the direct
# socket finds no pair, so it gets no answer. Without an allocation there are
# no pairs and the call fails.
#
# Edits are single-line and idempotent, so a CRLF checkout and a second configure
# are both fine. A libdatachannel bump that moves any of these lines stops the
# configure: review the new code, don't drop the patch.

function(_redvault_patch_source file marker anchor replacement)
    file(READ "${file}" content)
    string(FIND "${content}" "${marker}" already)
    if(NOT already EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${anchor}" first)
    string(FIND "${content}" "${anchor}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "Relay-only patch: expected exactly one \"${anchor}\" in ${file}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" content "${content}")
    file(WRITE "${file}" "${content}")
endfunction()

function(redvault_patch_relay_only source_dir)
    set(juice "${source_dir}/deps/libjuice")
    _redvault_patch_source("${juice}/include/juice/juice.h" "bool relay_only;"
        "} juice_config_t;"
        "\tbool relay_only; // Redvault: never pair the direct socket with a remote candidate\n} juice_config_t;")
    _redvault_patch_source("${juice}/src/agent.c" "agent->config.relay_only = config->relay_only;"
        "agent->config.user_ptr = config->user_ptr;"
        "agent->config.user_ptr = config->user_ptr;\n\tagent->config.relay_only = config->relay_only;")
    _redvault_patch_source("${juice}/src/agent.c" "!agent->config.relay_only &&"
        "if (agent_add_candidate_pair(agent, NULL, remote))"
        "if (!agent->config.relay_only && agent_add_candidate_pair(agent, NULL, remote))")
    _redvault_patch_source("${source_dir}/src/impl/icetransport.cpp" "jconfig.relay_only ="
        "jconfig.user_ptr = this;"
        "jconfig.user_ptr = this;\n\tjconfig.relay_only = config.iceTransportPolicy == TransportPolicy::Relay;")
endfunction()
