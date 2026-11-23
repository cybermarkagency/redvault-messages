// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Sesame gaps 1 and 2 (docs/signal-integration/13-every-device-and-session-reset.md) between the real apps'
// code, through the REAL server code on a throwaway PostgreSQL cluster. Development only: throwaway accounts.
//
//   Windows desktop: redvault_v3_probe.exe (the redvault_e2ev3 library the app uses)
//   Android:         SignalProbeTest on an emulator (the app's devicekeys code, Keystore, JNI engine), via adb reverse
//
// A recording proxy sits between the apps and the server. It logs each mailbox send (marked or not, which
// devices, the answer) and can hand a sender, once, the device list it saw before a new device was linked:
// the race of gap 1. Gap 2: a desktop is put back to an old copy of its data, as after restoring a backup.
//
//   node desktop/tools/v3_probe/sesame-run.mjs
//
// Env: SESAME_WIN_PROBE, SESAME_ADB_SERIAL (default emulator-5580), SESAME_APK, SESAME_TEST_APK.
import { spawn, spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { cpSync, createWriteStream, existsSync, mkdirSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { createServer as createHttpServer, request as httpRequest } from "node:http";
import { createServer } from "node:net";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const HERE = fileURLToPath(new URL(".", import.meta.url));
const ROOT = join(HERE, "..", "..", "..");
const PORT = 3055; // what the apps reach (the proxy)
const SERVER_PORT = 3059; // the server behind it
const PG_PORT = 5493;
const CONTROL_PORT = 3057;
const SERVER = `http://127.0.0.1:${PORT}`;
const WORK = mkdtempSync(join(tmpdir(), "rv-sesame-"));
const WIN_PROBE = process.env.SESAME_WIN_PROBE ?? join(ROOT, "desktop", "build", "release", "tools", "v3_probe", "redvault_v3_probe.exe");
const SERIAL = process.env.SESAME_ADB_SERIAL ?? "emulator-5580";
const ADB = join(process.env.LOCALAPPDATA, "Android", "Sdk", "platform-tools", "adb.exe");
const APK_DIR = join(ROOT, "android", "app", "build", "outputs", "apk");
const APK = process.env.SESAME_APK ?? join(APK_DIR, "debug", "app-debug.apk");
const TEST_APK = process.env.SESAME_TEST_APK ?? join(APK_DIR, "androidTest", "debug", "app-debug-androidTest.apk");

const sodium = createRequire(join(ROOT, "ui", "package.json"))("libsodium-wrappers-sumo");
await sodium.ready;
const pg = createRequire(join(ROOT, "server", "package.json"))("pg");

const b64 = (bytes) => Buffer.from(bytes).toString("base64");
const hex = (n) => randomBytes(n).toString("hex");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function within(ms, what, promise) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => (timer = setTimeout(() => reject(new Error(`timed out after ${ms / 1000} s: ${what}`)), ms)))]).finally(() => clearTimeout(timer));
}
const step = (text) => console.log(`\n  ..   ${text}`);
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: Boolean(ok) });
  const text = typeof detail === "string" ? detail : JSON.stringify(detail);
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${ok || !text ? "" : ` -- ${text.slice(0, 600)}`}`);
}
const sh = (command, args, options = {}) => spawnSync(command, args, { encoding: "utf8", windowsHide: true, ...options });
const kill = (child) => {
  if (!child || child.exitCode !== null) return;
  if (process.platform === "win32") spawnSync("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
  else child.kill();
};

// --- PostgreSQL, the server, and the recording proxy ---

const pgTool = (name) => join("C:/Program Files/PostgreSQL/17/bin", `${name}.exe`);
const PGDATA = join(WORK, "pgdata");
let pgProcess = null;
let db = null;
async function startPostgres() {
  const r = sh(pgTool("initdb"), ["-D", PGDATA, "-U", "postgres", "-A", "trust", "-E", "UTF8"]);
  if (r.status !== 0) throw new Error(`initdb: ${r.stderr}`);
  pgProcess = spawn(pgTool("postgres"), ["-D", PGDATA, "-p", String(PG_PORT), "-c", "listen_addresses=127.0.0.1"], { stdio: "ignore", windowsHide: true });
  for (let i = 0; i < 100; i++) {
    if (sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "postgres", "-c", "select 1"]).status === 0) break;
    await sleep(300);
  }
  sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-c", "CREATE DATABASE sesame"]);
  const files = [join(ROOT, "db", "schema.sql"), ...readdirSync(join(ROOT, "db", "migrations")).sort().map((f) => join(ROOT, "db", "migrations", f))];
  for (const file of files) sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "sesame", "-q", "-f", file]);
  db = new pg.Pool({ connectionString: `postgres://postgres@127.0.0.1:${PG_PORT}/sesame` });
}

