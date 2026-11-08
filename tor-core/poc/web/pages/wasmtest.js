// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// What a page under the Redvault web app's policy can do in this browser, at its current security level.
// The tiny module is the smallest valid WebAssembly binary: "\0asm" + version 1.
(async () => {
  const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
  const result = { js: true, secureContext: window.isSecureContext, hasWebAssembly: typeof WebAssembly === "object" };
  try { result.compileSync = String(new WebAssembly.Module(bytes)).slice(0, 30); } catch (e) { result.compileSync = "ERR " + e.name + ": " + String(e.message).slice(0, 140); }
  try { await WebAssembly.instantiate(bytes); result.instantiate = "ok"; } catch (e) { result.instantiate = "ERR " + e.name + ": " + String(e.message).slice(0, 140); }
  document.title = "wasmtest: " + (result.instantiate === "ok" && !String(result.compileSync).startsWith("ERR") ? "wasm ok" : "wasm blocked");
  document.getElementById("out").textContent = JSON.stringify(result);
})();
