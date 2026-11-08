// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Proof that the web app itself never collects location or usage data, and
// never talks to a third party that could see a visitor's IP address.
// Scans every shipped source file (tests excluded) plus index.html and the
// production Content-Security-Policy.
import { readdirSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { CONTENT_SECURITY_POLICY } from "../../csp.js";
import { resolveApiUrl } from "../lib/apiBase.js";

const SRC = resolve(__dirname, "..");
const sources: [string, string][] = (readdirSync(SRC, { recursive: true }) as string[])
  .filter((file) => /\.(ts|tsx|css)$/.test(file) && !/^test[\\/]/.test(file))
  .map((file) => [file, readFileSync(join(SRC, file), "utf8")]);

describe("the web app collects no location or usage data", () => {
  it("scans a real set of source files", () => {
    expect(sources.length).toBeGreaterThan(20);
  });

  const forbidden: [string, RegExp][] = [
    ["location lookups", /navigator\.geolocation|getCurrentPosition|watchPosition/],
    ["timezone/locale reporting", /resolvedOptions\(\)\.timeZone|navigator\.languages?/],
    ["device fingerprinting", /navigator\.(userAgent|platform|hardwareConcurrency|deviceMemory)|userAgentData|screen\.(width|height)/],
    ["background beacons", /sendBeacon/],
    ["analytics / tracking SDKs", /google-analytics|googletagmanager|gtag\(|mixpanel|segment\.|amplitude|posthog|sentry|hotjar|fbq\(|plausible/i],
    ["third-party network requests", /(fetch|src=|href=|url\()\s*\(?\s*["'`]https?:\/\//],
    ["third-party fonts/CDNs", /fonts\.googleapis|fonts\.gstatic|cdnjs|unpkg\.com|jsdelivr/],
  ];

  for (const [what, pattern] of forbidden) {
    it(`contains no ${what}`, () => {
      const offenders = sources.filter(([, code]) => pattern.test(code)).map(([file]) => file);
      expect(offenders).toEqual([]);
    });
  }

  it("index.html tells the browser never to send a Referer", () => {
    const html = readFileSync(resolve(__dirname, "../../index.html"), "utf8");
    expect(html).toContain('<meta name="referrer" content="no-referrer" />');
    expect(html).not.toMatch(/https?:\/\//);
  });

  it("the production build only allows talking to its own origin", () => {
    expect(CONTENT_SECURITY_POLICY).toContain("default-src 'self'");
    expect(CONTENT_SECURITY_POLICY).toContain("connect-src 'self'");
    expect(CONTENT_SECURITY_POLICY).toContain("script-src 'self' 'wasm-unsafe-eval'");
    // WebAssembly for the crypto library is allowed; JS eval() is not.
    expect(CONTENT_SECURITY_POLICY).not.toContain("'unsafe-eval'");
    expect(CONTENT_SECURITY_POLICY).toContain("img-src 'self' data: blob:");
    expect(CONTENT_SECURITY_POLICY).not.toMatch(/https?:|\*/);
  });

  it("avatar URLs from the server stay on this app's own API", () => {
    expect(resolveApiUrl("/api/users/abc/avatar?v=1")).toBe("/api/users/abc/avatar?v=1");
  });
});
