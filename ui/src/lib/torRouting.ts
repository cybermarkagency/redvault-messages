// Tor routing for the web app (docs/tor-routing-plan.md).
//
// The web app can't route anything through Tor by itself: JavaScript has no
// say in how the browser connects. What it can do is run from the service's
// onion address in Tor Browser, where every request it makes (to its own
// origin, the only one it ever talks to) goes through Tor, and refuse to be
// the way around that. On the clearnet address it says where to go instead,
// and once the server enforces Tor routing it doesn't sign anyone in there.

import { API_BASE } from "./apiBase.js";

/** The same words on every platform. */
export const TOR_UNAVAILABLE_MESSAGE =
  "Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored.";

/** Tor Browser turns WebRTC off, on purpose: it would reveal the IP address. */
export const CALLS_UNAVAILABLE_MESSAGE =
  "Calls aren't available in Tor Browser: it turns WebRTC off so it can't reveal your IP address. Use the desktop or Android app for calls (they go through the call relay, not Tor).";

export type TorRoutingMode = "off" | "report" | "notice" | "enforce";

/** GET /api/public/routing (server/src/middleware/torRouting.ts). */
export interface RoutingInfo {
  mode: TorRoutingMode;
  onionAddress: string | null;
  cutoffAt: string | null;
  viaOnion: boolean;
}

/** This page was loaded from an onion address, so (in Tor Browser) all of its requests go through Tor. */
export function onOnionOrigin(): boolean {
  return window.location.hostname.toLowerCase().endsWith(".onion");
}

/**
 * Tor Browser turns WebRTC off (it would reveal the IP address), so calls
 * can't work there. Calls are never Tor-anonymous anyway: they go through
 * the TURN relay, in the desktop and Android apps.
 */
export function webRtcAvailable(): boolean {
  return typeof RTCPeerConnection !== "undefined";
}

/** What the server says about Tor routing; null when it can't be asked (an older server, or offline). */
export async function fetchRouting(): Promise<RoutingInfo | null> {
  try {
    const res = await fetch(`${API_BASE}/public/routing`, { credentials: "omit", cache: "no-store" });
    if (!res.ok) return null;
    const body = (await res.json()) as Partial<RoutingInfo>;
    if (!body || typeof body.mode !== "string") return null;
    return {
      mode: body.mode as TorRoutingMode,
      onionAddress: typeof body.onionAddress === "string" ? body.onionAddress : null,
      cutoffAt: typeof body.cutoffAt === "string" ? body.cutoffAt : null,
      viaOnion: body.viaOnion === true,
    };
  } catch {
    return null;
  }
}

/** The web app's address on the onion service (same path as here). */
export function onionUrl(onionAddress: string): string {
  return `http://${onionAddress}${import.meta.env.BASE_URL}`;
}

/**
 * What to say when the server couldn't be reached at all. It says nothing about the account, so it's
 * safe to show on the sign-in screens too (a dropped Tor circuit is routine, and a wrong-password
 * message for it would send people chasing the wrong problem).
 */
export function unreachableMessage(): string {
  return onOnionOrigin() ? TOR_UNAVAILABLE_MESSAGE : "Can't reach the server. Check your connection and try again.";
}

/**
 * A request that failed before any answer: no connection, or (in Tor
 * Browser) Tor or the onion service unreachable. Worth sending again later
 * as it is, unlike a refusal from the server.
 */
export function isNetworkFailure(err: unknown): boolean {
  return err instanceof TypeError || (err instanceof Error && err.name === "NetworkError");
}
