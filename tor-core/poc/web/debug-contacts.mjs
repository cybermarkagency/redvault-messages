// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Debug helper: what the Contacts screen offers in a headless browser (selectors drift as the UI changes).
import { launch, sleep } from "./cdp.mjs";
import { signUp, INIT_SCRIPT } from "./app_driver.mjs";

const b = await launch("dbg", 9251, { profileRoot: "C:\\Users\\user\\AppData\\Local\\Temp\\rv-edge-profiles", initScript: INIT_SCRIPT(false) });
try {
  await signUp(b.page, "http://localhost:18081/", "dbgalice1", "correct horse battery staple dbg1");
  console.log("viewport", await b.page.eval("innerWidth + 'x' + innerHeight"));
  console.log("nav items:", await b.page.eval("[...document.querySelectorAll('nav button, nav a')].map(e => JSON.stringify(e.innerText.trim()))"));
  await b.page.eval(`[...document.querySelectorAll('nav button, nav a')].find(e => /^\\d*\\s*Contacts$/.test(e.innerText.trim())).click()`);
  await sleep(1000);
  console.log("buttons:", await b.page.eval("[...document.querySelectorAll('button')].map(e => JSON.stringify((e.innerText.trim() || '') + '|' + (e.getAttribute('aria-label') || '') + '|' + (e.title || ''))).slice(0, 30)"));
} finally {
  b.stop();
}
