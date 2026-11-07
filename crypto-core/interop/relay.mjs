// The interop run's stand-in for the server: a device directory (public
// identities and signed one-time keys, each key handed out once), a mailbox per
// device for Olm and group messages, and a job queue the browser page uses as
// its driver. Like the real server it only ever holds public keys and
// ciphertext, and it logs routes and counts, never bodies.
//
//   node crypto-core/interop/relay.mjs        (listens on 127.0.0.1:8787)
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

const PORT = Number(process.env.PORT ?? 8787);
const HERE = fileURLToPath(new URL(".", import.meta.url));
const STATIC = {
  "/": "browser.html",
  "/browser.html": "browser.html",
  // The research engine's page (crypto-research/interop): post-quantum MLS and key-transparency checks.
  "/research": "../../crypto-research/interop/research.html",
};
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".wasm": "application/wasm" };

const devices = new Map(); // "user/device" -> { identity, oneTimeKeys: [], fallbackKey }
const lists = new Map(); // userId -> the account's latest signed device list (opaque to the relay)
const mailboxes = new Map(); // "user/device" -> [{ id, from, kind, payload }]
const jobs = new Map(); // worker -> [{ id, request }]
const results = new Map(); // job id -> response
const waiting = { jobs: new Map(), results: new Map() };
let seq = 0;

const key = (userId, deviceId) => `${userId}/${deviceId}`;

function body(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      try {
        resolve(chunks.length ? JSON.parse(Buffer.concat(chunks).toString("utf8")) : {});
      } catch (err) {
        reject(err);
      }
    });
  });
}

function send(res, status, value) {
  res.writeHead(status, { "content-type": "application/json", "cache-control": "no-store" });
  res.end(value === undefined ? "" : JSON.stringify(value));
}

function wake(map, id, value) {
  const listeners = map.get(id);
  if (!listeners) return;
  map.delete(id);
  for (const fn of listeners) fn(value);
}

function wait(map, id, ms) {
  return new Promise((resolve) => {
    const list = map.get(id) ?? [];
    map.set(id, list);
    const timer = setTimeout(() => resolve(undefined), ms);
    list.push((value) => {
      clearTimeout(timer);
      resolve(value);
    });
  });
}

const routes = [
  ["POST", /^\/devices$/, async (req, res) => {
    const { identity, upload } = await body(req);
    devices.set(key(identity.userId, identity.deviceId), {
      identity,
      oneTimeKeys: [...(upload.oneTimeKeys ?? [])],
      fallbackKey: upload.fallbackKey ?? null,
    });
    send(res, 204);
  }],
  ["POST", /^\/lists$/, async (req, res) => {
    const { deviceList } = await body(req);
    lists.set(deviceList.userId, deviceList);
    send(res, 204);
  }],
  ["GET", /^\/lists\/([^/]+)$/, async (_req, res, [userId]) => {
    const list = lists.get(decodeURIComponent(userId));
    list ? send(res, 200, list) : send(res, 404, { error: "not_found" });
  }],
  ["GET", /^\/devices\/([^/]+)$/, async (_req, res, [userId]) => {
    send(res, 200, [...devices.values()].filter((d) => d.identity.userId === decodeURIComponent(userId)).map((d) => d.identity));
  }],
  // Hands out one signed one-time key, once (the fallback key when they're used up).
  ["POST", /^\/claim$/, async (req, res) => {
    const { userId, deviceId } = await body(req);
    const device = devices.get(key(userId, deviceId));
    if (!device) return send(res, 404, { error: "not_found" });
    send(res, 200, { identity: device.identity, oneTimeKey: device.oneTimeKeys.shift() ?? device.fallbackKey });
  }],
  ["POST", /^\/messages$/, async (req, res) => {
    const { to, from, kind, payload } = await body(req);
    const box = mailboxes.get(key(to.userId, to.deviceId)) ?? [];
    const id = `m${++seq}`;
    box.push({ id, from, kind, payload });
    mailboxes.set(key(to.userId, to.deviceId), box);
    send(res, 200, { id });
  }],
  ["GET", /^\/messages\/([^/]+)\/([^/]+)$/, async (_req, res, [userId, deviceId]) => {
    send(res, 200, mailboxes.get(key(decodeURIComponent(userId), decodeURIComponent(deviceId))) ?? []);
  }],
  ["POST", /^\/messages\/ack$/, async (req, res) => {
    const { userId, deviceId, ids } = await body(req);
    const box = mailboxes.get(key(userId, deviceId)) ?? [];
    mailboxes.set(key(userId, deviceId), box.filter((m) => !ids.includes(m.id)));
    send(res, 204);
  }],
  // --- The browser page as a driver ---
  ["POST", /^\/jobs\/([^/]+)$/, async (req, res, [worker]) => {
    const job = await body(req);
    const queue = jobs.get(worker) ?? [];
    queue.push(job);
    jobs.set(worker, queue);
    wake(waiting.jobs, worker);
    send(res, 204);
  }],
  ["GET", /^\/jobs\/([^/]+)\/next$/, async (_req, res, [worker]) => {
    if (!(jobs.get(worker) ?? []).length) await wait(waiting.jobs, worker, 25_000);
    const job = (jobs.get(worker) ?? []).shift();
    job ? send(res, 200, job) : send(res, 204);
  }],
  ["POST", /^\/jobs\/([^/]+)\/result$/, async (req, res) => {
    const { id, response } = await body(req);
    results.set(id, response);
    wake(waiting.results, id);
    send(res, 204);
  }],
  ["GET", /^\/results\/([^/]+)$/, async (_req, res, [id]) => {
    if (!results.has(id)) await wait(waiting.results, id, 60_000);
    if (!results.has(id)) return send(res, 204);
    const response = results.get(id);
    results.delete(id);
    send(res, 200, { response });
  }],
];

createServer(async (req, res) => {
  const url = new URL(req.url, "http://localhost");
  try {
    if (req.method === "GET" && (STATIC[url.pathname] || url.pathname.startsWith("/pkg/") || url.pathname.startsWith("/research-pkg/"))) {
      const [prefix, dir] = url.pathname.startsWith("/research-pkg/") ? ["/research-pkg/", "../../crypto-research/interop/build/web"] : ["/pkg/", "build/web"];
      const file = STATIC[url.pathname] ?? join(dir, normalize(url.pathname.slice(prefix.length)).replace(/^(\.\.[/\\])+/, ""));
      const data = await readFile(join(HERE, file));
      res.writeHead(200, { "content-type": TYPES[extname(file)] ?? "application/octet-stream", "cache-control": "no-store" });
      return res.end(data);
    }
    for (const [method, pattern, handler] of routes) {
      const match = req.method === method && url.pathname.match(pattern);
      if (match) return await handler(req, res, match.slice(1));
    }
    send(res, 404, { error: "not_found" });
  } catch (err) {
    send(res, 400, { error: "bad_request", message: String(err?.message ?? err) });
  }
}).listen(PORT, "127.0.0.1", () => console.log(`relay listening on http://127.0.0.1:${PORT}`));
