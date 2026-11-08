// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The Device keys (preview) panel on Linked devices, with the real engine
// (crypto-core's Node build) and an in-memory protocol-3 server. Skipped when
// crypto-core/build-web.sh hasn't been run.
import "fake-indexeddb/auto";
import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import { IDBFactory } from "fake-indexeddb";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeAll, describe, expect, it, vi } from "vitest";
import type { FakeV3Server } from "./fakeV3Api.js";

const holder = vi.hoisted(() => ({ server: null as FakeV3Server | null }));
const USER = "44444444-4444-4444-8444-444444444444";
const legacyKey = crypto.getRandomValues(new Uint8Array(32));

vi.mock("../lib/api.js", async (importActual) => {
  const actual = await importActual<typeof import("../lib/api.js")>();
  const { FakeV3Server } = await import("./fakeV3Api.js");
  holder.server = new FakeV3Server(actual.ApiError);
  return { ...actual, ...holder.server.module() };
});

vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({ state: { identity: { userId: USER, privateKey: legacyKey } }, dispatch: () => {} }),
}));

const { DeviceKeys } = await import("../screens/settings/DeviceKeys.js");
const engine = await import("../lib/e2eV3/engine.js");
const { setUpFirstDevice } = await import("../lib/e2eV3/device.js");
const { DeviceStore, browserDeviceStore } = await import("../lib/e2eV3/deviceStore.js");
const { TrustedPairing } = await import("../lib/e2eV3/pairing.js");
await import("../lib/api.js");

// Tests run from ui/ (vitest's root).
const enginePath = resolve(process.cwd(), "../crypto-core/pkg/node/redvault_e2ee.js");

describe.skipIf(!existsSync(enginePath))("Device keys (preview)", () => {
  beforeAll(() => {
    holder.server!.currentUser = USER;
    engine.useEngineForTests(createRequire(enginePath)(enginePath).call);
  });

  it("sets up this browser as the first trusted device and shows a recovery key once", async () => {
    const user = userEvent.setup();
    render(<DeviceKeys />);
    await user.click(await screen.findByRole("button", { name: "Set up" }));
    expect(await screen.findByText("This browser")).toBeInTheDocument();
    expect(holder.server!.lists.get(USER)?.legacyCertificate).toBeDefined();
    await user.click(screen.getByRole("button", { name: "Create" }));
    expect(await screen.findByText(/^([0-9A-F]{4}-){15}[0-9A-F]{4}$/)).toBeInTheDocument();
    expect(holder.server!.backups.has(USER)).toBe(true);
    // A device that only signed in (a stolen password, say) shows up as not trusted.
    holder.server!.devices.set(`${USER}/stranger`, {
      identity: { userId: USER, deviceId: "55555555-5555-4555-8555-555555555555", curve25519: "x".repeat(43), ed25519: "y".repeat(43) },
      oneTimeKeys: [],
      fallbackKey: null,
    });
    render(<DeviceKeys />);
    await waitFor(() => expect(screen.getAllByText("Signed in, not trusted").length).toBeGreaterThan(0));
  });

  it("pairs this browser by comparing codes with a trusted device", { timeout: 30_000 }, async () => {
    const server = holder.server!;
    server.lists.clear();
    server.devices.clear();
    server.enrollments.clear();
    server.mailbox.length = 0;
    await browserDeviceStore().wipe(USER);
    // The account's trusted device, in another browser.
    const phone = await setUpFirstDevice(USER, legacyKey, new DeviceStore(new IDBFactory()));
    const fast = { intervalMs: 5, timeoutMs: 20_000 };

    const user = userEvent.setup();
    render(<DeviceKeys />);
    await user.click(await screen.findByRole("button", { name: "Start pairing" }));
    await waitFor(() => expect(server.enrollments.size).toBe(1));
    const [enrollmentId] = server.enrollments.keys();
    const answered = await TrustedPairing.begin(phone, enrollmentId, fast);
    // The same numbers on both screens.
    const shown = await screen.findByLabelText("Pairing codes", {}, { timeout: 10_000 });
    expect(shown.textContent!.trim().split(/\s+/).map(Number)).toEqual(answered.codes);
    const approving = answered.pairing.confirm(fast);
    await user.click(screen.getByRole("button", { name: "They match" }));
    await approving;
    expect(await screen.findByText("This browser", {}, { timeout: 15_000 })).toBeInTheDocument();
    expect(server.lists.get(USER)!.devices).toHaveLength(2);
  });
});
