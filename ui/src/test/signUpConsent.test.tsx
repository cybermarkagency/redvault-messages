// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The sign-up form's three boxes: Terms of Service, Privacy Policy, 18 or older.
import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { LEGAL_DOCUMENTS, parseLegalDocument } from "../lib/legalDocuments.js";
import { resetFakeBackend } from "./fakeApiBackend.js";
import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";

vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

describe("sign-up consent", () => {
  beforeEach(() => {
    resetFakeBackend();
    localStorage.clear();
  });

  async function fillFields(user: ReturnType<typeof userEvent.setup>) {
    render(<App />);
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
  }

  it("shows the three boxes unticked, and Create Account stays disabled until all are ticked", async () => {
    const user = userEvent.setup();
    await fillFields(user);
    const terms = screen.getByRole("checkbox", { name: "I agree to the RedVault Messages Terms of Service" });
    const privacy = screen.getByRole("checkbox", { name: "I acknowledge the RedVault Messages Privacy Policy" });
    const age = screen.getByRole("checkbox", { name: "I confirm I am 18 or older" });
    const create = screen.getByRole("button", { name: /^create account$/i });
    for (const box of [terms, privacy, age]) expect(box).not.toBeChecked();

    expect(create).toBeDisabled();
    await user.click(terms);
    expect(create).toBeDisabled();
    await user.click(privacy);
    expect(create).toBeDisabled();
    await user.click(age);
    expect(create).toBeEnabled();
    // Unticking any one disables it again.
    await user.click(privacy);
    expect(create).toBeDisabled();
    await user.click(privacy);

    await user.click(create);
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
  });

  it("opens the documents from their links, without ticking the box", async () => {
    const user = userEvent.setup();
    await fillFields(user);

    await user.click(screen.getByRole("button", { name: "Terms of Service" }));
    const terms = screen.getByRole("dialog", { name: /terms of service/i });
    expect(within(terms).getByText(/1\. Eligibility — Adults Only/)).toBeInTheDocument();
    await user.click(within(terms).getByRole("button", { name: /^done$/i }));
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(screen.getByRole("checkbox", { name: /terms of service/i })).not.toBeChecked();

    await user.click(screen.getByRole("button", { name: "Privacy Policy" }));
    const privacy = screen.getByRole("dialog", { name: /privacy policy/i });
    expect(within(privacy).getByText(/1\. Our Privacy Approach/)).toBeInTheDocument();
    await user.keyboard("{Escape}");
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(screen.getByRole("checkbox", { name: /privacy policy/i })).not.toBeChecked();
  });
});

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
    expect(LEGAL_DOCUMENTS.privacy).toContain("Effective Date: 6 October 2026");
  });
});