let serverProcess = null;
const serverLog = createWriteStream(join(WORK, "server.log"), { flags: "a" });
async function startServer() {
  const tsx = join(ROOT, "server", "node_modules", "tsx", "dist", "cli.mjs");
  serverProcess = spawn(process.execPath, [tsx, join(ROOT, "crypto-core", "interop", "server", "dev-server.mts")], {
    env: {
      ...process.env,
      V3_DEV_PEPPER: randomBytes(32).toString("base64"),
      V3_DEV_USERNAME_KEY: randomBytes(32).toString("base64"),
      V3_DEV_DATABASE_URL: `postgres://postgres@127.0.0.1:${PG_PORT}/sesame`,
      V3_DEV_PORT: String(SERVER_PORT),
      V3_ENABLED: "true",
      V3_SIGNAL_ENABLED: "true",
    },
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  serverProcess.stdout.pipe(serverLog, { end: false });
  serverProcess.stderr.pipe(serverLog, { end: false });
  for (let i = 0; i < 80; i++) {
    try {
      if ((await fetch(`http://127.0.0.1:${SERVER_PORT}/health`)).ok) return;
    } catch {}
    await sleep(500);
  }
  throw new Error("the dev API didn't start");
}

/** Every mailbox send the apps make: { from, path, allDevices, to: ["user/device"], status, body }. */
const sends = [];
/** Last device-list answer per account, and an old one to hand out once (userId -> {status, type, body}). */
const lastList = new Map();
const replayOnce = new Map();
let proxy = null;
function startProxy() {
  proxy = createHttpServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const body = Buffer.concat(chunks);
      const listMatch = req.method === "GET" && req.url.match(/^\/api\/e2e\/v3\/users\/([0-9a-f-]{36})\/device-list/);
      if (listMatch && replayOnce.has(listMatch[1])) {
        const old = replayOnce.get(listMatch[1]);
        replayOnce.delete(listMatch[1]);
        res.writeHead(old.status, { "content-type": old.type });
        res.end(old.body);
        return;
      }
      const upstream = httpRequest(
        { host: "127.0.0.1", port: SERVER_PORT, method: req.method, path: req.url, headers: { ...req.headers, host: `127.0.0.1:${SERVER_PORT}` } },
        (up) => {
          const out = [];
          up.on("data", (c) => out.push(c));
          up.on("end", () => {
            const answer = Buffer.concat(out);
            if (listMatch && up.statusCode === 200) lastList.set(listMatch[1], { status: 200, type: up.headers["content-type"], body: answer });
            if (req.method === "POST" && /^\/api\/e2e\/v3\/(messages|group-messages)(\?|$)/.test(req.url)) {
              try {
                const j = JSON.parse(body.toString("utf8"));
                const to = (j.messages ?? j.recipients ?? []).map((m) => `${m.userId}/${m.deviceId}`);
                const bodies = (j.messages ?? []).map((m) => m.body);
                let reply = null;
                try {
                  reply = JSON.parse(answer.toString("utf8"));
                } catch {}
                sends.push({ from: j.senderDeviceId, path: req.url, allDevices: j.allDevices === true, to, status: up.statusCode, bodies, reply });
              } catch {}
            }
            res.writeHead(up.statusCode, up.headers);
            res.end(answer);
          });
        },
      );
      upstream.on("error", (e) => {
        res.writeHead(502);
        res.end(String(e));
      });
      upstream.end(body);
    });
  });
  return new Promise((r) => proxy.listen(PORT, "127.0.0.1", r));
}

