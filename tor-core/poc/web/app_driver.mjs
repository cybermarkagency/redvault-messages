// The real web app's user actions, as page scripts (sign up, add a friend, accept, open a chat, call).
// Each is what a person does with the screen: real clicks on real buttons, found by their visible text or label.
import { sleep } from "./cdp.mjs";

/** Runs the same lookup in the page until it returns something truthy, or throws after `timeout` ms. */
async function waitFor(page, expression, timeout = 30000, label = expression) {
  const end = Date.now() + timeout;
  let last;
  while (Date.now() < end) {
    try {
      // Truthiness is decided inside the page: a DOM element can't be sent back by value.
      last = await page.eval(`!!(${expression})`);
      if (last) return last;
    } catch (e) {
      last = e.message;
    }
    await sleep(400);
  }
  throw new Error(`timed out waiting for ${label.slice(0, 90)} (last: ${String(last).slice(0, 120)})`);
}

// The app covers itself when the window loses focus; this uncovers it (as a statement, and as an expression).
const unlockStatement = `const u = [...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Unlock'); if (u) u.click();`;
const unlock = `(() => { ${unlockStatement} return true; })()`;

export async function signUp(page, base, handle, password) {
  await page.navigate(base);
  await waitFor(page, `document.body.innerText.includes('Create') || document.body.innerText.includes('Log in')`, 30000, "the sign-up screen");
  await page.eval(`(() => { const link = [...document.querySelectorAll('button')].find(b => /Create a new identity instead/.test(b.textContent)); if (link) link.click(); return true; })()`);
  await sleep(500);
  await page.eval(`(async () => {
    const set = (el, v) => { Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value').set.call(el, v); el.dispatchEvent(new Event('input', {bubbles: true})); };
    const q = (p) => [...document.querySelectorAll('input')].find(i => i.placeholder === p);
    set(q('Choose a handle'), ${JSON.stringify(handle)});
    set(q('Choose a password (min. 8 characters)'), ${JSON.stringify(password)});
    set(q('Confirm password'), ${JSON.stringify(password)});
    [...document.querySelectorAll('input[type=checkbox]')].forEach(c => { if (!c.checked) c.click(); });
    await new Promise(r => setTimeout(r, 300));
    [...document.querySelectorAll('button')].find(b => b.textContent.trim() === 'Create Account').click();
    return true;
  })()`);
  await waitFor(page, `/saved my recovery key/i.test(document.body.innerText)`, 60000, "the recovery key screen");
  await page.eval(`(async () => {
    const cb = [...document.querySelectorAll('input[type=checkbox]')].find(c => /saved my recovery key/i.test(c.closest('label')?.innerText || c.parentElement.innerText));
    cb.click(); await new Promise(r => setTimeout(r, 300));
    [...document.querySelectorAll('button')].find(b => /^done$/i.test(b.textContent.trim())).click();
    return true;
  })()`);
  await waitFor(page, `/No conversations yet/.test(document.body.innerText)`, 30000, "the empty chat list");
}

export async function addContact(page, handle) {
  await page.eval(unlock);
  await page.eval(`[...document.querySelectorAll('nav button, nav a')].find(e => /^\\d*\\s*Contacts$/.test(e.innerText.trim())).click()`);
  await sleep(600);
  await waitFor(page, `[...document.querySelectorAll('button')].find(b => (b.innerText.trim() || b.getAttribute('aria-label')) === 'Add a contact')`, 15000, "Add a contact");
  await page.eval(`[...document.querySelectorAll('button')].find(b => (b.innerText.trim() || b.getAttribute('aria-label')) === 'Add a contact').click()`);
  await waitFor(page, `[...document.querySelectorAll('input')].find(i => i.placeholder === 'their-handle·xxxx')`, 15000, "the handle field");
  await page.eval(`(() => { const el = [...document.querySelectorAll('input')].find(i => i.placeholder === 'their-handle·xxxx'); Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value').set.call(el, ${JSON.stringify(handle)}); el.dispatchEvent(new Event('input', {bubbles: true})); return true; })()`);
  await page.eval(`[...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Look up').click()`);
  await waitFor(page, `[...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Send friend request' && !b.disabled)`, 30000, "Send friend request");
  await page.eval(`[...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Send friend request').click()`);
  await sleep(1500);
}

export async function acceptRequest(page) {
  await waitFor(page, `(() => { ${unlockStatement} const nav = [...document.querySelectorAll('nav button, nav a')].find(e => /^\\d*\\s*Chats$/.test(e.innerText.trim())); if (nav) nav.click(); return [...document.querySelectorAll('button')].find(b => /friend request/i.test(b.innerText)); })()`, 90000, "the friend request banner");
  await page.eval(`[...document.querySelectorAll('button')].find(b => /friend request/i.test(b.innerText)).click()`);
  await waitFor(page, `[...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Accept')`, 20000, "Accept");
  await page.eval(`[...document.querySelectorAll('button')].find(b => b.innerText.trim() === 'Accept').click()`);
  await sleep(3000);
}

export async function openChat(page, handle) {
  await page.eval(unlock);
  await page.eval(`[...document.querySelectorAll('nav button, nav a')].find(e => /^\\d*\\s*Chats$/.test(e.innerText.trim())).click()`);
  await waitFor(
    page,
    `(() => { const row = [...document.querySelectorAll('button, a, [role=button]')].filter(e => e.innerText.includes(${JSON.stringify(handle)}) && e.innerText.length < 200 && !e.closest('nav')).sort((a, b) => a.innerText.length - b.innerText.length)[0]; if (row) { row.click(); return true; } return false; })()`,
    90000,
    "the conversation in the list"
  );
  await sleep(1000);
}

