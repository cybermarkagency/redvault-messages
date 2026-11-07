// Protocol 3 on the desktop, end to end against the REAL server code on a
// throwaway PostgreSQL cluster (development only; nothing deployed, no real
// account). Desktop computers are redvault_v3_probe processes (the same library
// the Settings panel uses, over HTTP, with this machine's key protection);
// the "web" device is the engine's WebAssembly build in Node doing what
// ui/src/lib/e2eV3 does.
//
//   node desktop/tools/v3_probe/run.mjs <path to redvault_v3_probe.exe>
//
// Needs: PostgreSQL 17 (initdb/pg_ctl/psql under Program Files or on PATH),
// server/node_modules and ui/node_modules (libsodium), and
// crypto-core/interop/build/node (wasm-bindgen --target nodejs build of crypto-core).
import { spawn, spawnSync } from "node:child_process";
import { randomBytes, randomUUID } from "node:crypto";
import { createWriteStream, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const HERE = fileURLToPath(new URL(".", import.meta.url));
const ROOT = join(HERE, "..", "..", "..");
const PROBE = process.argv[2];
if (!PROBE || !existsSync(PROBE)) throw new Error("usage: node run.mjs <redvault_v3_probe>");
const PORT = Number(process.env.V3_DEV_PORT ?? 8791);
const PG_PORT = Number(process.env.V3_PG_PORT ?? 5461);
const WORK = mkdtempSync(join(tmpdir(), "rv-desktop-v3-"));
const SERVER = `http://127.0.0.1:${PORT}`;
// The server code under test: this checkout, or a tree with the chat routes (git archive of the server branch).
const TREE = process.env.V3_SERVER_TREE ?? ROOT;

const sodium = createRequire(join(ROOT, "ui", "package.json"))("libsodium-wrappers-sumo");
await sodium.ready;
const pg = createRequire(join(ROOT, "server", "package.json"))("pg");
const engine = createRequire(import.meta.url)(join(ROOT, "crypto-core", "interop", "build", "node", "redvault_e2ee.js"));

const b64 = (bytes) => Buffer.from(bytes).toString("base64");
const hex = (n) => randomBytes(n).toString("hex");
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: Boolean(ok) });
  const text = typeof detail === "string" ? detail : JSON.stringify(detail);
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${ok || !text ? "" : ` -- ${text.slice(0, 300)}`}`);
}

// --- PostgreSQL: a throwaway cluster ---

function pgTool(name) {
  const dir = ["C:/Program Files/PostgreSQL/17/bin", "C:/Program Files/PostgreSQL/16/bin"].find((d) => existsSync(join(d, `${name}.exe`)));
  return dir ? join(dir, `${name}.exe`) : name;
}
function sh(command, args, options = {}) {
  const r = spawnSync(command, args, { encoding: "utf8", windowsHide: true, ...options });
  return r;
}

const PGDATA = join(WORK, "pgdata");
let pgProcess = null;
let dbUrl;
function startPostgres() {
  let r = sh(pgTool("initdb"), ["-D", PGDATA, "-U", "postgres", "-A", "trust", "-E", "UTF8"]);
  if (r.status !== 0) throw new Error(`initdb: ${r.stderr}`);
  // Started detached with no inherited pipes (pg_ctl -w would wait on them), then polled until it answers.
  pgProcess = spawn(pgTool("postgres"), ["-D", PGDATA, "-p", String(PG_PORT), "-c", "listen_addresses=127.0.0.1"], { stdio: ["ignore", "ignore", "ignore"], windowsHide: true });
  for (let i = 0; i < 100; i++) {
    const ping = sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "postgres", "-c", "select 1"]);
    if (ping.status === 0) break;
    if (i === 99) throw new Error("postgres didn't start");
    spawnSync(process.execPath, ["-e", "setTimeout(()=>{},300)"]);
  }
  sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-c", "CREATE DATABASE v3desktop"]);
  const files = [join(ROOT, "db", "schema.sql"), ...readdirSync(join(ROOT, "db", "migrations")).sort().map((f) => join(ROOT, "db", "migrations", f))];
  for (const file of files) {
    // 005's first DELETE fails harmlessly on an empty database; everything else must apply.
    sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "v3desktop", "-q", "-f", file]);
  }
  dbUrl = `postgres://postgres@127.0.0.1:${PG_PORT}/v3desktop`;
}
function stopPostgres() {
  if (pgProcess) sh(pgTool("pg_ctl"), ["-D", PGDATA, "-m", "immediate", "stop"]);
}

