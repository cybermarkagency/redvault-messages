// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import App from "./App.js";
import { WebAssemblyGate } from "./components/WebAssemblyGate.js";
import { SharedFile } from "./screens/SharedFile.js";
import "./styles/global.css";

// A share link to a file from the apps' Files (…?file=<id>#<secret>) opens on its own: no sign-in.
const sharedFileId = new URLSearchParams(window.location.search).get("file");

// The file viewer decrypts in the browser, so it needs WebAssembly (libsodium). The website
// itself shows its pages and downloads without it; only signing in asks for it (site/SignIn.tsx).
createRoot(document.getElementById("root")!).render(
  <StrictMode>
    {sharedFileId ? (
      <WebAssemblyGate>
        <SharedFile linkId={sharedFileId} />
      </WebAssemblyGate>
    ) : (
      <App />
    )}
  </StrictMode>
);
