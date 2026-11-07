import { render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { WebAssemblyGate } from "../components/WebAssemblyGate.js";

// Tor Browser's Safer and Safest levels remove WebAssembly from non-HTTPS pages, which includes onion
// addresses (checked in Tor Browser 15.0.25: `WebAssembly` is not defined there). The app's encryption
// needs it, so the app must say so instead of failing at sign-in with a wrong-password message.

describe("WebAssemblyGate", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("runs the app where WebAssembly exists", () => {
    render(
      <WebAssemblyGate>
        <p>the app</p>
      </WebAssemblyGate>
    );
    expect(screen.getByText("the app")).toBeInTheDocument();
  });

  it("explains the problem, and the fix, where it doesn't", () => {
    vi.stubGlobal("WebAssembly", undefined);
    render(
      <WebAssemblyGate>
        <p>the app</p>
      </WebAssemblyGate>
    );
    expect(screen.getByText("This browser can't run Redvault's encryption")).toBeInTheDocument();
    expect(screen.queryByText("the app")).not.toBeInTheDocument();
    expect(screen.getByText(/Nothing was sent anywhere/)).toBeInTheDocument();
  });

  it("tells a Tor Browser user which setting to change", () => {
    vi.stubGlobal("WebAssembly", undefined);
    const original = window.location;
    Object.defineProperty(window, "location", { value: { ...original, hostname: "abcdefghijklmnop.onion" }, configurable: true });
    try {
      render(
        <WebAssemblyGate>
          <p>the app</p>
        </WebAssemblyGate>
      );
      expect(screen.getByText(/Safer/)).toBeInTheDocument();
      expect(screen.getByText(/Standard/)).toBeInTheDocument();
    } finally {
      Object.defineProperty(window, "location", { value: original, configurable: true });
    }
  });
});
