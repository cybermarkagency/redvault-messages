/**
 * Prototype of the server-side half of the Tor-only policy (to move into
 * server/src/middleware once approved). The server can't trust anything a
 * client says about its route, so it decides by which listener a request
 * arrived on: the onion service forwards to a loopback-only port that nginx
 * never proxies to.
 *
 * Default deny: with enforcement on, every route needs the onion listener
 * except a short clearnet allowlist (health check, payment-provider webhooks,
 * public channel reading and the public transparency log). A new route is
 * therefore Tor-only until someone deliberately allowlists it.
 */
import type { NextFunction, Request, Response } from "express";

export interface TorPolicyOptions {
  /** Off: log-only (rollout). On: refuse clearnet requests to Tor-only routes. */
  enforce: () => boolean;
  /** True when the request came in on the onion listener. */
  viaOnion: (req: Request) => boolean;
  /** Called for every refused request (no IPs: the server never has them). */
  onRefused?: (method: string, path: string) => void;
}

/** Routes that stay reachable without Tor. Everything else is Tor-only. */
const CLEARNET_ALLOWED: readonly RegExp[] = [
  /^\/health$/,
  // Stripe / BTCPay / NOWPayments call these from their own servers.
  /^\/api\/billing\/(stripe|btcpay|nowpayments)-webhook(\/|$)/,
  // Public, unauthenticated, no private content.
  /^\/api\/public\/channels(\/|$)/,
  /^\/api\/public\/transparency(-feed|-log)?(\/|$)/,
];

export function isClearnetAllowed(path: string): boolean {
  return CLEARNET_ALLOWED.some((pattern) => pattern.test(path));
}

export function torRoutingPolicy(options: TorPolicyOptions) {
  return (req: Request, res: Response, next: NextFunction) => {
    if (options.viaOnion(req) || isClearnetAllowed(req.path)) return next();
    options.onRefused?.(req.method, req.path);
    if (!options.enforce()) return next();
    res.status(403).json({
      error: "tor_required",
      message: "Private messages and media are only available through Redvault's Tor onion service.",
    });
  };
}
