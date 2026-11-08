// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The public privacy.html and terms.html, built from legal/*.md.
import { describe, expect, it } from "vitest";
import { LEGAL_DOCUMENTS } from "../lib/legalDocuments.js";
import { renderLegalPage } from "../lib/legalPages.js";

describe("public legal pages", () => {
  const privacy = renderLegalPage("privacy", LEGAL_DOCUMENTS.privacy);
  const terms = renderLegalPage("terms", LEGAL_DOCUMENTS.terms);

  it("are complete standalone pages with the documents' titles", () => {
    expect(privacy.startsWith("<!doctype html>")).toBe(true);
    expect(privacy).toContain("<title>RedVault Messages – Privacy Policy</title>");
    expect(terms).toContain("<title>RedVault Messages – Terms of Service</title>");
    expect(privacy).toContain('<meta name="viewport" content="width=device-width, initial-scale=1">');
  });

  it("carry every section, each one linkable", () => {
    expect(privacy.match(/<h2 id="section-\d+">/g)).toHaveLength(26);
    expect(terms.match(/<h2 id="section-\d+">/g)).toHaveLength(27);
    expect(terms).toContain('<h2 id="section-7">7. NOT AN EMERGENCY OR LIFE-SAFETY SERVICE</h2>');
    expect(privacy).toContain("<h3>Payment Information</h3>");
    expect(privacy).toContain("<li>You can create a messaging identity using a username without supplying an email address or telephone number.</li>");
  });

  it("keep the account deletion steps Google Play links to", () => {
    expect(privacy).toContain('id="delete-account"');
    expect(privacy).toContain("Settings → Account and plan → Delete my account");
    expect(terms).not.toContain('id="delete-account"');
  });

  it("link to each other and to the contact address", () => {
    expect(privacy).toContain('<a href="terms.html">Terms of Service</a>');
    expect(terms).toContain('<a href="privacy.html">Privacy Policy</a>');
    expect(terms).toContain('<a href="mailto:cybermarkagency@gmail.com">cybermarkagency@gmail.com</a>');
  });

  it("escape the text and load nothing from anywhere else", () => {
    const page = renderLegalPage("terms", '# A <b>&</b> "test"\n\n## 1. <script>alert(1)</script>\n\n- x < y');
    expect(page).toContain("<title>A &lt;b&gt;&amp;&lt;/b&gt; &quot;test&quot;</title>");
    expect(page).toContain("1. &lt;script&gt;alert(1)&lt;/script&gt;");
    expect(page).not.toContain("<script");
    for (const html of [privacy, terms]) {
      expect(html).not.toMatch(/<script|<link|<img|src=|@import|url\(/);
      // Only links within the site, and mail.
      for (const href of html.match(/href="[^"]+"/g) ?? []) {
        expect(href).toMatch(/^href="(privacy\.html|terms\.html|\.\/|mailto:cybermarkagency@gmail\.com)"$/);
      }
    }
  });
});
