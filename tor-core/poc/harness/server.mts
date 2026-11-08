// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/**
 * Scratch Redvault API for the Tor proof of concept: the real server code
 * (server/src/app.ts) on in-memory repositories, behind three loopback
 * listeners:
 *
 *   onion   ONION_PORT    only the onion service forwards here (the Tor path)
 *   direct  DIRECT_PORT   stands in for the clearnet nginx path
 *   control CONTROL_PORT  test hooks and the request log (never forwarded)
 *
 * Every request is logged with the listener it arrived on, so the tests can
 * prove which path the apps used. Nothing here touches the production server.
 *
 * Run with the server's tsx:  server/node_modules/.bin/tsx server.mts
 * Env: ONION_PORT (18080) DIRECT_PORT (18081) CONTROL_PORT (18082) ENFORCE (1)
 *
 * TOR_ROUTING_MODE (off|report|notice|enforce, unset by default) also turns on
 * the server's own middleware (server/src/middleware/torRouting.ts) with the
 * onion listener as ONION_PORT, ONION_ADDRESS and TOR_ROUTING_CUTOFF_AT; run it
 * with ENFORCE=0 so only that one decides.
 *
 * WEB_DIST: a built web app (vite build --outDir ...), served on both listeners.
 * Calls (off unless set): TURN_URLS (comma separated turn: URLs), TURN_SECRET, TURN_CREDENTIAL_TTL (seconds);
 * LIVEKIT_URL (ws:// or wss://), LIVEKIT_API_KEY, LIVEKIT_API_SECRET.
 */
import http from "node:http";
import { randomBytes } from "node:crypto";
import { fileURLToPath, pathToFileURL } from "node:url";
import path from "node:path";
import { createRequire } from "node:module";

import { isClearnetAllowed, torRoutingPolicy } from "./torPolicy.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const serverDir = path.resolve(here, "../../../server");
const serverModule = (rel: string) => pathToFileURL(path.join(serverDir, rel)).href;
const requireFromServer = createRequire(path.join(serverDir, "package.json"));
const express = requireFromServer("express");

const { createApp } = await import(serverModule("src/app.ts"));
const { createInMemoryRepositories } = await import(serverModule("src/test/fakes/inMemoryRepositories.ts"));
const { InMemoryAvatarStorage } = await import(serverModule("src/test/fakes/inMemoryAvatarStorage.ts"));
const { InMemoryFileObjectStorage } = await import(serverModule("src/test/fakes/inMemoryFiles.ts"));

const ONION_PORT = Number(process.env.ONION_PORT ?? 18080);
const DIRECT_PORT = Number(process.env.DIRECT_PORT ?? 18081);
const CONTROL_PORT = Number(process.env.CONTROL_PORT ?? 18082);
let enforce = (process.env.ENFORCE ?? "1") === "1";

const realMode = process.env.TOR_ROUTING_MODE as "off" | "report" | "notice" | "enforce" | undefined;
const repos = createInMemoryRepositories();
const fileStorage = new InMemoryFileObjectStorage();
const app = createApp({
  repos,
  usernamePepper: "tor-poc-pepper",
  usernameEncryptionKey: randomBytes(32),
  sessionTtlMs: 60 * 60 * 1000,
  maxFailedLoginAttempts: 5,
  accountLockMs: 15 * 60 * 1000,
  // The onion address is http:// (the onion service itself authenticates and encrypts).
  cookieSecure: false,
  avatarStorage: new InMemoryAvatarStorage(),
  demoConversationId: null,
  calls: process.env.TURN_URLS
    ? {
        turnUrls: process.env.TURN_URLS.split(",").map((u) => u.trim()).filter(Boolean),
        turnSecret: process.env.TURN_SECRET ?? "",
        credentialTtlSeconds: Number(process.env.TURN_CREDENTIAL_TTL ?? 24 * 60 * 60),
      }
    : null,
  groupCalls: process.env.LIVEKIT_URL
    ? {
        url: process.env.LIVEKIT_URL,
        apiKey: process.env.LIVEKIT_API_KEY ?? "",
        apiSecret: process.env.LIVEKIT_API_SECRET ?? "",
        tokenTtlSeconds: 6 * 60 * 60,
      }
    : null,
  torRouting: realMode
    ? {
        mode: realMode,
        onionListenPort: ONION_PORT,
        onionAddress: process.env.ONION_ADDRESS ?? null,
        cutoffAt: process.env.TOR_ROUTING_CUTOFF_AT ? new Date(process.env.TOR_ROUTING_CUTOFF_AT) : null,
      }
    : null,
  push: null,
  e2eV2Enabled: false,
  files: {
    storage: fileStorage,
    config: {
      transferMode: "proxy",
      maxFileSize: 25 * 1024 * 1024,
      uploadUrlTtlSeconds: 900,
      downloadUrlTtlSeconds: 900,
      retentionDays: 30,
      orphanUploadTtlSeconds: 3600,
      maxPendingUploadsPerUser: 20,
      dailyUploadBytesPerUser: 500 * 1024 * 1024,
      maxAttachmentsPerMessage: 10,
      proStorageLimitBytes: 1024 * 1024 * 1024,
    },
  },
  // Files tab (resumable uploads in parts, Range downloads), same in-memory bucket.
  backup: {
    storage: fileStorage,
    config: {
      transferMode: "proxy",
      storageLimits: { free: 1024 * 1024 * 1024, pro: 1024 * 1024 * 1024, premium: 1024 * 1024 * 1024 },
      maxObjectSize: 200 * 1024 * 1024,
      uploadUrlTtlSeconds: 3600,
      downloadUrlTtlSeconds: 900,
      maxPendingUploadsPerUser: 20,
    },
  },
});

type Entry = { at: string; listener: "onion" | "direct"; method: string; path: string; status: number };
const log: Entry[] = [];
const refusedClearnet: { method: string; path: string }[] = [];

const listenerOf = (req: http.IncomingMessage): "onion" | "direct" =>
  req.socket.localPort === ONION_PORT ? "onion" : "direct";

const outer = express();
outer.disable("x-powered-by");
outer.use((req: any, res: any, next: any) => {
  const entry: Entry = {
    at: new Date().toISOString(),
    listener: listenerOf(req),
    method: req.method,
    path: req.path,
    status: 0,
  };
  res.on("finish", () => {
    entry.status = res.statusCode;
    log.push(entry);
  });
  next();
});
// Fault switch for the tests: POST /fault {"mode":"reset"} on the control port makes the onion listener drop every
// API connection without an answer (what a page sees when Tor or the onion service is unreachable);
// {"mode":"none"} ends it. The page itself and the other listeners are unaffected.
let fault: "none" | "reset" = "none";
outer.use((req: any, res: any, next: any) => {
  if (fault === "reset" && listenerOf(req) === "onion" && (req.path.startsWith("/api") || req.path === "/health")) {
    req.socket.destroy();
    return;
  }
  next();
});
// The built web app (WEB_DIST), as nginx would serve it: static files, then the single-page fallback.
// It sits before the API's Tor policy on purpose: only the API is private, the page itself isn't.
const webDist = process.env.WEB_DIST;
if (webDist) {
  const isApi = (p: string) => p.startsWith("/api") || p === "/health" || p.startsWith("/__bench");
  const files = express.static(webDist, { index: "index.html" });
  outer.use((req: any, res: any, next: any) => (isApi(req.path) ? next() : files(req, res, next)));
  outer.use((req: any, res: any, next: any) =>
    req.method === "GET" && !isApi(req.path) && req.accepts("html") ? res.sendFile(path.join(webDist, "index.html")) : next()
  );
}
// Throughput benchmark (onion listener only): GET /__bench/<KiB> returns that many random bytes.
outer.get("/__bench/:kib", (req: any, res: any) => {
  if (listenerOf(req) !== "onion") return res.status(404).end();
  const kib = Math.min(Number(req.params.kib) || 0, 16 * 1024);
  res.setHeader("Content-Type", "application/octet-stream");
  res.end(randomBytes(kib * 1024));
});
outer.use(
  torRoutingPolicy({
    enforce: () => enforce,
    viaOnion: (req) => listenerOf(req) === "onion",
    onRefused: (method, p) => refusedClearnet.push({ method, path: p }),
  })
);
outer.use(app);

const control = express();
control.use(express.json());
control.get("/log", (_req: any, res: any) => res.json({ log, refusedClearnet }));
control.post("/log/clear", (_req: any, res: any) => {
  log.length = 0;
  refusedClearnet.length = 0;
  res.json({ ok: true });
});
control.post("/policy", (req: any, res: any) => {
  enforce = Boolean(req.body?.enforce);
  res.json({ enforce });
});
control.post("/fault", (req: any, res: any) => {
  fault = req.body?.mode === "reset" ? "reset" : "none";
  res.json({ fault });
});
control.get("/policy", (_req: any, res: any) => res.json({ enforce, sample: { "/api/conversations": isClearnetAllowed("/api/conversations"), "/health": isClearnetAllowed("/health") } }));
// Files need a paid plan: give a test account Pro by username lookup through the repositories.
control.post("/tier", async (req: any, res: any) => {
  const { userId, tier } = req.body ?? {};
  await repos.users.setSubscriptionTier(userId, tier ?? "pro");
  res.json({ ok: true });
});
control.get("/stored-blobs", (_req: any, res: any) => {
  // What the server holds for files: sizes only, to show it's ciphertext of the expected length.
  const objects = (fileStorage as any).objects as Map<string, Uint8Array> | undefined;
  res.json({ count: objects?.size ?? null });
});

const bind = (handler: any, port: number, name: string) =>
  new Promise<void>((resolve) => {
    http.createServer(handler).listen(port, "127.0.0.1", () => {
      console.log(`${name} listening on 127.0.0.1:${port}`);
      resolve();
    });
  });

await bind(outer, ONION_PORT, "onion");
await bind(outer, DIRECT_PORT, "direct");
await bind(control, CONTROL_PORT, "control");
console.log(`tor policy enforcement: ${enforce ? "on" : "off (log only)"}`);