class Api {
  constructor() {
    this.cookies = new Map();
  }
  async call(method, path, body) {
    const headers = body ? { "content-type": "application/json" } : {};
    if (this.cookies.size) headers.cookie = [...this.cookies.values()].join("; ");
    const res = await fetch(`http://127.0.0.1:${SERVER_PORT}${path}`, { method, headers, body: body ? JSON.stringify(body) : undefined });
    for (const cookie of res.headers.getSetCookie?.() ?? []) {
      const pair = cookie.split(";")[0];
      this.cookies.set(pair.split("=")[0], pair);
    }
    const text = await res.text();
    let json = null;
    try {
      json = text ? JSON.parse(text) : null;
    } catch {}
    return { status: res.status, body: json };
  }
  get = (path) => this.call("GET", path);
  post = (path, body) => this.call("POST", path, body ?? {});
  put = (path, body) => this.call("PUT", path, body);
}

async function account(name) {
  const api = new Api();
  const username = `${name}${hex(4)}`;
  const password = `pw-${hex(12)}`;
  const legacyKey = sodium.randombytes_buf(32);
  const registered = await api.post("/api/auth/register", {
    username,
    password,
    publicKey: b64(sodium.crypto_scalarmult_base(legacyKey)),
    encryptedPrivateKey: b64(randomBytes(48)),
    privateKeyNonce: b64(randomBytes(24)),
    privateKeyKdfSalt: b64(randomBytes(16)),
    terms_accepted: true,
    privacy_acknowledged: true,
    age_18_confirmed: true,
  });
  if (registered.status !== 201) throw new Error(`register ${registered.status} ${JSON.stringify(registered.body)}`);
  const seed = sodium.crypto_kdf_derive_from_key(32, 1, "rvsignk1", legacyKey);
  const signing = sodium.crypto_sign_seed_keypair(seed);
  if ((await api.put("/api/auth/signing-key", { signingPublicKey: b64(signing.publicKey) })).status !== 204) throw new Error("signing key");
  return { name, api, userId: registered.body.userId, username, password, legacyKey: b64(legacyKey) };
}

async function befriend(a, b) {
  await a.api.post("/api/friend-requests", { username: b.username });
  const request = (await b.api.get("/api/friend-requests/incoming")).body[0];
  const accepted = await b.api.post(`/api/friend-requests/${request.id}/accept`, {
    participants: [a, b].map((u) => ({ userId: u.userId, wrappedKey: b64(randomBytes(80)) })),
  });
  if (accepted.status !== 201) throw new Error(`accept ${accepted.status} ${JSON.stringify(accepted.body)}`);
  return accepted.body.id;
}

// --- The apps ---

class Desktop {
  constructor(name, acct) {
    this.name = name;
    this.account = acct;
    this.dataDir = join(WORK, name);
    mkdirSync(this.dataDir, { recursive: true });
    this.start();
  }
  start() {
    this.child = spawn(WIN_PROBE, [], { stdio: ["pipe", "pipe", "inherit"], windowsHide: true, env: { ...process.env, PATH: `C:\\msys64\\ucrt64\\bin;${process.env.PATH}` } });
    this.pending = [];
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      if (!line.startsWith("{")) return;
      this.pending.shift()?.(JSON.parse(line));
    });
  }
  call(cmd) {
    return within(120_000, `${this.name} ${cmd.cmd}`, new Promise((resolve) => {
      this.pending.push(resolve);
      this.child.stdin.write(`${JSON.stringify(cmd)}\n`);
    }));
  }
  async must(cmd) {
    const answer = await this.call(cmd);
    if (!answer.ok) throw new Error(`${this.name} ${cmd.cmd}: ${answer.error} ${answer.message ?? ""}`);
    return answer;
  }
  init() {
    return this.must({ cmd: "init", baseUrl: `${SERVER}/api/`, dataDir: this.dataDir, username: this.account.username, password: this.account.password, legacyKey: this.account.legacyKey, vault: "default" });
  }
  async stop() {
    try {
      this.child.stdin.end();
    } catch {}
    await Promise.race([new Promise((r) => (this.child.exitCode !== null ? r() : this.child.on("exit", r))), sleep(3000)]);
    kill(this.child);
  }
  async restart() {
    await this.stop();
    this.start();
    return this.init();
  }
  async deviceId() {
    return (await this.must({ cmd: "status" })).deviceId;
  }
  async send(contact, conversationId, text) {
    return (await this.must({ cmd: "chatSend", conversationId, contactUserId: contact.userId, text })).delivered;
  }
  async texts(conversationId) {
    return (await this.must({ cmd: "chatReceive", conversationId })).items.map((m) => m.text);
  }
  signal() {
    return this.must({ cmd: "signal" });
  }
}

