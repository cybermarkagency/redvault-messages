// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Terms of Service and Privacy Policy bundled for the admin panel's create-account form.
import { describe, expect, it } from "vitest";
import { LEGAL_DOCUMENTS, parseLegalDocument } from "../lib/legalDocuments.js";

describe("legal documents", () => {
  it("parse into a title, headings, paragraphs and lists", () => {
    expect(parseLegalDocument("# T\n\nIntro\n\n## 1. A\n\n- x;\n- y\n\n### Sub\n\n1. one\n2. two\n\nEnd")).toEqual([
      { kind: "title", text: "T" },
      { kind: "paragraph", text: "Intro" },
      { kind: "heading", text: "1. A" },
      { kind: "bullets", items: ["x;", "y"] },
      { kind: "subheading", text: "Sub" },
      { kind: "numbered", items: ["one", "two"] },
      { kind: "paragraph", text: "End" },
    ]);
  });

  it("are both bundled, with every section", () => {
    const sections = (id: "terms" | "privacy") => parseLegalDocument(LEGAL_DOCUMENTS[id]).filter((b) => b.kind === "heading");
    expect(sections("terms")).toHaveLength(27);
    expect(sections("privacy")).toHaveLength(26);
    expect(LEGAL_DOCUMENTS.terms).toContain("Effective Date: 6 October 2026");
    expect(LEGAL_DOCUMENTS.privacy).toContain("Effective Date: 19 November 2026");
  });
});
