// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/// <reference types="vitest/config" />
import react from "@vitejs/plugin-react";
import { defineConfig, searchForWorkspaceRoot, type Plugin } from "vite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { CONTENT_SECURITY_POLICY, SHARE_CONTENT_SECURITY_POLICY } from "./csp.js";
import { renderLegalPage } from "./src/lib/legalPages.js";

/** Production builds only — Vite's dev HMR needs an inline script the policy would block. */
function contentSecurityPolicy(): Plugin {
  return {
    name: "content-security-policy",
    apply: "build",
    transformIndexHtml(html, ctx) {
      // The share page gets its own, stricter policy.
      const policy = /[\/]share[\/]index\.html$/.test(ctx.filename) ? SHARE_CONTENT_SECURITY_POLICY : CONTENT_SECURITY_POLICY;
      return html.replace(
        "<head>",
        `<head>
    <meta http-equiv="Content-Security-Policy" content="${policy}" />`
      );
    },
  };
}

/**
 * privacy.html and terms.html, made from legal/*.md (the text every app shows
 * at sign-up) so the public pages never drift from the apps: served by the
 * dev server, and written next to index.html by the build.
 */
function legalPages(): Plugin {
  const read = (file: string) => readFileSync(new URL(`../legal/${file}`, import.meta.url), "utf8");
  const pages = (): Record<string, string> => ({
    "privacy.html": renderLegalPage("privacy", read("privacy-policy.md")),
    "terms.html": renderLegalPage("terms", read("terms-of-service.md")),
  });
  return {
    name: "legal-pages",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const page = pages()[(req.url ?? "").split("?")[0].replace(/^\//, "")];
        if (!page) return next();
        res.setHeader("Content-Type", "text/html; charset=utf-8");
        res.end(page);
      });
    },
    generateBundle() {
      for (const [fileName, source] of Object.entries(pages())) this.emitFile({ type: "asset", fileName, source });
    },
  };
}

/**
 * Where the built app is served. Production lives under
 * www.redvaultsystems.com/redvaultmessages/, so a plain `npm run build`
 * targets that path (asset URLs and, via BASE_URL, the API in
 * src/lib/apiBase.ts). Override with VITE_BASE=/other/path/ to host it
 * elsewhere; the dev server stays at /.
 */
const PRODUCTION_BASE = process.env.VITE_BASE ?? "/redvaultmessages/";

/** The version shown with the source offer (src/lib/sourceOffer.ts). */
const APP_VERSION: string = JSON.parse(readFileSync(new URL("./package.json", import.meta.url), "utf8")).version;

export default defineConfig(({ command }) => ({
  base: command === "build" ? PRODUCTION_BASE : "/",
  plugins: [react(), contentSecurityPolicy(), legalPages()],
  define: { __APP_VERSION__: JSON.stringify(APP_VERSION) },
  // public/ (Vite's default publicDir) is copied as-is to the build output,
  // so public/legal/LICENSE.txt and THIRD_PARTY_NOTICES.txt are served at
  // <base>legal/ next to index.html.
  publicDir: "public",
  build: {
    rollupOptions: {
      // The app, and the public share page as its own small bundle (share/, docs/public-share-links.md).
      input: {
        main: fileURLToPath(new URL("./index.html", import.meta.url)),
        share: fileURLToPath(new URL("./share/index.html", import.meta.url)),
      },
    },
  },
  server: {
    // The Terms of Service and Privacy Policy live in ../legal (shared with
    // the desktop and Android apps); the dev server may serve them too. So
    // may the shared encryption engine's web build (crypto-core/build-web.sh),
    // used only by the protocol-3 preview (lib/e2eV3).
    fs: { allow: [searchForWorkspaceRoot(process.cwd()), "../legal", "../crypto-core/pkg"] },
    // Same-origin from the browser's perspective, so the session cookie
    // (SameSite=Lax) just works — no CORS config needed on the server.
    // REDVAULT_DEV_API points it elsewhere (e.g. the Tor PoC harness: tor-core/poc/harness).
    proxy: {
      // VITE_API_PROXY (or REDVAULT_DEV_API) points the dev server at another local API (e.g. a test harness on another port).
      "/api": process.env.VITE_API_PROXY ?? process.env.REDVAULT_DEV_API ?? "http://localhost:3000",
    },
  },
  test: {
    environment: "jsdom",
    setupFiles: ["./src/test/setup.ts"],
    globals: true,
  },
}));