const adb = (...args) => sh(ADB, ["-s", SERIAL, ...args]);

class Android {
  constructor(name, acct, control, testPackage) {
    this.name = name;
    this.account = acct;
    this.control = control;
    this.testPackage = testPackage;
  }
  async start() {
    const waiting = this.control.next();
    this.instrument = spawn(
      ADB,
      ["-s", SERIAL, "shell", "am", "instrument", "-w", "-e", "signalProbePort", String(CONTROL_PORT), "-e", "class",
        "com.redvaultsystems.redvaultmessage.devicekeys.SignalProbeTest", `${this.testPackage}/androidx.test.runner.AndroidJUnitRunner`],
      { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
    );
    this.instrumentOut = "";
    this.instrument.stdout.on("data", (d) => (this.instrumentOut += d));
    try {
      this.conn = await within(90_000, "the Android driver connecting back", waiting);
    } catch (e) {
      throw new Error(`${e.message}; am instrument said: ${this.instrumentOut.slice(-1500) || "(nothing)"}`);
    }
    const hello = await within(30_000, "the Android driver's first line", this.conn.read());
    if (!hello.ready) throw new Error(`android probe didn't start: ${JSON.stringify(hello)}`);
    this.sdk = hello.sdk;
  }
  async call(cmd) {
    this.conn.write(cmd);
    return within(120_000, `${this.name} ${cmd.cmd}`, this.conn.read());
  }
  async must(cmd) {
    const answer = await this.call(cmd);
    if (!answer.ok) throw new Error(`${this.name} ${cmd.cmd}: ${answer.error} ${answer.message ?? ""}`);
    return answer.result;
  }
  init() {
    return this.must({ cmd: "init", baseUrl: `http://localhost:${PORT}/api/`, username: this.account.username, password: this.account.password, legacyKey: this.account.legacyKey });
  }
  async stop() {
    try {
      await this.call({ cmd: "quit" });
    } catch {}
    await new Promise((r) => (this.instrument.exitCode !== null ? r() : this.instrument.on("exit", r)));
  }
  async send(contact, conversationId, text) {
    const r = await this.must({ cmd: "chatSend", conversationId, contactUserId: contact.userId, text });
    return r.sent ? 1 : 0;
  }
  async texts() {
    return (await this.must({ cmd: "chatReceive" })).items.map((m) => m.text);
  }
  signal() {
    return this.must({ cmd: "signal" });
  }
}

function controlServer() {
  const queue = [];
  const server = createServer((socket) => {
    const lines = createInterface({ input: socket });
    const waiting = [];
    const buffered = [];
    lines.on("line", (line) => (waiting.length ? waiting.shift()(JSON.parse(line)) : buffered.push(JSON.parse(line))));
    const conn = {
      write: (obj) => socket.write(`${JSON.stringify(obj)}\n`),
      read: () => new Promise((r) => (buffered.length ? r(buffered.shift()) : waiting.push(r))),
    };
    queue.shift()?.(conn);
  });
  server.listen(CONTROL_PORT, "127.0.0.1");
  return { server, next: () => new Promise((r) => queue.push(r)) };
}

/** A desktop joins an account, approved by a trusted desktop of it (both compare the same codes). */
async function link(trusted, joining) {
  const begun = await joining.must({ cmd: "joinBegin" });
  const answered = trusted.must({ cmd: "answer", enrollmentId: begun.enrollmentId });
  const codes = (await joining.must({ cmd: "joinCodes" })).codes;
  const theirs = (await answered).codes;
  check("link: both desktops show the same codes", JSON.stringify(codes) === JSON.stringify(theirs), [codes, theirs]);
  const approved = trusted.must({ cmd: "approve" });
  const joined = await joining.must({ cmd: "joinConfirm" });
  await approved;
  return joined;
}

const sendsSince = (n) => sends.slice(n);
const signalOutcome = (r) => r.outcome ?? r.result?.outcome;

// --- The run ---

const desktops = [];
let android = null;
let control = null;

async function main() {
  console.log(`work folder ${WORK}`);
  for (const [what, path] of [["Windows probe", WIN_PROBE], ["APK", APK], ["test APK", TEST_APK]]) {
    if (!existsSync(path)) throw new Error(`${what} missing: ${path}`);
  }
  await startPostgres();
  await startServer();
  await startProxy();
  console.log("server up (protocol 3 + rv-signal-v1, throwaway Postgres), recording proxy in front");

  for (const p of [String(PORT), String(CONTROL_PORT)]) adb("reverse", `tcp:${p}`, `tcp:${p}`);
  control = controlServer();
  const installed = adb("install", "-r", "-t", APK);
  const installedTest = adb("install", "-r", "-t", TEST_APK);
  check("android: app and test APKs install on the emulator", installed.status === 0 && installedTest.status === 0, installed.stdout + installed.stderr + installedTest.stderr);
  const instrumentation = adb("shell", "pm", "list", "instrumentation").stdout;
  const testPackage = instrumentation.split(/\r?\n/).find((l) => l.includes("(target=com.redvaultsystems.redvaultmessage"))?.match(/^instrumentation:([^/]+)\//)?.[1];
  check("android: the probe's instrumentation is installed", Boolean(testPackage), instrumentation);

  step("accounts: alice (Android), bob (two Windows desktops), carol (one Windows desktop)");
  const alice = await account("alice");
  const bob = await account("bob");
  const carol = await account("carol");
  const ab = await befriend(alice, bob);
  const ac = await befriend(alice, carol);

  const A = new Android("A", alice, control, testPackage);
  android = A;
  await A.start();
  check(`android: the driver runs on the emulator (API ${A.sdk})`, A.sdk >= 26);
  const B1 = new Desktop("bob-pc1", bob);
  const C = new Desktop("carol-pc", carol);
  desktops.push(B1, C);
  for (const d of [A, B1, C]) await d.init();
  for (const d of [A, B1, C]) await d.must({ cmd: "setup" });
  for (const d of [A, B1, C]) {
    const outcome = signalOutcome(await d.signal());
    check(`${d.name}: Signal on and declared (${outcome})`, outcome === "enabled" || outcome === "declared");
  }
  const config = (await alice.api.get("/api/e2e/v3/config")).body;
  check("server: e2e/v3/config advertises allDevices", config?.allDevices === true, config);
  const b1Id = await B1.deviceId();

  // ---------------- Gap 1 ----------------
  step("gap 1: ordinary 1:1 sends go out marked and are accepted");
  let mark = sends.length;
  check("alice -> bob delivered", (await A.send(bob, ab, "hello bob")) >= 1);
  check("bob's desktop reads it", (await B1.texts(ab)).includes("hello bob"));
  check("bob -> alice delivered", (await B1.send(alice, ab, "hi alice")) >= 1);
  check("alice reads it", (await A.texts()).includes("hi alice"));
  let got = sendsSince(mark);
  check("both sends went out marked allDevices, both 201", got.length === 2 && got.every((s) => s.allDevices && s.status === 201), got.map(({ bodies, ...s }) => s));
  const bobListBefore = lastList.get(bob.userId);
  check("alice's app fetched bob's list (kept for the race below)", Boolean(bobListBefore));

  step("gap 1: bob links a second desktop");
  const B2 = new Desktop("bob-pc2", bob);
  desktops.push(B2);
  await B2.init();
  const joined = await link(B1, B2);
  check("bob's list now has two devices", joined.devices === 2, joined);
  check("bob-pc2: Signal on and declared", ["enabled", "declared"].includes(signalOutcome(await B2.signal())));
  await B1.signal();
  const b2Id = await B2.deviceId();

  step("gap 1, the race: alice's app gets bob's list from before the link, then sends");
  replayOnce.set(bob.userId, bobListBefore);
  mark = sends.length;
  check("alice -> bob delivered", (await A.send(bob, ab, "after the link")) >= 1);
  got = sendsSince(mark);
  const first = got[0];
  const second = got[1];
  check("first try: marked, only bob-pc1, refused 409 device_list_stale", first?.allDevices && first.status === 409 && first.reply?.error === "device_list_stale" && first.to.length === 1, got.map(({ bodies, ...s }) => s));
  check("the refusal names bob-pc2 as missing", JSON.stringify(first?.reply?.details?.missing ?? []) === JSON.stringify([{ userId: bob.userId, deviceId: b2Id }]), first?.reply);
  check("retry: marked, bob-pc1 and bob-pc2, 201", second?.allDevices && second.status === 201 && second.to.includes(`${bob.userId}/${b1Id}`) && second.to.includes(`${bob.userId}/${b2Id}`), second && { ...second, bodies: undefined });
  check("bob-pc2 (the new desktop) reads the message", (await B2.texts(ab)).includes("after the link"));
  check("bob-pc1 reads it too", (await B1.texts(ab)).includes("after the link"));

  step("gap 1: bob's sends now include his other desktop");
  mark = sends.length;
  check("bob-pc1 -> alice delivered", (await B1.send(alice, ab, "from pc1")) >= 1);
  got = sendsSince(mark);
  check("marked, to alice and bob-pc2, 201", got.length === 1 && got[0].allDevices && got[0].status === 201 && got[0].to.includes(`${bob.userId}/${b2Id}`), got.map(({ bodies, ...s }) => s));
  check("alice reads it", (await A.texts()).includes("from pc1"));
  check("bob-pc2 has its own copy", (await B2.texts(ab)).includes("from pc1"));

  // ---------------- Gap 2 ----------------
  step("gap 2: alice and carol talk; carol's desktop data is copied aside");
  check("alice -> carol", (await A.send(carol, ac, "hello carol")) >= 1);
  check("carol reads it", (await C.texts(ac)).includes("hello carol"));
  check("carol -> alice", (await C.send(alice, ac, "hi")) >= 1);
  check("alice reads it", (await A.texts()).includes("hi"));
  await C.stop();
  const snapshot = join(WORK, "carol-pc.snapshot");
  cpSync(C.dataDir, snapshot, { recursive: true });
  C.start();
  await C.init();

  step("gap 2: three more rounds, then carol's desktop comes back from the old copy");
  for (let i = 0; i < 3; i++) {
    await A.send(carol, ac, `a${i}`);
    check(`round ${i}: carol reads a${i}`, (await C.texts(ac)).includes(`a${i}`));
    await C.send(alice, ac, `c${i}`);
    check(`round ${i}: alice reads c${i}`, (await A.texts()).includes(`c${i}`));
  }
  await C.stop();
  rmSync(C.dataDir, { recursive: true, force: true });
  cpSync(snapshot, C.dataDir, { recursive: true });
  C.start();
  await C.init();
  const carolId = await C.deviceId();

  step("gap 2: alice's next message can't be opened; carol's app heals the session");
  check("alice -> carol (lost)", (await A.send(carol, ac, "lost after the restore")) >= 1);
  mark = sends.length;
  const atCarol = await C.texts(ac);
  check("carol's desktop shows nothing for it", !atCarol.includes("lost after the restore"), atCarol);
  got = sendsSince(mark);
  const refresh = got.find((s) => s.from === carolId);
  check("carol's app sent one refresh: unmarked, to alice's one device, 201", got.length === 1 && refresh && !refresh.allDevices && refresh.to.length === 1 && refresh.to[0].startsWith(`${alice.userId}/`) && refresh.status === 201, got.map(({ bodies, ...s }) => s));
  let refreshType = null;
  try {
    refreshType = JSON.parse(refresh?.bodies?.[0] ?? "{}").type;
  } catch {}
  check("the refresh starts a new session (a prekey message)", refreshType === "prekey", refreshType);
  const atAlice = await A.texts();
  check("alice's phone shows nothing for the refresh", atAlice.length === 0, atAlice);

  step("gap 2: both directions work again");
  check("alice -> carol", (await A.send(carol, ac, "after the heal")) >= 1);
  check("carol reads it", (await C.texts(ac)).includes("after the heal"));
  check("carol -> alice", (await C.send(alice, ac, "carol is back")) >= 1);
  check("alice reads it", (await A.texts()).includes("carol is back"));
  mark = sends.length;
  await A.send(carol, ac, "lost again?");
  await C.texts(ac);
  check("no second refresh while things work", sendsSince(mark).filter((s) => s.from === carolId).length === 0);

  step("gap 2, the other way: alice's phone comes back from an old copy of its data");
  const aliceDevice = sends.find((s) => s.to.some((t) => t.startsWith(`${carol.userId}/`)) && s.from !== carolId)?.from;
  const pkg = "com.redvaultsystems.redvaultmessage";
  const probeDir = `no_backup/signal-probe/${alice.userId}`;
  const snapDir = `no_backup/sesame-snapshot-${alice.userId}`;
  await A.stop();
  const copied = adb("shell", "run-as", pkg, "cp", "-r", probeDir, snapDir);
  check("android: the phone's device data copied aside", copied.status === 0, copied.stderr);
  await A.start();
  await A.init();
  for (let i = 0; i < 3; i++) {
    await C.send(alice, ac, `c-late${i}`);
    check(`round ${i}: alice reads c-late${i}`, (await A.texts()).includes(`c-late${i}`));
    await A.send(carol, ac, `a-late${i}`);
    check(`round ${i}: carol reads a-late${i}`, (await C.texts(ac)).includes(`a-late${i}`));
  }
  await A.stop();
  const restored = [
    adb("shell", "run-as", pkg, "rm", "-rf", probeDir),
    adb("shell", "run-as", pkg, "cp", "-r", snapDir, probeDir),
    adb("shell", "run-as", pkg, "rm", "-rf", snapDir),
  ];
  check("android: the old copy put back", restored.every((r) => r.status === 0), restored.map((r) => r.stderr).join(" "));
  await A.start();
  await A.init();
  check("carol -> alice (lost)", (await C.send(alice, ac, "lost on the phone")) >= 1);
  mark = sends.length;
  const atPhone = await A.texts();
  check("alice's phone shows nothing for it", !atPhone.includes("lost on the phone"), atPhone);
  got = sendsSince(mark);
  check("alice's app sent one refresh: unmarked, to carol's one device, 201",
    got.length === 1 && got[0].from === aliceDevice && !got[0].allDevices && got[0].to.length === 1 && got[0].to[0] === `${carol.userId}/${carolId}` && got[0].status === 201,
    got.map(({ bodies, ...s }) => s));
  check("carol's desktop shows nothing for the refresh", (await C.texts(ac)).length === 0);
  check("carol -> alice", (await C.send(alice, ac, "phone healed?")) >= 1);
  check("alice reads it", (await A.texts()).includes("phone healed?"));
  check("alice -> carol", (await A.send(carol, ac, "yes, from the phone")) >= 1);
  check("carol reads it", (await C.texts(ac)).includes("yes, from the phone"));

  step("old apps: an unmarked send that leaves out a device is still accepted");
  const unmarked = await bob.api.post("/api/e2e/v3/messages", { senderDeviceId: b1Id, conversationId: ab, messages: [{ userId: bob.userId, deviceId: b2Id, body: "x" }] });
  check("server: 201 for an unmarked partial send (as from desktop 0.9.0)", unmarked.status === 201, unmarked);
  await B2.texts(ab);

  const log = sh("cmd", ["/c", "type", join(WORK, "server.log")]).stdout ?? "";
  const secrets = ["hello bob", "after the link", "lost after the restore", "after the heal", "carol is back"];
  check("server: no plaintext in the server log", !secrets.some((s) => log.includes(s)));
  console.log(`\n  sends seen by the proxy: ${sends.length} (${sends.filter((s) => s.allDevices).length} marked, ${sends.filter((s) => s.status === 409).length} refused)`);
}

let exitCode = 0;
try {
  await main();
} catch (e) {
  console.log(`  FAIL run stopped: ${e.stack ?? e}`);
  exitCode = 1;
} finally {
  for (const d of desktops) await d.stop();
  if (android) await Promise.race([android.stop(), sleep(5000)]);
  control?.server.close();
  proxy?.close();
  for (const p of [String(PORT), String(CONTROL_PORT)]) adb("reverse", "--remove", `tcp:${p}`);
  kill(serverProcess);
  await db?.end();
  if (pgProcess) sh(pgTool("pg_ctl"), ["-D", PGDATA, "-m", "immediate", "stop"]);
  const failed = results.filter((r) => !r.ok).length;
  console.log(`\n${results.length - failed}/${results.length} checks passed${failed ? `, ${failed} failed` : ""}`);
  if (failed) exitCode = 1;
}
process.exit(exitCode);
