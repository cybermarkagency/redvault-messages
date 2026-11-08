// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { SharePage } from "../share/SharePage.js";

// The share page's states that don't need a server (the server round trips
// are in shareEndToEnd.test.ts): a broken link, and what each server answer
// is shown as.

const okLink = { search: "?id=AAAAAAAAAAAAAAAAAAAAAA", hash: `#${"A".repeat(43)}` };

afterEach(() => vi.unstubAllGlobals());

function answer(status: number, body: unknown) {
  vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } })));
}

describe("share page", () => {
  it("explains a link with its secret missing, without calling the server", () => {
    const fetchSpy = vi.fn();
    vi.stubGlobal("fetch", fetchSpy);
    render(<SharePage location={{ search: okLink.search, hash: "" }} />);
    expect(screen.getByRole("heading", { name: "This link is incomplete" })).toBeInTheDocument();
    expect(fetchSpy).not.toHaveBeenCalled();
  });

  it.each([
    [410, "share_revoked", "This link was turned off"],
    [410, "share_expired", "This link has expired"],
    [410, "share_unavailable", "No longer available"],
    [404, "not_found", "Link not found"],
  ])("shows %s %s clearly", async (status, error, title) => {
    answer(status, { error, message: "x" });
    render(<SharePage location={okLink} />);
    expect(await screen.findByRole("heading", { name: title })).toBeInTheDocument();
  });

  it("asks for a password when the link has one, and never puts the secret in a request URL", async () => {
    const calls: { url: string; headers: Headers }[] = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init: RequestInit) => {
        calls.push({ url, headers: new Headers(init.headers) });
        return new Response(JSON.stringify({ kind: "folder", password: { salt: btoa("0123456789abcdef"), ops: 2, mem: 67108864 } }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        });
      })
    );
    render(<SharePage location={okLink} />);
    expect(await screen.findByRole("heading", { name: "This link needs a password" })).toBeInTheDocument();
    expect(calls).toHaveLength(1);
    expect(calls[0].url).toMatch(/\/public\/shares\/AAAAAAAAAAAAAAAAAAAAAA$/);
    expect(calls[0].url).not.toContain("A".repeat(43));
    // A derived token goes in a header, never the secret itself.
    expect(calls[0].headers.get("X-Share-Lookup")).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(calls[0].headers.get("X-Share-Lookup")).not.toBe("A".repeat(43));
  });
});
