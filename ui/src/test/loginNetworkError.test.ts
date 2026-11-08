// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { describe, expect, it, vi } from "vitest";
import { TOR_UNAVAILABLE_MESSAGE } from "../lib/torRouting.js";
import { loginIdentity } from "../state/actions.js";

// Sign-in used to say "Wrong handle or password." for everything, including a server that was never reached.
// Over Tor a dropped circuit is routine, so that wording sent people chasing the wrong problem.

vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return {
    ...backend.fakeApiModule,
    login: vi.fn(async (handle: string) => {
      if (handle === "offline") throw new TypeError("NetworkError when attempting to fetch resource.");
      return backend.fakeApiModule.login(handle);
    }),
  };
});

describe("sign-in failures", () => {
  it("says the server couldn't be reached, not that the password is wrong", async () => {
    const dispatch = vi.fn();
    await loginIdentity(dispatch, "offline", "whatever it is");
    const failed = dispatch.mock.calls.map((c) => c[0]).find((a) => a.type === "AUTH_FAILED");
    expect(failed.message).toMatch(/can't reach the server/i);
    expect(failed.message).not.toMatch(/NetworkError/);
    expect(failed.message).not.toMatch(/wrong handle or password/i);
  });

  it("still keeps the wrong-password message for a real refusal", async () => {
    const dispatch = vi.fn();
    await loginIdentity(dispatch, "someone-who-does-not-exist", "whatever it is");
    const failed = dispatch.mock.calls.map((c) => c[0]).find((a) => a.type === "AUTH_FAILED");
    expect(failed.message).toBe("Wrong handle or password.");
  });

  it("the Tor wording is the one every platform uses", () => {
    expect(TOR_UNAVAILABLE_MESSAGE).toBe(
      "Tor connection unavailable. Your private messages and media will remain queued until the secure connection is restored."
    );
  });
});
