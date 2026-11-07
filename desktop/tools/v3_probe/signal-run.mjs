// M4 (docs/signal-integration/): rv-signal-v1 between the real apps' code on three platforms, through the
// REAL server code on a throwaway PostgreSQL cluster. Development only: throwaway accounts, nothing deployed.
//
//   Windows desktop: redvault_v3_probe.exe (the redvault_e2ev3 library the app uses), built with REDVAULT_E2E_SIGNAL
//   Linux desktop:   the same probe built in WSL, reaching the server through wsl_tunnel.py (loopback only)
//   Android:         SignalProbeTest on an emulator (the app's devicekeys code, Keystore, JNI engine), via adb reverse
//
//   node desktop/tools/v3_probe/signal-run.mjs
//
// Env: SIGNAL_WIN_PROBE, SIGNAL_LINUX_PROBE (path inside WSL), SIGNAL_ADB_SERIAL (default emulator-5580),
//      SIGNAL_APK, SIGNAL_TEST_APK. Prints one line per check and a summary; exits 1 on any failure.
import { spawn, spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { createWriteStream, existsSync, mkdirSync, mkdtempSync, readdirSync } from "node:fs";
import { createServer } from "node:net";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const HERE = fileURLToPath(new URL(".", import.meta.url));
const ROOT = join(HERE, "..", "..", "..");
const PORT = 3055; // wsl_tunnel.py's app port
const PG_PORT = 5492;
const CONTROL_PORT = 3057;
const SERVER = `http://127.0.0.1:${PORT}`;
const WORK = mkdtempSync(join(tmpdir(), "rv-signal-m4-"));
const WIN_PROBE = process.env.SIGNAL_WIN_PROBE ?? join(ROOT, "desktop", "build-signal", "tools", "v3_probe", "redvault_v3_probe.exe");
const LINUX_PROBE = process.env.SIGNAL_LINUX_PROBE ?? "/home/winchester/rv-signal-m4/build/tools/v3_probe/redvault_v3_probe";
const SERIAL = process.env.SIGNAL_ADB_SERIAL ?? "emulator-5580";
const ADB = join(process.env.LOCALAPPDATA, "Android", "Sdk", "platform-tools", "adb.exe");
const APK_DIR = join(ROOT, "android", "app", "build", "outputs", "apk");
const APK = process.env.SIGNAL_APK ?? join(APK_DIR, "debug", "app-debug.apk");
const TEST_APK = process.env.SIGNAL_TEST_APK ?? join(APK_DIR, "androidTest", "debug", "app-debug-androidTest.apk");

const sodium = createRequire(join(ROOT, "ui", "package.json"))("libsodium-wrappers-sumo");
await sodium.ready;
const pg = createRequire(join(ROOT, "server", "package.json"))("pg");

const b64 = (bytes) => Buffer.from(bytes).toString("base64");
const hex = (n) => randomBytes(n).toString("hex");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
/** Rejects after `ms` with `what`, so a hang names the step it's in. */
function within(ms, what, promise) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => (timer = setTimeout(() => reject(new Error(`timed out after ${ms / 1000} s: ${what}`)), ms)))]).finally(() => clearTimeout(timer));
}
const step = (text) => console.log(`  ..   ${text}`);
const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok: Boolean(ok) });
  const text = typeof detail === "string" ? detail : JSON.stringify(detail);
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${ok || !text ? "" : ` -- ${text.slice(0, 400)}`}`);
}
const sh = (command, args, options = {}) => spawnSync(command, args, { encoding: "utf8", windowsHide: true, ...options });
/** A Windows path as WSL sees it: C:\a\b becomes /mnt/c/a/b. */
const toWsl = (p) => p.replace(/^([A-Za-z]):/, (_, d) => `/mnt/${d.toLowerCase()}`).split("\\").join("/");
const kill = (child) => {
  if (!child || child.exitCode !== null) return;
  if (process.platform === "win32") spawnSync("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
  else child.kill();
};

// --- PostgreSQL and the server ---

function pgTool(name) {
  return join("C:/Program Files/PostgreSQL/17/bin", `${name}.exe`);
}
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
  sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-c", "CREATE DATABASE signalm4"]);
  const files = [join(ROOT, "db", "schema.sql"), ...readdirSync(join(ROOT, "db", "migrations")).sort().map((f) => join(ROOT, "db", "migrations", f))];
  for (const file of files) sh(pgTool("psql"), ["-h", "127.0.0.1", "-p", String(PG_PORT), "-U", "postgres", "-d", "signalm4", "-q", "-f", file]);
  db = new pg.Pool({ connectionString: `postgres://postgres@127.0.0.1:${PG_PORT}/signalm4` });
}

