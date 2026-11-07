// Cross-platform interoperability run for the shared engine.
//
// Each device is driven through its platform's own binding (drivers.mjs).
// This script plays the app around the engine: it keeps each device's encrypted
// state on disk (written atomically, before anything a call produced is sent)
// and delivers through the relay, which only sees public keys and ciphertext.
//
//   node crypto-core/interop/run.mjs [platform,platform,...]
import { randomBytes, randomUUID } from "node:crypto";
import { mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { HERE, RELAY, driverFor, flip } from "./drivers.mjs";

const RUN_DIR = join(HERE, "build", "run", new Date().toISOString().replace(/[:.]/g, "-"));
mkdirSync(RUN_DIR, { recursive: true });

// --- The relay ---

async function relay(method, path, body) {
  const res = await fetch(`${RELAY}${path}`, {
    method,
    headers: body ? { "content-type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  return res.status === 204 ? null : res.json();
}

// --- A device, as an app would hold it ---

class Device {
  constructor(platform, userId, deviceId) {
    this.platform = platform;
    this.driver = driverFor(platform);
    this.userId = userId;
    this.deviceId = deviceId;
    // Stands in for secure storage (Keystore / DPAPI / a WebCrypto key): only this device knows it.
    this.pickleKey = randomBytes(32).toString("base64");
    this.stateFile = join(RUN_DIR, `${userId}-${deviceId}.state`);
  }

  async engine(request) {
    const text = await this.driver.call(JSON.stringify(request));
    try {
      return JSON.parse(text);
    } catch {
      throw new Error(`${this.platform} driver answered: ${text.slice(0, 200)}`);
    }
  }

  /** Runs an op on this device's stored state; on success the new state is on disk before this returns. */
  async op(op, fields = {}) {
    const state = readFileSync(this.stateFile, "utf8");
    const response = await this.engine({ op, state, pickleKey: this.pickleKey, ...fields });
    if (response.ok) {
      writeFileSync(`${this.stateFile}.tmp`, response.state);
      renameSync(`${this.stateFile}.tmp`, this.stateFile);
    }
    return response;
  }

  /** The first device of a new account: its keys, the account key, device list version 1. */
  async create() {
    const created = await this.engine({ op: "createAccount", userId: this.userId, deviceId: this.deviceId, pickleKey: this.pickleKey });
    if (!created.ok) throw new Error(`${this.platform}: createAccount failed: ${created.error}`);
    writeFileSync(this.stateFile, created.state);
    this.identity = created.identity;
    await relay("POST", "/lists", { deviceList: created.deviceList });
    await this.publishPrekeys();
  }

  /** A device not yet linked to its account. */
  async createUnlinked() {
    const created = await this.engine({ op: "createDevice", userId: this.userId, deviceId: this.deviceId, pickleKey: this.pickleKey });
    if (!created.ok) throw new Error(`${this.platform}: createDevice failed: ${created.error}`);
    writeFileSync(this.stateFile, created.state);
    this.identity = created.identity;
  }

  async publishPrekeys() {
    const prekeys = await this.op("prekeys", { count: 5 });
    if (!prekeys.ok) throw new Error(`${this.platform}: prekeys failed: ${prekeys.error}`);
    await relay("POST", "/devices", { identity: this.identity, upload: prekeys.upload });
  }

  /** Fetches another account's signed device list from the relay and checks it. */
  async verifyListOf(userId) {
    const deviceList = await relay("GET", `/lists/${encodeURIComponent(userId)}`);
    return this.op("verifyDeviceList", { deviceList });
  }

  /** Olm-encrypts to `to`, claiming a one-time key from the relay the first time. */
  async sendTo(to, text, kind = "olm") {
    let response = await this.op("encrypt", { recipient: to.identity, plaintext: text });
    if (!response.ok && response.error === "no_session") {
      const claimed = await relay("POST", "/claim", { userId: to.userId, deviceId: to.deviceId });
      response = await this.op("encrypt", { recipient: claimed.identity, oneTimeKey: claimed.oneTimeKey, plaintext: text });
    }
    if (!response.ok) throw new Error(`${this.platform}: encrypt failed: ${response.error}`);
    await relay("POST", "/messages", { to: { userId: to.userId, deviceId: to.deviceId }, from: this.identity, kind, payload: response.message });
    return response.message;
  }

  async inbox() {
    return relay("GET", `/messages/${encodeURIComponent(this.userId)}/${encodeURIComponent(this.deviceId)}`);
  }

  async ack(ids) {
    await relay("POST", "/messages/ack", { userId: this.userId, deviceId: this.deviceId, ids });
  }
}

// --- One pairing ---

function check(results, name, ok, detail = "") {
  results.push({ name, ok: Boolean(ok), detail });
  if (!ok) console.log(`    FAIL ${name} ${detail}`);
}

async function scenario(platformA, platformB) {
  const run = randomUUID().slice(0, 8);
  const a = new Device(platformA, `alice-${run}`, `${platformA}-1`);
  const b = new Device(platformB, `bob-${run}`, `${platformB}-1`);
  const extra = [];
  const results = [];
  try {
    await a.create();
    await b.create();
    check(results, "identities differ", a.identity.curve25519 !== b.identity.curve25519);
    // Each checks the other's signed device list (and pins the account key) before anything else.
    for (const [viewer, owner] of [[a, b], [b, a]]) {
      const seen = await viewer.verifyListOf(owner.userId);
      check(results, `${viewer.platform} verifies ${owner.platform}'s device list`, seen.ok && seen.firstSeen === true, seen.error);
    }

    // 1. Session establishment while B is offline: A only has B's signed one-time key.
    for (const text of ["m1: hello while you're offline", "m2: ünïcödé ✓ and emoji 🔐", "m3: third"]) await a.sendTo(b, text);

    // 2. B comes online and reads them out of order, then a replay is refused.
    const inbox = (await b.inbox()).filter((m) => m.kind === "olm");
    check(results, "three messages delivered", inbox.length === 3, `got ${inbox.length}`);
    const order = [2, 0, 1];
    for (const i of order) {
      const read = await b.op("decrypt", { sender: inbox[i].from, message: inbox[i].payload });
      const expected = ["m1: hello while you're offline", "m2: ünïcödé ✓ and emoji 🔐", "m3: third"][i];
      check(results, `B reads m${i + 1} (out of order)`, read.ok && read.plaintext === expected, read.error ?? read.plaintext);
    }
    const replay = await b.op("decrypt", { sender: inbox[0].from, message: inbox[0].payload });
    check(results, "replayed message refused", replay.ok === false && replay.error === "duplicate", replay.error);
    await b.ack(inbox.map((m) => m.id));

    // 3. The other direction (ratchet turns).
    await b.sendTo(a, "r1: got them");
    await b.sendTo(a, "r2: all three");
    const replies = (await a.inbox()).filter((m) => m.kind === "olm");
    for (const [i, m] of [...replies.entries()].reverse()) {
      const read = await a.op("decrypt", { sender: m.from, message: m.payload });
      check(results, `A reads r${i + 1}`, read.ok && read.plaintext === ["r1: got them", "r2: all three"][i], read.error ?? read.plaintext);
    }
    await a.ack(replies.map((m) => m.id));

    // 4. Restarts: each side's process goes; only the stored state survives.
    a.driver.restart();
    const m4 = await a.sendTo(b, "m4: after A restarted");
    check(results, "after the reply A sends normal messages", m4.type === 1, `type ${m4.type}`);
    b.driver.restart();
    for (const m of (await b.inbox()).filter((x) => x.kind === "olm")) {
      const read = await b.op("decrypt", { sender: m.from, message: m.payload });
      check(results, "B reads m4 after both restarts", read.ok && read.plaintext === "m4: after A restarted", read.error ?? read.plaintext);
      await b.ack([m.id]);
    }
    await b.sendTo(a, "r3: after B restarted");
    for (const m of (await a.inbox()).filter((x) => x.kind === "olm")) {
      const read = await a.op("decrypt", { sender: m.from, message: m.payload });
      check(results, "A reads r3", read.ok && read.plaintext === "r3: after B restarted", read.error ?? read.plaintext);
      await a.ack([m.id]);
    }

    // 5. A group of the two (Megolm): each sends with its own session, whose key goes to the other over Olm.
    const groupId = `group-${run}`;
    for (const d of [a, b]) {
      const set = await d.op("groupSetMembers", { groupId, members: [a.identity, b.identity] });
      check(results, `${d.platform} sets the members`, set.ok, set.error);
    }
    for (const [sender, reader] of [[a, b], [b, a]]) {
      const session = await sender.op("groupSession", { groupId });
      for (const member of session.unshared) {
        const shared = await sender.op("shareRoomKey", { groupId, recipient: member });
        check(results, `${sender.platform} shares its group key`, shared.ok, shared.error);
        await relay("POST", "/messages", { to: member, from: sender.identity, kind: "olm", payload: shared.message });
      }
      const text = `group hello from ${sender.platform}`;
      const sent = await sender.op("groupEncrypt", { groupId, plaintext: text });
      check(results, `${sender.platform} encrypts for the group`, sent.ok, sent.error);
      // Only the group message goes out (never the response's state).
      const groupMessage = { groupId: sent.groupId, sessionId: sent.sessionId, ciphertext: sent.ciphertext };
      await relay("POST", "/messages", { to: reader.identity, from: sender.identity, kind: "group", payload: groupMessage });
      for (const m of await reader.inbox()) {
        if (m.kind === "olm") {
          const key = await reader.op("decrypt", { sender: m.from, message: m.payload });
          check(results, `${reader.platform} accepts the group key`, key.ok && key.kind === "roomKey", key.error);
        } else {
          const tampered = { ...m.payload, ciphertext: flip(m.payload.ciphertext) };
          const bad = await reader.op("groupDecrypt", tampered);
          check(results, "a tampered group message is refused", bad.ok === false, bad.error);
          const read = await reader.op("groupDecrypt", m.payload);
          check(results, `${reader.platform} reads the group message`, read.ok && read.plaintext === text, read.error ?? read.plaintext);
          check(results, "with an authenticated sender", read.ok && read.sender.userId === sender.userId && read.sender.deviceId === sender.deviceId);
        }
        await reader.ack([m.id]);
      }
    }
    // 6. Safety numbers: the same on both sides; scanning the other's QR code verifies.
    const fpA = await a.op("fingerprint", { userId: b.userId });
    const fpB = await b.op("fingerprint", { userId: a.userId });
    check(results, "safety numbers match", fpA.ok && fpB.ok && fpA.safetyNumber.join(" ") === fpB.safetyNumber.join(" "), fpA.error ?? fpB.error);
    const scanned = await a.op("verifyFingerprintQr", { userId: b.userId, qr: fpB.qr });
    check(results, `${a.platform} verifies ${b.platform}'s QR code`, scanned.ok && scanned.verified === true, scanned.error);

    // 7. A trusted device on one platform links a new device of the same account on the other.
    const a2 = new Device(platformB, a.userId, `${platformB}-2`);
    extra.push(a2);
    await a2.createUnlinked();
    const offer = await a2.op("linkOffer");
    const approved = await a.op("approveDevice", { qr: offer.qrText, verifiedBy: "qr" });
    check(results, `${a.platform} approves a ${platformB} device by QR`, approved.ok && approved.deviceList?.version === 2, approved.error);
    await relay("POST", "/lists", { deviceList: approved.deviceList });
    await relay("POST", "/messages", { to: a2.identity, from: a.identity, kind: "olm", payload: approved.message });
    for (const m of await a2.inbox()) {
      const accepted = await a2.op("decrypt", { sender: m.from, message: m.payload });
      check(results, `the ${platformB} device accepts the link`, accepted.ok && accepted.kind === "link", accepted.error);
      await a2.ack([m.id]);
    }
    await a2.publishPrekeys();
    const added = await b.verifyListOf(a.userId);
    check(results, `${b.platform} is told a device was added`, added.ok && added.added?.length === 1 && added.added[0].deviceId === a2.deviceId, added.error);
    await b.sendTo(a2, "hello, new device");
    for (const m of await a2.inbox()) {
      const read = await a2.op("decrypt", { sender: m.from, message: m.payload });
      check(results, "the linked device reads a message", read.ok && read.plaintext === "hello, new device", read.error ?? read.plaintext);
      await a2.ack([m.id]);
    }

    // 8. Revocation: it drops out of the next list, everyone is told, nobody can reach it.
    const revoked = await a.op("revokeDevice", { deviceId: a2.deviceId });
    check(results, `${a.platform} revokes the device`, revoked.ok && revoked.deviceList?.version === 3, revoked.error);
    await relay("POST", "/lists", { deviceList: revoked.deviceList });
    const removed = await b.verifyListOf(a.userId);
    check(results, `${b.platform} is told the device was removed`, removed.ok && removed.removed?.length === 1, removed.error);
    const refused = await b.op("encrypt", { recipient: a2.identity, plaintext: "still there?" });
    check(results, "the revoked device can't be reached", refused.ok === false && refused.error === "unlisted_device", refused.error);
    const rollback = await b.op("verifyDeviceList", { deviceList: approved.deviceList });
    check(results, "an older device list is refused", rollback.ok === false && rollback.error === "rollback", rollback.error);

    // 9. A story: fresh key, sent only to the audience over Olm, opened with its publisher known.
    const now = Date.now();
    const story = await a.op("storyCreate", { storyId: `story-${run}`, content: `story from ${a.platform} 🌅`, expiresAt: now + 3_600_000, audience: [b.identity] });
    check(results, `${a.platform} publishes a story`, story.ok, story.error);
    const keyShare = await a.op("storyShareKey", { storyId: `story-${run}`, recipient: b.identity });
    await relay("POST", "/messages", { to: b.identity, from: a.identity, kind: "olm", payload: keyShare.message });
    for (const m of await b.inbox()) {
      const got = await b.op("decrypt", { sender: m.from, message: m.payload });
      check(results, `${b.platform} receives the story key`, got.ok && got.kind === "storyKey", got.error);
      await b.ack([m.id]);
    }
    const opened = await b.op("storyOpen", { storyId: `story-${run}`, ciphertext: story.ciphertext, now });
    check(results, `${b.platform} opens the story`, opened.ok && opened.content === `story from ${a.platform} 🌅` && opened.publisher.userId === a.userId, opened.error);
    const tamperedStory = await b.op("storyOpen", { storyId: `story-${run}`, ciphertext: flip(story.ciphertext), now });
    check(results, "a tampered story is refused", tamperedStory.ok === false, tamperedStory.error);
    const expired = await b.op("storyOpen", { storyId: `story-${run}`, ciphertext: story.ciphertext, now: now + 7_200_000 });
    check(results, "an expired story's key is deleted", expired.ok && expired.expired === true, expired.error);
  } catch (err) {
    check(results, "no unexpected error", false, String(err?.message ?? err));
  } finally {
    a.driver.stop();
    b.driver.stop();
    for (const d of extra) d.driver.stop();
  }
  return results;
}

// --- The matrix ---

const platforms = (process.argv[2] ?? "web,node,windows,linux,android").split(",");
const pairs = [];
for (let i = 0; i < platforms.length; i++) {
  for (let j = i; j < platforms.length; j++) {
    pairs.push([platforms[i], platforms[j]]);
    if (i !== j) pairs.push([platforms[j], platforms[i]]);
  }
}

const summary = [];
for (const [first, second] of pairs) {
  process.stdout.write(`${first} → ${second} ... `);
  const started = Date.now();
  const results = await scenario(first, second);
  const passed = results.filter((r) => r.ok).length;
  const ok = passed === results.length;
  console.log(`${ok ? "PASS" : "FAIL"} (${passed}/${results.length} checks, ${((Date.now() - started) / 1000).toFixed(1)} s)`);
  summary.push({ initiator: first, responder: second, ok, passed, checks: results.length, failures: results.filter((r) => !r.ok) });
}
writeFileSync(join(HERE, "build", "interop-results.json"), JSON.stringify({ at: new Date().toISOString(), summary }, null, 2));
const failed = summary.filter((s) => !s.ok);
console.log(`\n${summary.length - failed.length}/${summary.length} pairings passed`);
process.exit(failed.length ? 1 : 0);
