// A one-to-one call between two real web-app users in two headless browsers (fake microphones), through the real
// coturn the test server hands out (REST credentials), watched with the browsers' own WebRTC statistics.
//
//   node call_relay_test.mjs --base http://localhost:18081/ --minutes 10 --tag a1 [--block-renewal] --out results.jsonl
//
// Checks: the connection is relay-only (iceTransportPolicy "relay", every local candidate "relay", the selected pair
// relay/relay), audio flows both ways, and the call outlives the TURN credentials (the server issues them for
// TURN_CREDENTIAL_TTL seconds): the caller renews at 80% of what's left and restarts ICE (ui/src/lib/turnRenewal.ts).
// --block-renewal makes the renewal request fail, as a control: then the call must die once the relay's credentials lapse.
import { appendFileSync, writeFileSync } from "node:fs";
import { launch, sleep } from "./cdp.mjs";
import { INIT_SCRIPT, acceptRequest, addContact, answerCall, callStats, openChat, placeVoiceCall, signUp } from "./app_driver.mjs";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith("--") ? [...acc, [a.slice(2), all[i + 1]?.startsWith("--") || all[i + 1] === undefined ? true : all[i + 1]]] : acc), [])
);
const base = args.base ?? "http://localhost:18081/";
const minutes = Number(args.minutes ?? 2);
const tag = args.tag ?? String(Date.now()).slice(-5);
const blockRenewal = Boolean(args["block-renewal"]);
const out = args.out ?? `call-${tag}.jsonl`;
const profileRoot = args.profiles ?? "C:\\Users\\user\\AppData\\Local\\Temp\\rv-edge-profiles";
const basePort = Number(args.port ?? 9231);
const password = `correct horse battery staple ${tag}`;
writeFileSync(out, "");
const log = (obj) => {
  const line = JSON.stringify({ t: Math.round((Date.now() - started) / 1000), ...obj });
  console.log(line);
  appendFileSync(out, line + "\n");
};
const started = Date.now();

const alice = await launch(`alice-${tag}`, basePort, { profileRoot, initScript: INIT_SCRIPT(blockRenewal) });
const bob = await launch(`bob-${tag}`, basePort + 1, { profileRoot, initScript: INIT_SCRIPT(blockRenewal) });
let failed = false;
try {
  await Promise.all([signUp(alice.page, base, `callalice${tag}`, password), signUp(bob.page, base, `callbob${tag}`, password)]);
  log({ step: "both signed up" });
  await addContact(alice.page, `callbob${tag}`);
  await acceptRequest(bob.page);
  log({ step: "friends" });
  await openChat(alice.page, `callbob${tag}`);
  await openChat(bob.page, `callalice${tag}`);
  await placeVoiceCall(alice.page);
  log({ step: "alice called" });
  await answerCall(bob.page);
  log({ step: "bob answered" });

  // Connected on both sides?
  let connected = false;
  for (let i = 0; i < 60 && !connected; i++) {
    const [a, b] = [await callStats(alice.page), await callStats(bob.page)];
    connected = a.connectionState === "connected" && b.connectionState === "connected";
    if (!connected) await sleep(1000);
  }
  const [a0, b0] = [await callStats(alice.page), await callStats(bob.page)];
  log({ step: "connected", connected, alice: a0, bob: b0 });
  const relayOnly = (s) =>
    s.iceTransportPolicy === "relay" && s.localCandidateTypes.length > 0 && s.localCandidateTypes.every((t) => t === "relay") && s.selectedLocalType === "relay";
  log({ check: "relay only", alice: relayOnly(a0), bob: relayOnly(b0) });
  if (!connected || !relayOnly(a0) || !relayOnly(b0)) failed = true;

  // Watch the call.
  const end = Date.now() + minutes * 60 * 1000;
  let last = { a: a0.audioPacketsReceived, b: b0.audioPacketsReceived };
  let alive = true;
  let firstDeadAt = null;
  while (Date.now() < end) {
    await sleep(20000);
    const [a, b] = [await callStats(alice.page).catch((e) => ({ error: e.message })), await callStats(bob.page).catch((e) => ({ error: e.message }))];
    const flowing = (s, prev) => s.audioPacketsReceived > prev;
    const aFlow = !a.error && flowing(a, last.a);
    const bFlow = !b.error && flowing(b, last.b);
    log({
      probe: true,
      alice: { state: a.connectionState, ice: a.iceConnectionState, rx: a.audioPacketsReceived, flowing: aFlow, pcs: a.pcs, renewals: a.renewalCalls, user: a.iceServerUsername?.slice(0, 18), errs: a.rtcErrors, log: a.rtcLog },
      bob: { state: b.connectionState, ice: b.iceConnectionState, rx: b.audioPacketsReceived, flowing: bFlow, pcs: b.pcs, errs: b.rtcErrors },
    });
    if (!a.error) last.a = a.audioPacketsReceived;
    if (!b.error) last.b = b.audioPacketsReceived;
    if (!(aFlow && bFlow) && alive) {
      alive = false;
      firstDeadAt = Math.round((Date.now() - started) / 1000);
    }
  }
  const [aEnd, bEnd] = [await callStats(alice.page).catch(() => ({})), await callStats(bob.page).catch(() => ({}))];
  log({ summary: true, blockRenewal, minutes, callSurvived: alive, firstStallAtSeconds: firstDeadAt, aliceRenewals: aEnd.renewalCalls, aliceState: aEnd.connectionState, bobState: bEnd.connectionState });
  if (blockRenewal ? alive : !alive) failed = true; // control must die, the real run must live
} catch (e) {
  log({ error: e.message });
  failed = true;
} finally {
  alice.stop();
  bob.stop();
}
process.exit(failed ? 1 : 0);