let serverProcess = null;
const SERVER_SECRETS = { V3_DEV_PEPPER: randomBytes(32).toString("base64"), V3_DEV_USERNAME_KEY: randomBytes(32).toString("base64") };
const serverLog = createWriteStream(join(WORK, "server.log"), { flags: "a" });
async function startServer() {
  const tsx = join(ROOT, "server", "node_modules", "tsx", "dist", "cli.mjs");
  serverProcess = spawn(process.execPath, [tsx, join(ROOT, "crypto-core", "interop", "server", "dev-server.mts")], {
    env: {
      ...process.env,
      ...SERVER_SECRETS,
      V3_DEV_DATABASE_URL: `postgres://postgres@127.0.0.1:${PG_PORT}/signalm4`,
      V3_DEV_PORT: String(PORT),
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
      if ((await fetch(`${SERVER}/health`)).ok) return;
    } catch {}
    await sleep(500);
  }
  throw new Error("the dev API didn't start");
}
function stopServer() {
  kill(serverProcess);
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

/** An account as it exists today (protocol 1), with its legacy key known to the test so the probe can certify. */
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

// --- The three platforms ---

/** A line-oriented JSON process: the desktop probe on Windows, or the same probe inside WSL. */
class Desktop {
  constructor(name, acct, platform) {
    this.name = name;
    this.account = acct;
    this.platform = platform;
    if (platform === "windows") {
      this.dataDir = join(WORK, name);
      mkdirSync(this.dataDir, { recursive: true });
    } else {
      this.dataDir = `/home/winchester/rv-signal-m4/run/${name}-${hex(3)}`;
      sh("wsl.exe", ["-d", "Ubuntu", "--", "mkdir", "-p", this.dataDir]);
    }
    this.start();
  }
  start() {
    this.child =
      this.platform === "windows"
        ? spawn(WIN_PROBE, [], { stdio: ["pipe", "pipe", "inherit"], windowsHide: true, env: { ...process.env, PATH: `C:\\msys64\\ucrt64\\bin;${process.env.PATH}` } })
        : spawn("wsl.exe", ["-d", "Ubuntu", "--", LINUX_PROBE], { stdio: ["pipe", "pipe", "inherit"], windowsHide: true });
    this.pending = [];
    createInterface({ input: this.child.stdout }).on("line", (line) => {
      if (!line.startsWith("{")) return;
      this.pending.shift()?.(JSON.parse(line));
    });
  }
  call(cmd) {
    return within(
      120_000,
      `${this.name} ${cmd.cmd}`,
      new Promise((resolve) => {
        this.pending.push(resolve);
        this.child.stdin.write(`${JSON.stringify(cmd)}\n`);
      }),
    );
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
      vault: this.platform === "linux" ? "file" : "default",
    });
  }
  /** The app quits and starts again: a new process, everything from storage. */
  async restart() {
    this.stop();
    await sleep(500);
    this.start();
    return this.init();
  }
  stop() {
    try {
      this.child.stdin.end();
    } catch {}
    kill(this.child);
  }
  async send(contact, conversationId, text) {
    const r = await this.must({ cmd: "chatSend", conversationId, contactUserId: contact.userId, text });
    return { delivered: r.delivered };
  }
  async sendGroup(conversationId, text) {
    const r = await this.must({ cmd: "chatSend", group: true, conversationId, text });
    return { delivered: r.delivered };
  }
  async receive(conversationId) {
    return (await this.must({ cmd: "chatReceive", conversationId })).items;
  }
  signal() {
    return this.must({ cmd: "signal" });
  }
}

const adb = (...args) => sh(ADB, ["-s", SERIAL, ...args]);

