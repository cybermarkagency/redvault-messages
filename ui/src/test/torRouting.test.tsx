// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { TorGate } from "../components/TorGate.js";
import { isNetworkFailure } from "../lib/torRouting.js";

// Tor routing on the website (docs/tor-routing-plan.md): the Tor Browser gate on
// the clearnet address.

describe("network failures", () => {
  it("tells a dropped connection from a refusal", () => {
    expect(isNetworkFailure(new TypeError("NetworkError when attempting to fetch resource."))).toBe(true);
    expect(isNetworkFailure(new Error("blocked"))).toBe(false);
  });
});

describe("the Tor Browser gate on the clearnet address", () => {
  afterEach(() => vi.unstubAllGlobals());

  function routing(mode: string) {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () =>
        new Response(JSON.stringify({ mode, onionAddress: "a".repeat(52) + "aaad.onion", cutoffAt: "2027-01-01T00:00:00Z", viaOnion: false }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        })
      )
    );
  }

  it("enforce: the app doesn't run here; it says where to go instead", async () => {
    routing("enforce");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(await screen.findByText("Open Redvault in Tor Browser")).toBeInTheDocument();
    expect(screen.queryByText("the app")).not.toBeInTheDocument();
    expect(screen.getByText(/aaad\.onion/)).toBeInTheDocument();
  });

  it("notice: the app runs, with the move announced", async () => {
    routing("notice");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(await screen.findByText(/Redvault is moving to Tor/)).toBeInTheDocument();
    expect(screen.getByText("the app")).toBeInTheDocument();
  });

  it("off: nothing changes", async () => {
    routing("off");
    render(
      <TorGate>
        <p>the app</p>
      </TorGate>
    );
    expect(screen.getByText("the app")).toBeInTheDocument();
    await new Promise((r) => setTimeout(r, 20));
    expect(screen.queryByText(/Tor/)).not.toBeInTheDocument();
  });
});
