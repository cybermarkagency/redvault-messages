// TURN credentials for long one-to-one calls (docs/tor-routing-plan.md, decision 5).
//
// The server issues coturn REST credentials whose username is
// "<expiry, unix seconds>:<random>" (server/src/services/callService.ts).
// coturn checks the expiry when a client authenticates, and asks for that again
// whenever its nonce goes stale (production: stale-nonce=600), so a call that
// outlives its credentials loses its relay soon after. Before that, the caller
// fetches fresh ones (POST /api/calls/:id/relay-credentials) and restarts ICE
// with them; the callee answers the restart with fresh credentials of its own.

/** When the credentials in `servers` stop working (epoch ms), or null if they don't say. */
export function relayExpiryMs(servers: { username?: string }[]): number | null {
  let earliest: number | null = null;
  for (const server of servers) {
    const match = /^(\d{9,12}):/.exec(server.username ?? "");
    if (!match) continue;
    const ms = Number(match[1]) * 1000;
    if (earliest === null || ms < earliest) earliest = ms;
  }
  return earliest;
}

/** Renew at 80% of what's left, and never sooner than a minute from now. */
export function renewalDelayMs(expiryMs: number, now = Date.now()): number {
  return Math.max(60_000, Math.floor((expiryMs - now) * 0.8));
}

/** What waiting for ICE gathering needs from a connection (a RTCPeerConnection, or a stand-in in tests). */
export type GatheringSource = Pick<RTCPeerConnection, "iceGatheringState" | "localDescription" | "addEventListener" | "removeEventListener">;

/**
 * Waits for ICE gathering to finish, so the sealed SDP carries every relay candidate (no trickle through the server).
 *
 * After an ICE restart (`afterRestart`) the state can still read "complete" when setLocalDescription resolves, with the
 * new description holding no candidates yet; gathering starts a moment later. Returning at once there sealed an SDP
 * without a relay candidate and the restart was dropped, every time. So after a restart "complete" only counts once
 * gathering has been seen to start again, or the new description already has its relay candidates.
 */
export function gatherComplete(pc: GatheringSource, timeoutMs: number, afterRestart = false): Promise<void> {
  const hasRelay = () => / typ relay /.test(pc.localDescription?.sdp ?? "");
  let sawGathering = pc.iceGatheringState === "gathering";
  const finished = () => pc.iceGatheringState === "complete" && (!afterRestart || sawGathering || hasRelay());
  if (finished()) return Promise.resolve();
  return new Promise((resolve) => {
    const done = () => {
      clearTimeout(timer);
      pc.removeEventListener("icegatheringstatechange", check);
      resolve();
    };
    const check = () => {
      if (pc.iceGatheringState === "gathering") sawGathering = true;
      if (finished()) done();
    };
    const timer = setTimeout(done, timeoutMs);
    pc.addEventListener("icegatheringstatechange", check);
  });
}