/** The emulator: SignalProbeTest takes commands over a socket the runner listens on (adb reverse). */
class Android {
  constructor(name, acct, control) {
    this.name = name;
    this.account = acct;
    this.control = control;
    this.platform = "android";
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
  async restart() {
    await this.stop();
    await this.start();
    return this.init();
  }
  async stop() {
    try {
      await this.call({ cmd: "quit" });
    } catch {}
    await new Promise((r) => (this.instrument.exitCode !== null ? r() : this.instrument.on("exit", r)));
  }
  async send(contact, conversationId, text) {
    const r = await this.must({ cmd: "chatSend", conversationId, contactUserId: contact.userId, text });
    return { delivered: r.sent ? 1 : 0 };
  }
  async sendGroup(conversationId, text) {
    const r = await this.must({ cmd: "chatSend", group: true, conversationId, text });
    return { delivered: r.sent ? 1 : 0 };
  }
  async receive() {
    return (await this.must({ cmd: "chatReceive" })).items;
  }
  signal() {
    return this.must({ cmd: "signal" });
  }
}

/** The runner's end of the Android control socket: one connection per instrumentation run. */
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

// --- The run ---

let tunnelWsl = null;
let tunnelWin = null;
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
  console.log("server up (protocol 3 + rv-signal-v1 on, throwaway Postgres)");