// --- The real API ---

let serverProcess = null;
const SERVER_SECRETS = { V3_DEV_PEPPER: randomBytes(32).toString("base64"), V3_DEV_USERNAME_KEY: randomBytes(32).toString("base64") };
const SERVER_LOG = join(WORK, "server.log");
const logStream = createWriteStream(SERVER_LOG, { flags: "a" });

async function startServer(enabled = true) {
  const tsx = join(TREE, "server", "node_modules", "tsx", "dist", "cli.mjs");
  serverProcess = spawn(process.execPath, [tsx, join(TREE, "crypto-core", "interop", "server", "dev-server.mts")], {
    env: { ...process.env, ...SERVER_SECRETS, V3_DEV_DATABASE_URL: dbUrl, V3_DEV_PORT: String(PORT), V3_ENABLED: String(enabled), V3_RESET_DELAY_MS: "4000" },
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });
  serverProcess.stdout.pipe(logStream, { end: false });
  serverProcess.stderr.pipe(logStream, { end: false });
  for (let i = 0; i < 80; i++) {
    try {
      if ((await fetch(`${SERVER}/health`)).ok) return;
    } catch {}
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error("the dev API didn't start");
}
function stopServer() {
  if (!serverProcess) return;
  // By process id, never by name.
  if (process.platform === "win32") spawnSync("taskkill", ["/pid", String(serverProcess.pid), "/T", "/F"], { stdio: "ignore" });
  else serverProcess.kill();
  serverProcess = null;
}

class Api {
  constructor() {
    this.cookies = new Map();
  }
  async call(method, path, body) {
    const headers = body ? { "content-type": "application/json" } : {};
    if (this.cookies.size) headers.cookie = [...this.cookies.values()].join("; ");
    const res = await fetch(`${SERVER}${path}`, { method, headers, body: body ? JSON.stringify(body) : undefined });
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

// An account as it exists today (protocol 1): the dev API stores whatever wrapped key it is handed.
async function legacyAccount(name) {
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
  return { api, userId: registered.body.userId, username, password, legacyKey: b64(legacyKey) };
}

async function befriend(a, b) {
  await a.api.post("/api/friend-requests", { username: b.username });
  const incoming = (await b.api.get("/api/friend-requests/incoming")).body;
  const request = incoming[0];
  const accepted = await b.api.post(`/api/friend-requests/${request.id}/accept`, {
    participants: [a, b].map((u) => ({ userId: u.userId, wrappedKey: b64(randomBytes(80)) })),
  });
  if (accepted.status !== 201) throw new Error(`accept ${accepted.status} ${JSON.stringify(accepted.body)}`);
  return accepted.body.id;
}

// --- A desktop computer: one probe process ---

class Desktop {
  constructor(name, account, vault = "default") {
    this.name = name;
    this.account = account;
    this.dataDir = join(WORK, name);
    mkdirSync(this.dataDir, { recursive: true });
    this.vault = vault;
    this.child = spawn(PROBE, [], { stdio: ["pipe", "pipe", "inherit"], windowsHide: true });
    this.pending = [];
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      const next = this.pending.shift();
      if (next) next(JSON.parse(line));
    });
  }
  call(cmd) {
    return new Promise((resolve) => {
      this.pending.push(resolve);
      this.child.stdin.write(`${JSON.stringify(cmd)}\n`);
    });
  }
  async must(cmd) {
    const answer = await this.call(cmd);
    if (!answer.ok) throw new Error(`${this.name} ${cmd.cmd}: ${answer.error} ${answer.message ?? ""}`);
    return answer;
  }
  init() {
    return this.must({
      cmd: "init",
      baseUrl: `${SERVER}/api/`,
      dataDir: this.dataDir,
      username: this.account.username,
      password: this.account.password,
      legacyKey: this.account.legacyKey,
      vault: this.vault,
    });
  }
  stop() {
    this.child.stdin.end();
    this.child.kill();
  }
}

// --- The web device: the engine's WebAssembly build, doing what ui/src/lib/e2eV3 does ---

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

class Web {
  constructor(name, account) {
    this.name = name;
    this.account = account;
    this.api = account.api;
    this.userId = account.userId;
    this.deviceId = randomUUID();
    this.pickleKey = randomBytes(32).toString("base64");
    this.state = null;
  }
  run(request) {
    return JSON.parse(engine.call(JSON.stringify(request)));
  }
  op(op, fields = {}) {
    const r = this.run({ op, state: this.state, pickleKey: this.pickleKey, ...fields });
    if (r.ok && r.state) this.state = r.state;
    return r;
  }
  must(op, fields = {}) {
    const r = this.op(op, fields);
    if (!r.ok) throw new Error(`${this.name} ${op}: ${r.error} ${r.message ?? ""}`);
    return r;
  }
  async register(count = 10) {
    const { upload } = this.must("prekeys", { count });
    const res = await this.api.put(`/api/e2e/v3/devices/${this.deviceId}`, upload);
    if (res.status !== 200) throw new Error(`register ${res.status} ${JSON.stringify(res.body)}`);
  }
  async createAccount() {
    const r = this.run({ op: "createAccount", userId: this.userId, deviceId: this.deviceId, pickleKey: this.pickleKey, legacyAccountKey: this.account.legacyKey });
    if (!r.ok) throw new Error(`createAccount ${r.error}`);
    this.state = r.state;
    await this.register();
    const put = await this.api.put("/api/e2e/v3/device-list", { deviceList: r.deviceList, rotationProof: null });
    if (put.status !== 200) throw new Error(`put list ${put.status} ${JSON.stringify(put.body)}`);
  }
  async createDevice() {
    const r = this.run({ op: "createDevice", userId: this.userId, deviceId: this.deviceId, pickleKey: this.pickleKey });
    if (!r.ok) throw new Error(`createDevice ${r.error}`);
    this.state = r.state;
    await this.register();
  }
  async list(userId, conversationId = null) {
    const query = conversationId ? `?conversationId=${conversationId}` : "";
    return (await this.api.get(`/api/e2e/v3/users/${userId}/device-list${query}`)).body;
  }
  identity() {
    return this.must("identity").identity;
  }
  async putList(made) {
    let res = await this.api.put("/api/e2e/v3/device-list", { deviceList: made.deviceList, rotationProof: null });
    return res;
  }

  // Pairing, as ui/src/lib/e2eV3/pairing.ts
  async post(id, body) {
    return this.api.post(`/api/e2e/v3/enrollments/${id}/messages`, { fromDeviceId: this.deviceId, body: JSON.stringify(body) });
  }
  async waitFor(id, type) {
    for (let i = 0; i < 200; i++) {
      for (const m of (await this.api.get(`/api/e2e/v3/enrollments/${id}/messages?after=0`)).body ?? []) {
        if (m.fromDeviceId === this.deviceId) continue;
        const body = JSON.parse(m.body);
        if (body.type === type) return body;
      }
      await sleep(100);
    }
    throw new Error(`${this.name} waited for ${type}`);
  }
  async joinBegin() {
    this.enrollment = (await this.api.post("/api/e2e/v3/enrollments", { newDeviceId: this.deviceId })).body;
    this.sas = this.run({ op: "sasStart", pairingId: this.enrollment.id, role: "new" });
    await this.post(this.enrollment.id, { type: "commit", commitment: this.sas.commitment, identity: this.identity() });
    return this.enrollment.id;
  }
  async joinCodes() {
    const key = await this.waitFor(this.enrollment.id, "key");
    const revealed = this.run({ op: "sasReveal", sasId: this.sas.sasId });
    await this.post(this.enrollment.id, { type: "reveal", publicKey: revealed.publicKey });
    const est = this.run({ op: "sasEstablish", sasId: this.sas.sasId, theirPublicKey: key.publicKey, newDevice: this.identity(), trustedDevice: key.identity });
    if (!est.ok) throw new Error(`sasEstablish ${est.error}`);
    return est.decimals;
  }
  async joinConfirm() {
    const id = this.enrollment.id;
    const ours = this.must("sasMac", { sasId: this.sas.sasId });
    await this.post(id, { type: "mac", mac: ours.mac });
    const theirs = await this.waitFor(id, "mac");
    this.must("sasConfirm", { sasId: this.sas.sasId, mac: theirs.mac });
    const offer = this.must("linkOffer");
    await this.post(id, { type: "offer", qr: offer.qrText });
    for (let i = 0; i < 200; i++) {
      const items = (await this.api.get(`/api/e2e/v3/devices/${this.deviceId}/messages`)).body ?? [];
      for (const item of items) {
        const lst = (await this.list(item.senderUserId)).deviceList;
        const sender = lst?.devices.find((d) => d.deviceId === item.senderDeviceId);
        const r = this.op("decrypt", { sender: sender ?? undefined, message: JSON.parse(item.body) });
        await this.api.post(`/api/e2e/v3/devices/${this.deviceId}/messages/ack`, { ids: [item.id] });
        if (r.ok && r.kind === "link") {
          await this.register(10);
          await this.api.call("DELETE", `/api/e2e/v3/enrollments/${id}`);
          return this.must("deviceList").deviceList;
        }
      }
      await sleep(100);
    }
    throw new Error("link never came");
  }
  async answerBegin(id) {
    const commit = await this.waitFor(id, "commit");
    const started = this.run({ op: "sasStart", pairingId: id, role: "trusted" });
    const identity = this.identity();
    await this.post(id, { type: "key", publicKey: started.publicKey, identity });
    const reveal = await this.waitFor(id, "reveal");
    const est = this.run({
      op: "sasEstablish",
      sasId: started.sasId,
      theirPublicKey: reveal.publicKey,
      theirCommitment: commit.commitment,
      newDevice: commit.identity,
      trustedDevice: identity,
    });
    if (!est.ok) throw new Error(`sasEstablish ${est.error}`);
    this.answering = { id, sasId: started.sasId, newDevice: commit.identity };
    return est.decimals;
  }
  async answerConfirm() {
    const { id, sasId, newDevice } = this.answering;
    const ours = this.must("sasMac", { sasId });
    await this.post(id, { type: "mac", mac: ours.mac });
    const theirs = await this.waitFor(id, "mac");
    this.must("sasConfirm", { sasId, mac: theirs.mac });
    const offer = await this.waitFor(id, "offer");
    const approved = this.must("approveDevice", { qr: offer.qr, verifiedBy: "sas" });
    const put = await this.api.put("/api/e2e/v3/device-list", { deviceList: approved.deviceList, rotationProof: null });
    if (put.status !== 200) throw new Error(`approve put ${put.status} ${JSON.stringify(put.body)}`);
    await this.api.post("/api/e2e/v3/messages", {
      senderDeviceId: this.deviceId,
      conversationId: null,
      messages: [{ userId: newDevice.userId, deviceId: newDevice.deviceId, body: JSON.stringify(approved.message) }],
    });
    return approved.deviceList;
  }

  // Messages, as device.ts sendText / receive
  async devicesOf(userId, conversationId) {
    const view = await this.list(userId, conversationId);
    const r = this.must("verifyDeviceList", { deviceList: view.deviceList });
    return r.devices;
  }
  async send(conversationId, contactUserId, text) {
    const theirs = await this.devicesOf(contactUserId, conversationId);
    const mine = (await this.devicesOf(this.userId, null)).filter((d) => d.deviceId !== this.deviceId);
    const recipients = [...theirs, ...mine];
    const first = this.must("encryptMany", { plaintext: text, recipients: recipients.map((identity) => ({ identity })) });
    let results = first.results;
    const need = recipients.filter((_, i) => results[i].error === "no_session");
    if (need.length) {
      const claimed = (await this.api.post("/api/e2e/v3/claim", { conversationId, devices: need.map((d) => ({ userId: d.userId, deviceId: d.deviceId })) })).body;
      const retry = this.must("encryptMany", { plaintext: text, recipients: need.map((identity, i) => ({ identity, oneTimeKey: claimed[i].oneTimeKey ?? claimed[i].fallbackKey })) });
      let j = 0;
      results = results.map((r) => (r.error === "no_session" ? retry.results[j++] : r));
    }
    const ok = results.filter((r) => !r.error && r.message);
    await this.api.post("/api/e2e/v3/messages", {
      senderDeviceId: this.deviceId,
      conversationId,
      messages: ok.map((r) => ({ userId: r.userId, deviceId: r.deviceId, body: JSON.stringify(r.message) })),
    });
    return ok.length;
  }
  async receive() {
    const items = (await this.api.get(`/api/e2e/v3/devices/${this.deviceId}/messages`)).body ?? [];
    const out = [];
    for (const item of items) {
      const view = await this.list(item.senderUserId, item.conversationId);
      const verified = this.op("verifyDeviceList", { deviceList: view.deviceList });
      const sender = verified.ok ? verified.devices.find((d) => d.deviceId === item.senderDeviceId) : null;
      const r = sender ? this.op("decrypt", { sender, message: JSON.parse(item.body) }) : { ok: false, error: "unknown_sender" };
      out.push({ sender: item.senderUserId, text: r.ok ? r.plaintext : null, error: r.ok ? null : r.error });
    }
    if (items.length) await this.api.post(`/api/e2e/v3/devices/${this.deviceId}/messages/ack`, { ids: items.map((i) => i.id) });
    return out;
  }
}

// --- The run ---

const secrets = [];
const desktops = [];
try {
  console.log(`work folder ${WORK}`);
  startPostgres();
  await startServer(true);
  console.log("real API up on a throwaway PostgreSQL 17");

  const alice = await legacyAccount("alice");
  const bob = await legacyAccount("bob");
  secrets.push(alice.legacyKey, bob.legacyKey);
  const chat = await befriend(alice, bob);

  console.log("\n1. A desktop computer becomes the first trusted device");
  const d1 = new Desktop("d1", alice);
  desktops.push(d1);
  const started = await d1.init();
  check("the desktop picks the best key protection for this machine", process.platform !== "win32" || started.protection === 0, started);
  check("protocol 3 is on at the server", (await d1.must({ cmd: "config" })).enabled === true);
  check("a fresh computer has no device yet", started.hasDevice === false);
  await d1.must({ cmd: "setup" });
  let status = await d1.must({ cmd: "status" });
  check("the desktop is the account's trusted device, list version 1", status.trusted === true && status.version === 1, status);
  check("the server recorded the device list", status.events.includes("device_list_created") && status.events.includes("device_registered"), status.events);
  // The app restarts: the device comes back from disk and the key vault alone.
  const reopened = await d1.must({ cmd: "reopen" });
  check("after a restart the device opens from DPAPI / keyring + the state file", reopened.hasDevice === true && reopened.deviceId === status.deviceId, reopened);

  console.log("\n2. A web browser joins the desktop's account (the desktop approves by comparing codes)");
  const w1 = new Web("w1", alice);
  await w1.createDevice();
  const enrollmentId = await w1.joinBegin();
  const enrollments = await d1.must({ cmd: "enrollments" });
  check("the request is listed for the desktop", enrollments.items.some((e) => e.newDeviceId === w1.deviceId), enrollments);
  const answerP = d1.must({ cmd: "answer", enrollmentId });
  const webCodes = await w1.joinCodes();
  const desktopCodes = (await answerP).codes;
  check("both screens show the same numbers", JSON.stringify(webCodes) === JSON.stringify(desktopCodes), [webCodes, desktopCodes]);
  const approveP = d1.must({ cmd: "approve" });
  const joined = await w1.joinConfirm();
  const approved = await approveP;
  check("the web browser holds the list with both devices", joined.devices.length === 2 && joined.version === 2, joined);
  check("the desktop reports the approved list", approved.devices === 2 && approved.version === 2, approved);

  console.log("\n3. A second desktop joins, approved by the web browser");
  const d2 = new Desktop("d2", alice, process.platform === "win32" ? "default" : "file");
  desktops.push(d2);
  await d2.init();
  const begun = await d2.must({ cmd: "joinBegin" });
  const answeredP = w1.answerBegin(begun.enrollmentId);
  const d2Codes = (await d2.must({ cmd: "joinCodes" })).codes;
  const w1Codes = await answeredP;
  check("both screens show the same numbers", JSON.stringify(d2Codes) === JSON.stringify(w1Codes), [d2Codes, w1Codes]);
  const confirmP = d2.must({ cmd: "joinConfirm" });
  const approvedList = await w1.answerConfirm();
  const joinedList = await confirmP;
  check("the second desktop is trusted", joinedList.devices === 3 && approvedList.devices.length === 3, [joinedList, approvedList.version]);
  status = await d2.must({ cmd: "status" });
  check("it knows it is linked and trusted", status.linked === true && status.trusted === true, status);

  console.log("\n4. One-to-one messages, with copies for the sender's other devices");
  const b1 = new Web("b1", bob);
  await b1.createAccount();
  const sent = await d1.must({ cmd: "send", conversationId: chat, contactUserId: bob.userId, text: "hello bob, from the desktop" });
  check("one message, three copies (bob's browser, alice's browser, alice's second desktop)", sent.delivered === 3, sent);
  const atBob = await b1.receive();
  check("bob's browser reads it", atBob.length === 1 && atBob[0].text === "hello bob, from the desktop" && atBob[0].sender === alice.userId, atBob);
  const atW1 = await w1.receive();
  check("alice's browser reads her own copy", atW1.some((m) => m.text === "hello bob, from the desktop"), atW1);
  const atD2 = (await d2.must({ cmd: "receive" })).items;
  check("alice's second desktop reads her own copy", atD2.some((m) => m.text === "hello bob, from the desktop" && !m.error), atD2);
  const replied = await b1.send(chat, alice.userId, "hi alice, from the browser");
  check("bob's reply reaches all three of alice's devices", replied === 3, replied);
  const atD1 = (await d1.must({ cmd: "receive" })).items;
  check("the first desktop reads the reply", atD1.some((m) => m.text === "hi alice, from the browser" && !m.error), atD1);
  check("nothing is left in the mailbox once read", (await d1.must({ cmd: "receive" })).items.length === 0);
  // The app restarts between messages.
  await d1.must({ cmd: "reopen" });
  const again = await d1.must({ cmd: "send", conversationId: chat, contactUserId: bob.userId, text: "after a restart" });
  check("after a restart the desktop keeps sending on the same sessions", again.delivered === 3, again);
  check("bob reads it", (await b1.receive()).some((m) => m.text === "after a restart"));
  const desktopNumber = await d1.must({ cmd: "safety", contactUserId: bob.userId, conversationId: chat });
  const webNumber = await b1.must("fingerprint", { userId: alice.userId });
  check("desktop and web show the same device-key safety number", JSON.stringify(desktopNumber.groups) === JSON.stringify(webNumber.safetyNumber) && desktopNumber.groups.length === 12, [desktopNumber, webNumber.safetyNumber]);

  console.log("\n4b. The v3 chat path: envelopes, local history, pins");
  const envelope = (conv, text) => JSON.stringify({ v: 1, t: "text", id: randomUUID(), c: conv, ts: Date.now(), body: text });
  const chatSent = await d1.must({ cmd: "chatSend", conversationId: chat, contactUserId: bob.userId, text: "enveloped hello" });
  check("a chat message goes to three devices", chatSent.delivered === 3, chatSent);
  const bobGot = await b1.receive();
  const bobEnvelope = JSON.parse(bobGot.find((m) => m.text?.includes("enveloped hello"))?.text ?? "{}");
  check("bob's browser reads the envelope with the right chat id", bobEnvelope.v === 1 && bobEnvelope.t === "text" && bobEnvelope.c === chat && bobEnvelope.body === "enveloped hello", bobEnvelope);
  const d2Chat = await d2.must({ cmd: "chatReceive", conversationId: chat });
  check("the second desktop files its copy as sent by alice, in the chat", d2Chat.items.some((m) => m.text === "enveloped hello" && m.direction === "out" && m.conversationId === chat) && d2Chat.pinned, d2Chat);
  await b1.send(chat, alice.userId, envelope(chat, "browser says hi"));
  const d1Chat = await d1.must({ cmd: "chatReceive", conversationId: chat });
  check("the first desktop files bob's message as received", d1Chat.items.some((m) => m.text === "browser says hi" && m.direction === "in" && m.from === bob.userId), d1Chat);
  await b1.send(chat, alice.userId, envelope("00000000-0000-4000-8000-000000000000", "wrong chat"));
  const dropped = await d1.must({ cmd: "chatReceive", conversationId: chat });
  check("a message whose envelope names another chat is dropped, not shown", dropped.items.length === 0, dropped);

  if (process.env.V3_SERVER_TREE) {
    console.log("\n4c. Groups and the live event, against the server branch with the chat routes");
    const gbob = await legacyAccount("gbob");
    const gcarol = await legacyAccount("gcarol");
    secrets.push(gbob.legacyKey, gcarol.legacyKey);
    await befriend(alice, gbob);
    await befriend(alice, gcarol);
    const created = await alice.api.post("/api/conversations", {
      isGroup: true,
      nameCiphertext: b64(randomBytes(24)),
      nameNonce: b64(randomBytes(24)),
      participants: [alice, gbob, gcarol].map((u) => ({ userId: u.userId, wrappedKey: b64(randomBytes(80)) })),
    });
    check("a group of three accounts exists", created.status === 201, created);
    const group = created.body.id;
    const pb = new Desktop("pb", gbob, "file");
    const pc = new Desktop("pc", gcarol, "file");
    desktops.push(pb, pc);
    await pb.init();
    await pc.init();
    await pb.must({ cmd: "setup" });
    await pc.must({ cmd: "setup" });
    // The live channel: the app's own reader sees the mailbox event (no sender, no chat id).
    const waiting = pc.must({ cmd: "liveWait" });
    await sleep(500);
    const sentG = await d1.must({ cmd: "chatSend", group: true, conversationId: group, text: "hello group" });
    check("one group message fans out to the other four devices (alice's browser and second desktop, bob, carol)", sentG.delivered === 4, sentG);
    const live = await waiting;
    check("the desktop's live reader receives the server's v3-mailbox event", live.seen === true, live);
    const atPb = await pb.must({ cmd: "chatReceive", conversationId: group });
    check("bob reads the group message, from alice, in the group", atPb.items.some((m) => m.text === "hello group" && m.from === alice.userId && m.conversationId === group), atPb);
    const atPc = await pc.must({ cmd: "chatReceive", conversationId: group });
    check("carol reads it", atPc.items.some((m) => m.text === "hello group"), atPc);
    const atD2g = await d2.must({ cmd: "chatReceive", conversationId: group });
    check("alice's second desktop files its copy as her own message", atD2g.items.some((m) => m.text === "hello group" && m.direction === "out"), atD2g);
    await pb.must({ cmd: "chatSend", group: true, conversationId: group, text: "hi from bob" });
    const backAtD1 = await d1.must({ cmd: "chatReceive", conversationId: group });
    check("bob's reply reaches alice's first desktop", backAtD1.items.some((m) => m.text === "hi from bob" && m.from === gbob.userId), backAtD1);
    check("and carol", (await pc.must({ cmd: "chatReceive", conversationId: group })).items.some((m) => m.text === "hi from bob"));
    const nothing = await pc.must({ cmd: "chatReceive", conversationId: group });
    check("nothing is replayed", nothing.items.length === 0, nothing);
  }

  console.log("\n5. Removing a device, and the recovery key");
  await d1.must({ cmd: "revoke", deviceId: begun.deviceId });
  const afterRevoke = await w1.devicesOf(alice.userId, null);
  check("the browser sees the second desktop removed", afterRevoke.length === 2 && !afterRevoke.some((d) => d.deviceId === begun.deviceId), afterRevoke.length);
  const gone = await d2.must({ cmd: "status" });
  check("the removed desktop finds out it is no longer listed", gone.listedHere === false, gone);
  const recovery = (await d1.must({ cmd: "recoveryCreate" })).key;
  secrets.push(recovery.replaceAll("-", ""), recovery);
  check("the recovery key is 64 hex digits in groups of four", /^([0-9A-F]{4}-){15}[0-9A-F]{4}$/.test(recovery), recovery.length);
  const d3 = new Desktop("d3", alice, "file");
  desktops.push(d3);
  await d3.init();
  const wrong = await d3.call({ cmd: "restore", key: "00".repeat(32) });
  check("a wrong recovery key restores nothing", wrong.ok === false, wrong);
  const restored = await d3.must({ cmd: "restore", key: recovery });
  const afterRestore = await w1.devicesOf(alice.userId, null);
  check("a new desktop restored from the recovery key is on the list", afterRestore.some((d) => d.deviceId === restored.deviceId), afterRestore.length);
  check("it can read what the account's other devices send", (await b1.send(chat, alice.userId, "to the restored one")) >= 1);
  check("and does", (await d3.must({ cmd: "receive" })).items.some((m) => m.text === "to the restored one"));

  console.log("\n6. Nothing secret reached the server");
  await sleep(500);
  const log = readFileSync(SERVER_LOG, "utf8");
  const leaked = secrets.filter((s) => s && log.includes(s));
  check("no legacy key or recovery key appears in the server's log", leaked.length === 0, leaked.length);
  const dump = sh(pgTool("pg_dump"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "v3desktop", "--data-only"], { maxBuffer: 1 << 28 });
  const dumpLeak = secrets.filter((s) => s && dump.stdout.includes(s));
  check("nor in a dump of the database", dump.status === 0 && dumpLeak.length === 0, dumpLeak.length);

  console.log("\n7. Rollback: the server flag off");
  stopServer();
  await startServer(false);
  const off = new Desktop("d-off", alice);
  desktops.push(off);
  const login = await off.init();
  check("protocol 1 sign-in still works", Boolean(login.userId));
  const cfg = await off.call({ cmd: "config" });
  check("the server says protocol 3 is off", cfg.ok && cfg.enabled === false, cfg);
  const refused = await off.call({ cmd: "enrollments" });
  check("every device-key route answers 503 e2e_v3_disabled", refused.ok === false && refused.error === "e2e_v3_disabled", refused);
} catch (err) {
  console.log(`\nABORTED: ${err.stack ?? err}`);
  results.push({ name: "run completed", ok: false });
} finally {
  for (const d of desktops) d.stop();
  stopServer();
  stopPostgres();
  await sleep(500);
  try {
    rmSync(WORK, { recursive: true, force: true });
  } catch {}
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} checks passed${failed.length ? `; failed: ${failed.map((f) => f.name).join("; ")}` : ""}`);
process.exit(failed.length ? 1 : 0);
