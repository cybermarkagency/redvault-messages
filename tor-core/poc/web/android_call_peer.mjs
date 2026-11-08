// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The web side of the Android live-call probe (android/app/src/androidTest/.../calls/CallRelayProbeTest.kt): a real web-app
// user in a headless browser that accepts the phone's friend request, answers the phone's call, then calls the phone back,
// and watches both calls with the browser's own WebRTC statistics (relay-only, audio arriving).
//
//   node android_call_peer.mjs --base http://localhost:18081/ --tag a1 --seconds 40 --out peer.jsonl
//   (web handle: webpeer<tag>; the phone's handle: droidcall<tag>; run the instrumented test with the same tag)
import { appendFileSync, writeFileSync } from "node:fs";
import { launch, sleep } from "./cdp.mjs";
import { INIT_SCRIPT, acceptRequest, answerCall, callStats, openChat, placeVoiceCall, signUp } from "./app_driver.mjs";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => (a.startsWith("--") ? [...acc, [a.slice(2), all[i + 1]?.startsWith("--") || all[i + 1] === undefined ? true : all[i + 1]]] : acc), [])
);
const base = args.base ?? "http://localhost:18081/";
const tag = args.tag ?? String(Date.now()).slice(-5);
const seconds = Number(args.seconds ?? 40);
const out = args.out ?? `android-call-peer-${tag}.jsonl`;
const profileRoot = args.profiles ?? "C:\\Users\\user\\AppData\\Local\\Temp\\rv-edge-profiles";
const port = Number(args.port ?? 9341);
const webHandle = `webpeer${tag}`;
const phoneHandle = `droidcall${tag}`;
const password = `correct horse battery staple ${tag}`;
const started = Date.now();
writeFileSync(out, "");
const log = (obj) => {
  const line = JSON.stringify({ t: Math.round((Date.now() - started) / 1000), ...obj });
  console.log(line);
  appendFileSync(out, line + "\n");
};

const relayOnly = (s) => s.iceTransportPolicy === "relay" && s.localCandidateTypes.length > 0 && s.localCandidateTypes.every((t) => t === "relay") && s.selectedLocalType === "relay" && s.selectedRemoteType === "relay";
let failed = false;

/** Samples the current call every 5 s until it ends or `limitMs` passes; returns the samples. */
async function watch(page, label, limitMs) {
  const samples = [];
  const end = Date.now() + limitMs;
  while (Date.now() < end) {
    await sleep(5000);
    const s = await callStats(page).catch((e) => ({ error: e.message }));
    samples.push(s);
    log({ probe: label, state: s.connectionState, rx: s.audioPacketsReceived, tx: s.audioPacketsSent, pair: `${s.selectedLocalType}/${s.selectedRemoteType}` });
    if (s.connectionState === "closed" || s.connectionState === "failed") break;
  }
  return samples;
}

function verdict(label, samples) {
  const live = samples.filter((s) => s.connectionState === "connected");
  const first = live[0], last = live[live.length - 1];
  const relay = !!last && relayOnly(last);
  const flowing = !!first && !!last && last.audioPacketsReceived > first.audioPacketsReceived;
  log({ check: `${label}: relay only (policy relay, every candidate relay, selected pair relay/relay)`, ok: relay });
  log({ check: `${label}: audio from the phone keeps arriving`, ok: flowing, rx: `${first?.audioPacketsReceived}->${last?.audioPacketsReceived}` });
  if (!relay || !flowing) failed = true;
}

const web = await launch(`webpeer-${tag}`, port, { profileRoot, initScript: INIT_SCRIPT(false) });
try {
  await signUp(web.page, base, webHandle, password);
  log({ step: "web user signed up", handle: webHandle, waitingFor: phoneHandle });

  // The phone registers and sends its friend request whenever the instrumented test gets going.
  let accepted = false;
  for (let i = 0; i < 8 && !accepted; i++) {
    try {
      await acceptRequest(web.page);
      accepted = true;
    } catch {
      log({ waiting: "friend request from the phone" });
    }
  }
  if (!accepted) throw new Error("no friend request from the phone");
  log({ step: "friend request accepted" });
  await openChat(web.page, phoneHandle);

  // Direction 1: the phone calls; we answer.
  let answered = false;
  for (let i = 0; i < 6 && !answered; i++) {
    try {
      await answerCall(web.page);
      answered = true;
    } catch {
      log({ waiting: "incoming call" });
    }
  }
  if (!answered) throw new Error("the phone's call never rang here");
  log({ step: "answered the phone's call" });
  for (let i = 0; i < 60; i++) {
    if ((await callStats(web.page)).connectionState === "connected") break;
    await sleep(1000);
  }
  verdict("phone calls web", await watch(web.page, "phone-calls-web", (seconds + 40) * 1000));

  // Direction 2: we call the phone, which answers by itself.
  await sleep(6000);
  await openChat(web.page, phoneHandle);
  await placeVoiceCall(web.page);
  log({ step: "called the phone" });
  for (let i = 0; i < 90; i++) {
    const s = await callStats(web.page);
    if (s.pcs >= 2 && s.connectionState === "connected") break;
    await sleep(1000);
  }
  verdict("web calls phone", await watch(web.page, "web-calls-phone", (seconds + 30) * 1000));
} catch (e) {
  log({ error: e.message });
  failed = true;
} finally {
  web.stop();
}
log({ summary: failed ? "FAILED" : "all checks passed" });
process.exit(failed ? 1 : 0);