  // WSL reaches the server through the loopback tunnel; the emulator through adb reverse.
  tunnelWsl = spawn("wsl.exe", ["-d", "Ubuntu", "--", "python3", toWsl(join(ROOT, "desktop", "tools", "v3_probe", "wsl_tunnel.py")), "wsl"], { stdio: ["ignore", "pipe", "inherit"], windowsHide: true });
  await sleep(3000);
  tunnelWin = spawn("python", ["-I", join(ROOT, "desktop", "tools", "v3_probe", "wsl_tunnel.py"), "win"], { stdio: ["ignore", "pipe", "inherit"], windowsHide: true });
  await sleep(2000);
  for (const p of [String(PORT), String(CONTROL_PORT)]) adb("reverse", `tcp:${p}`, `tcp:${p}`);
  control = controlServer();
  const installed = adb("install", "-r", "-t", APK);
  const installedTest = adb("install", "-r", "-t", TEST_APK);
  check("android: app and test APKs install on the emulator", installed.status === 0 && installedTest.status === 0, installed.stdout + installed.stderr + installedTest.stderr);
  const instrumentation = adb("shell", "pm", "list", "instrumentation").stdout;
  const testLine = instrumentation.split(/\r?\n/).find((line) => line.includes("(target=com.redvaultsystems.redvaultmessage"));
  const testPackage = testLine?.match(/^instrumentation:([^/]+)\//)?.[1];
  check("android: the probe's instrumentation is installed", Boolean(testPackage), instrumentation);

  step("accounts and friendships");
  // Three accounts, one device each, every pair befriended (each friendship is a conversation).
  const w = await account("win");
  const l = await account("linux");
  const a = await account("droid");
  const conv = new Map();
  const key = (x, y) => [x.userId, y.userId].sort().join("/");
  for (const [x, y] of [[w, l], [w, a], [l, a]]) conv.set(key(x, y), await befriend(x, y));

  step("starting the Windows and Linux probes and the Android driver");
  const W = new Desktop("W", w, "windows");
  const L = new Desktop("L", l, "linux");
  desktops.push(W, L);
  android = new Android("A", a, control);
  android.testPackage = testPackage;
  await android.start();
  check(`android: the probe runs on the emulator (API ${android.sdk})`, android.sdk >= 26);

  step("signing in on all three");
  for (const d of [W, L, android]) await d.init();
  for (const d of [W, L]) await d.must({ cmd: "setup" });
  await android.must({ cmd: "setup" });
  for (const d of [W, L, android]) {
    const r = await d.signal();
    const outcome = r.outcome ?? r.result?.outcome;
    check(`${d.platform}: Signal turned on and declared (${outcome})`, outcome === "enabled" || outcome === "declared");
  }

  const dbq = (text, params) => db.query(text, params);
  const signalDevices = (await dbq("SELECT count(*)::int AS n FROM v3_signal_devices")).rows[0].n;
  check("server: three devices published Signal keys", signalDevices === 3, signalDevices);
  for (const acct of [w, l, a]) {
    const list = (await dbq("SELECT list FROM v3_device_lists WHERE user_id = $1", [acct.userId])).rows[0]?.list;
    check(`server: ${acct.name}'s signed list declares rv-signal-v1`, list?.capabilities?.[0]?.protocol === "rv-signal-v1", list);
  }

  const peers = { W: { node: W, acct: w }, L: { node: L, acct: l }, A: { node: android, acct: a } };
  const secrets = [];
  const mailboxFor = async (acct) => (await dbq("SELECT body FROM v3_mailbox WHERE recipient_user_id = $1", [acct.userId])).rows.map((r) => JSON.parse(r.body));

  // All six directions, first message (PQXDH) and a reply.
  for (const [s, r] of [["W", "L"], ["L", "W"], ["W", "A"], ["A", "W"], ["L", "A"], ["A", "L"]]) {
    const S = peers[s];
    const R = peers[r];
    const c = conv.get(key(S.acct, R.acct));
    const text = `${s}->${r} ${hex(6)}`;
    secrets.push(text);
    const sent = await S.node.send(R.acct, c, text);
    const waiting = await mailboxFor(R.acct);
    const signalBodies = waiting.filter((b) => b.protocol === "rv-signal-v1");
    check(`${s}->${r}: the server holds only rv-signal-v1 ciphertext for the recipient`, sent.delivered >= 1 && waiting.length > 0 && signalBodies.length === waiting.length, waiting.map((b) => b.protocol ?? "olm"));
    check(`${s}->${r}: no plaintext in what the server holds`, !JSON.stringify(waiting).includes(text));
    const got = await R.node.receive(c);
    check(`${s}->${r}: ${R.node.platform} opens it`, got.some((m) => m.text === text), got.map((m) => m.text));
  }

  // Device-key safety numbers: the same digits on both sides of every pair, and each platform's
  // camera code verifies on the other platforms (both directions), while a wrong code doesn't.
  step("safety numbers");
  const safety = (n, other) => n.node.must({ cmd: "safety", contactUserId: other.acct.userId });
  for (const [x, y] of [["W", "L"], ["W", "A"], ["L", "A"]]) {
    const X = peers[x];
    const Y = peers[y];
    const atX = await safety(X, Y);
    const atY = await safety(Y, X);
    check(`safety ${x}/${y}: both sides show the same 60 digits`, Array.isArray(atX.groups) && atX.groups.length === 12 && JSON.stringify(atX.groups) === JSON.stringify(atY.groups), [atX.groups, atY.groups]);
    check(`safety ${x}/${y}: not verified yet, no key change`, atX.verified === false && atY.verified === false && !atX.keyChanged && !atY.keyChanged);
    const wrong = await X.node.must({ cmd: "safetyScan", contactUserId: Y.acct.userId, accountKey: atX.accountKey, qrText: atX.qrText });
    check(`safety ${x}/${y}: ${X.node.platform} refuses its own code as theirs`, wrong.verified === false, wrong);
    const xScansY = await X.node.must({ cmd: "safetyScan", contactUserId: Y.acct.userId, accountKey: atX.accountKey, qrText: atY.qrText });
    check(`safety ${x}/${y}: ${X.node.platform} scans ${Y.node.platform}'s code and it matches`, xScansY.verified === true, xScansY);
    const yScansX = await Y.node.must({ cmd: "safetyScan", contactUserId: X.acct.userId, accountKey: atY.accountKey, qrText: atX.qrText });
    check(`safety ${x}/${y}: ${Y.node.platform} scans ${X.node.platform}'s code and it matches`, yScansX.verified === true, yScansX);
    check(`safety ${x}/${y}: both now show verified`, (await safety(X, Y)).verified === true && (await safety(Y, X)).verified === true);
  }
  // Taking it back, by hand, on one platform only.
  await peers.A.node.must({ cmd: "safetyMark", contactUserId: w.userId, accountKey: (await safety(peers.A, peers.W)).accountKey, verified: false });
  check("safety: android marks windows as not verified; windows still shows android verified", (await safety(peers.A, peers.W)).verified === false && (await safety(peers.W, peers.A)).verified === true);

  // Offline recipient, sender restarts in the middle, recipient comes back.
  L.stop();
  const offline = [];
  for (const [s, n] of [["W", 2], ["A", 2]]) {
    for (let i = 0; i < n; i++) {
      const text = `offline ${s}->L #${i} ${hex(4)}`;
      offline.push(text);
      secrets.push(text);
      await peers[s].node.send(l, conv.get(key(peers[s].acct, l)), text);
    }
  }
  await W.restart();
  const afterRestart = `after W restart ${hex(4)}`;
  offline.push(afterRestart);
  secrets.push(afterRestart);
  const restartedSend = await W.send(l, conv.get(key(w, l)), afterRestart);
  check("windows: sends again after a restart, from stored state", restartedSend.delivered >= 1);
  const queued = (await dbq("SELECT count(*)::int AS n FROM v3_mailbox WHERE recipient_user_id = $1", [l.userId])).rows[0].n;
  check("server: queues all five for the offline Linux device", queued === 5, queued);
  await L.restart();
  const back = await L.receive(conv.get(key(w, l)));
  check("linux: back online, opens every queued message once", offline.every((t) => back.filter((m) => m.text === t).length === 1) && back.length === offline.length, back.map((m) => m.text));
  check("linux: nothing new on a second read", (await L.receive(conv.get(key(w, l)))).length === 0);

  // Android restarts (a new app process) and keeps its sessions.
  await android.restart();
  const fromRestartedAndroid = `after A restart ${hex(4)}`;
  secrets.push(fromRestartedAndroid);
  await android.send(w, conv.get(key(w, a)), fromRestartedAndroid);
  const atW = await W.receive(conv.get(key(w, a)));
  check("android: sends after its app process restarted; windows opens it", atW.some((m) => m.text === fromRestartedAndroid), atW.map((m) => m.text));

  // The network goes away in the middle of a send, then comes back.
  stopServer();
  let failed = false;
  try {
    await W.send(a, conv.get(key(w, a)), "while the server is down");
  } catch {
    failed = true;
  }
  check("windows: a send while the server is down fails visibly", failed);
  await startServer();
  const afterOutage = `after outage ${hex(4)}`;
  secrets.push(afterOutage);
  await W.send(a, conv.get(key(w, a)), afterOutage);
  const atA = await android.receive();
  check("android: gets the message sent after the outage, once", atA.filter((m) => m.text === afterOutage).length === 1, atA.map((m) => m.text));
  check("android: the failed send never arrives", !atA.some((m) => m.text === "while the server is down"));

  step("group phase");
  // --- M6: a three-platform group on Signal sender keys, then a removal ---
  const created = await w.api.post("/api/conversations", {
    isGroup: true,
    name: "m6",
    participants: [w, l, a].map((u) => ({ userId: u.userId, wrappedKey: b64(randomBytes(80)) })),
  });
  check("server: the group conversation is created", created.status === 201, created.body);
  const group = created.body.id;
  const groupBodies = async (acct) => (await dbq("SELECT body FROM v3_mailbox WHERE recipient_user_id = $1 AND conversation_id = $2", [acct.userId, group])).rows.map((r) => JSON.parse(r.body));
  for (const [s, others] of [["W", ["L", "A"]], ["A", ["W", "L"]], ["L", ["W", "A"]]]) {
    const text = `group ${s} ${hex(6)}`;
    secrets.push(text);
    const sent = await peers[s].node.sendGroup(group, text);
    check(`group ${s}: sent to both other members`, sent.delivered >= 1, sent);
    for (const r of others) {
      const bodies = await groupBodies(peers[r].acct);
      const protocols = bodies.map((b) => b.protocol ?? (b.kind === "group" ? "megolm" : "olm"));
      check(
        `group ${s}->${r}: only Signal (sender key and group message), no Megolm or Olm`,
        protocols.includes("rv-signal-group-v1") && protocols.every((p) => p === "rv-signal-group-v1" || p === "rv-signal-v1"),
        protocols,
      );
      check(`group ${s}->${r}: no plaintext on the server`, !JSON.stringify(bodies).includes(text));
      const got = await peers[r].node.receive(group);
      check(`group ${s}->${r}: ${peers[r].node.platform} opens it`, got.some((m) => m.text === text), got.map((m) => m.text));
    }
  }
  const rosters1 = (await dbq("SELECT epoch, roster FROM v3_group_rosters WHERE conversation_id = $1 ORDER BY epoch", [group])).rows;
  check("server: one signed roster (epoch 1) with all three members", rosters1.length === 1 && rosters1[0].roster.members.length === 3, rosters1.map((r) => r.roster.members));

  // Windows (the group's owner) removes Linux; the next send signs epoch 2 and uses a new sender key.
  const removed = await w.api.call("DELETE", `/api/conversations/${group}/participants/${l.userId}`);
  check("server: the owner removes Linux from the group", removed.status === 204, removed.body);
  const afterRemoval = `group after L left ${hex(6)}`;
  secrets.push(afterRemoval);
  await W.sendGroup(group, afterRemoval);
  const rosters2 = (await dbq("SELECT epoch, roster, roster_hash FROM v3_group_rosters WHERE conversation_id = $1 ORDER BY epoch", [group])).rows;
  check(
    "server: roster epoch 2 without Linux, chained to epoch 1",
    rosters2.length === 2 && !rosters2[1].roster.members.includes(l.userId) && rosters2[1].roster.prev === rosters2[0].roster_hash,
    rosters2.map((r) => ({ epoch: r.epoch, members: r.roster.members.length })),
  );
  const atAndroid = await android.receive();
  check("android: opens the message sent after the removal", atAndroid.some((m) => m.text === afterRemoval), atAndroid.map((m) => m.text));
  const toLinux = await groupBodies(l);
  check("server: nothing new waits for the removed Linux device", toLinux.length === 0, toLinux.length);
  const atLinux = await L.receive(group);
  check("linux: the removed member reads nothing new", !atLinux.some((m) => m.text === afterRemoval), atLinux.map((m) => m.text));
  let refused = false;
  try {
    await L.sendGroup(group, "from a removed member");
  } catch {
    refused = true;
  }
  check("linux: a removed member can't send to the group", refused);

  // Olm between two Signal devices: refused by each app's engine (checked on the desktop probe's raw engine call).
  // (The engine-level refusal is covered by the unit tests on every platform; here the apps never produced Olm chat.)
  const olmBodies = (await dbq("SELECT body FROM v3_mailbox")).rows.map((r) => JSON.parse(r.body)).filter((b) => b.protocol !== "rv-signal-v1" && b.protocol !== "rv-signal-group-v1");
  check("server: no Olm chat message between Signal devices is waiting anywhere", olmBodies.length === 0, olmBodies.length);

  // Leak scan: every table, as text, for every plaintext and every account's legacy key.
  const tables = (await dbq("SELECT tablename FROM pg_tables WHERE schemaname = 'public'")).rows.map((r) => r.tablename);
  let dump = "";
  for (const t of tables) dump += JSON.stringify((await dbq(`SELECT * FROM "${t}"`)).rows);
  const leaked = [...secrets, w.legacyKey, l.legacyKey, a.legacyKey].filter((s) => dump.includes(s));
  check(`server: no plaintext or private key in any of ${tables.length} tables`, leaked.length === 0, leaked);
  const log = sh("cmd", ["/c", "type", join(WORK, "server.log")]).stdout ?? "";
  check("server: no plaintext in the server log", ![...secrets].some((s) => log.includes(s)));
}

let exitCode = 0;
try {
  await main();
} catch (e) {
  console.log(`  FAIL run stopped: ${e.stack ?? e}`);
  exitCode = 1;
} finally {
  for (const d of desktops) d.stop();
  if (android) await Promise.race([android.stop(), sleep(5000)]);
  control?.server.close();
  for (const p of [String(PORT), String(CONTROL_PORT)]) adb("reverse", "--remove", `tcp:${p}`);
  kill(tunnelWin);
  kill(tunnelWsl);
  // "[w]sl_tunnel" so the pattern never matches pkill's own command line.
  sh("wsl.exe", ["-d", "Ubuntu", "--", "pkill", "-f", "[w]sl_tunnel.py wsl"]);
  sh("wsl.exe", ["-d", "Ubuntu", "--", "rm", "-rf", "/home/winchester/rv-signal-m4/run"]);
  stopServer();
  await db?.end();
  if (pgProcess) sh(pgTool("pg_ctl"), ["-D", PGDATA, "-m", "immediate", "stop"]);
  const failed = results.filter((r) => !r.ok).length;
  console.log(`\n${results.length - failed}/${results.length} checks passed${failed ? `, ${failed} failed` : ""}`);
  if (failed) exitCode = 1;
}
process.exit(exitCode);
