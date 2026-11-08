// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The AGPL source offer's build configuration (src/lib/sourceOffer.ts).
import { describe, expect, it } from "vitest";
import { APP_VERSION, legalFileUrl, parseHttpsUrl, resolveSourceOffer } from "../lib/sourceOffer.js";

describe("source offer configuration", () => {
  it("has no source link and no contact when nothing is configured", () => {
    expect(resolveSourceOffer({}, "1.2.3")).toEqual({ sourceUrl: null, contact: null, version: "1.2.3" });
    expect(resolveSourceOffer({ VITE_SOURCE_CODE_URL: "", VITE_SOURCE_CONTACT: "   " }, "1.2.3")).toEqual({
      sourceUrl: null,
      contact: null,
      version: "1.2.3",
    });
  });

  it("keeps an https URL", () => {
    expect(resolveSourceOffer({ VITE_SOURCE_CODE_URL: " https://code.example.org/redvault/tree/v0.1.0 " }).sourceUrl).toBe(
      "https://code.example.org/redvault/tree/v0.1.0",
    );
  });

  it("treats http, other schemes, relative paths and URLs with credentials as unset", () => {
    for (const raw of ["http://code.example.org/redvault", "javascript:alert(1)", "ftp://example.org/src", "/src", "not a url", "https://user:pw@example.org/src"]) {
      expect(parseHttpsUrl(raw)).toBeNull();
      expect(resolveSourceOffer({ VITE_SOURCE_CODE_URL: raw }).sourceUrl).toBeNull();
    }
  });

  it("reads the contact as an email address, an https URL, or plain text; unset falls back (null)", () => {
    expect(resolveSourceOffer({ VITE_SOURCE_CONTACT: "source@example.org" }).contact).toEqual({ kind: "email", address: "source@example.org" });
    expect(resolveSourceOffer({ VITE_SOURCE_CONTACT: "mailto:source@example.org" }).contact).toEqual({ kind: "email", address: "source@example.org" });
    expect(resolveSourceOffer({ VITE_SOURCE_CONTACT: "https://example.org/contact" }).contact).toEqual({ kind: "url", url: "https://example.org/contact" });
    // Never turned into a link unless it's https.
    expect(resolveSourceOffer({ VITE_SOURCE_CONTACT: "http://example.org/contact" }).contact).toEqual({ kind: "text", text: "http://example.org/contact" });
    expect(resolveSourceOffer({ VITE_SOURCE_CONTACT: "" }).contact).toBeNull();
  });

  it("has package.json's version and puts the legal files under the base path", async () => {
    const pkg = (await import("../../package.json")).default as { version: string };
    expect(APP_VERSION).toBe(pkg.version);
    expect(legalFileUrl("LICENSE.txt", "/redvaultmessages/")).toBe("/redvaultmessages/legal/LICENSE.txt");
    expect(legalFileUrl("THIRD_PARTY_NOTICES.txt", "/")).toBe("/legal/THIRD_PARTY_NOTICES.txt");
    expect(legalFileUrl("LICENSE.txt", "/other")).toBe("/other/legal/LICENSE.txt");
  });
});
