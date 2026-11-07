#pragma once

#include <string>
#include <string_view>

// Relay-only calls: what may reach the ICE agent from the other person's SDP.
//
// The other person writes their SDP, so nothing in it is trusted. A relay-only
// call must never send a packet from this computer's own address to an address
// the other person chose, because that packet shows them where this computer is.
// Only relay candidates survive here; anything that can't be read for certain
// throws RtcError("bad_candidate"), so a line two parsers would read differently
// never gets as far as the second parser.
//
// This is one of two layers. A candidate's type is only a label the sender
// chose, so the filter can't tell a real relay from a host address marked
// "typ relay". What makes that harmless is the patch in rtc/patch_relay_only.cmake:
// in relay mode the ICE agent keeps no pair on its direct socket, so every check
// leaves through the TURN relay, whatever the candidate says it is.

namespace redvault::rtc {

struct RelayFilterResult {
    std::string sdp;
    /// Relay candidates kept.
    int kept = 0;
    /// Candidates removed (host, srflx, prflx, or anything but UDP).
    int removed = 0;
};

/// Keeps only relay candidates, at session and media level. Throws RtcError("bad_candidate") on anything malformed.
RelayFilterResult keepRelayCandidates(std::string_view sdp);

/// One candidate, with or without its "a=" or "a=candidate:" prefix: true if it's a relay candidate
/// with a literal IPv4 or IPv6 address. Throws RtcError("bad_candidate") if it's malformed.
bool isRelayCandidate(std::string_view candidate);

}  // namespace redvault::rtc