export async function placeVoiceCall(page) {
  await waitFor(page, `[...document.querySelectorAll('button')].find(b => (b.getAttribute('aria-label') || '').startsWith('Voice call'))`, 60000, "the Voice call button (calls enabled on the server?)");
  await page.eval(`[...document.querySelectorAll('button')].find(b => (b.getAttribute('aria-label') || '').startsWith('Voice call')).click()`);
}

export async function answerCall(page) {
  await waitFor(page, `(() => { ${unlockStatement} return [...document.querySelectorAll('button')].find(b => /^(answer|accept)/i.test((b.getAttribute('aria-label') || b.innerText).trim())); })()`, 60000, "the Answer button");
  await page.eval(`[...document.querySelectorAll('button')].find(b => /^(answer|accept)/i.test((b.getAttribute('aria-label') || b.innerText).trim())).click()`);
}

/** Wraps RTCPeerConnection so every connection's configuration and instance can be inspected, and (optionally) blocks the renewal endpoint. */
export const INIT_SCRIPT = (blockRenewal) => `
(() => {
  window.__pcs = [];
  window.__renewalCalls = [];
  const Orig = window.RTCPeerConnection;
  if (Orig) {
    window.RTCPeerConnection = new Proxy(Orig, {
      construct(target, args) {
        const pc = new target(...args);
        window.__pcs.push({ pc, config: JSON.parse(JSON.stringify(args[0] || {})), createdAt: Date.now() });
        return pc;
      },
    });
  }
  // Errors from the calls that drive an ICE restart (the app swallows them and retries a minute later).
  window.__rtcErrors = [];
  window.__rtcLog = [];
  if (Orig) {
    for (const m of ['setConfiguration', 'createOffer', 'createAnswer', 'setLocalDescription', 'setRemoteDescription']) {
      const original = Orig.prototype[m];
      if (!original) continue;
      Orig.prototype[m] = function (...a) {
        try {
          const r = original.apply(this, a);
          if (r && r.catch) r.catch((e) => window.__rtcErrors.push({ m, name: e.name, msg: String(e.message).slice(0, 240) }));
          if (m === 'setLocalDescription' && r && r.then) {
            const pc = this;
            r.then(() => window.__rtcLog.push({ m, type: a[0] && a[0].type, gathering: pc.iceGatheringState, relayCandidatesInLocalSdp: ((pc.localDescription && pc.localDescription.sdp) || '').split(' typ relay ').length - 1, at: Date.now() % 1000000 }));
          }
          return r;
        } catch (e) {
          window.__rtcErrors.push({ m, name: e.name, msg: String(e.message).slice(0, 240), sync: true });
          throw e;
        }
      };
    }
  }
  const origFetch = window.fetch.bind(window);
  window.fetch = (input, init) => {
    const url = typeof input === 'string' ? input : input.url;
    if (/relay-credentials/.test(url)) {
      window.__renewalCalls.push({ at: Date.now(), blocked: ${blockRenewal ? "true" : "false"} });
      if (${blockRenewal ? "true" : "false"}) return Promise.reject(new TypeError('blocked for the control run'));
    }
    return origFetch(input, init);
  };
})();`;

/** What the call is doing now, from the browser's own WebRTC statistics. */
export async function callStats(page) {
  return page.eval(`(async () => {
    const entry = window.__pcs[window.__pcs.length - 1];
    if (!entry) return { pcs: 0 };
    const pc = entry.pc;
    const report = await pc.getStats();
    const byId = {};
    report.forEach(s => { byId[s.id] = s; });
    let pair = null;
    report.forEach(s => { if (s.type === 'transport' && s.selectedCandidatePairId) pair = byId[s.selectedCandidatePairId]; });
    if (!pair) report.forEach(s => { if (s.type === 'candidate-pair' && (s.nominated || s.selected) && s.state === 'succeeded') pair = s; });
    const local = pair && byId[pair.localCandidateId], remote = pair && byId[pair.remoteCandidateId];
    const localTypes = []; report.forEach(s => { if (s.type === 'local-candidate') localTypes.push(s.candidateType); });
    let inbound = 0, outbound = 0, audioIn = 0;
    report.forEach(s => { if (s.type === 'inbound-rtp' && s.kind === 'audio') { inbound = s.packetsReceived; audioIn = s.bytesReceived; } if (s.type === 'outbound-rtp' && s.kind === 'audio') outbound = s.packetsSent; });
    return {
      pcs: window.__pcs.length,
      connectionState: pc.connectionState, iceConnectionState: pc.iceConnectionState,
      iceTransportPolicy: entry.config.iceTransportPolicy,
      iceServerUrls: (entry.config.iceServers || []).map(s => s.urls),
      iceServerUsername: ((entry.config.iceServers || [])[0] || {}).username,
      selectedLocalType: local && local.candidateType, selectedRemoteType: remote && remote.candidateType,
      localCandidateTypes: [...new Set(localTypes)],
      audioPacketsReceived: inbound, audioBytesReceived: audioIn, audioPacketsSent: outbound,
      renewalCalls: window.__renewalCalls.length,
      rtcErrors: (window.__rtcErrors || []).slice(-4),
      rtcLog: (window.__rtcLog || []).slice(-6),
    };
  })()`);
}
