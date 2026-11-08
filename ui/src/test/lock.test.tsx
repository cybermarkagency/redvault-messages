// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { act, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App, { IDLE_LOCK_MS } from "../App.js";
import { resetFakeBackend } from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

// visibilitychange/timeouts fire outside React's synthetic event system, so
// the resulting dispatch() needs an explicit act() to flush before assertions.
function setHidden(hidden: boolean) {
  act(() => {
    Object.defineProperty(document, "hidden", { configurable: true, get: () => hidden });
    document.dispatchEvent(new Event("visibilitychange"));
  });
}

function advanceTimers(ms: number) {
  act(() => {
    vi.advanceTimersByTime(ms);
  });
}

function dispatchActivity() {
  act(() => {
    window.dispatchEvent(new Event("pointerdown"));
  });
}

// Registration is real (real crypto + the fake network backend), so this
// must run to completion — with real timers — before any test engages fake
// timers for the idle-lock assertions below.
async function continueOnboarding() {
  fireEvent.change(screen.getByPlaceholderText(/choose a password/i), {
    target: { value: "correct horse battery staple" },
  });
  fireEvent.change(screen.getByPlaceholderText(/confirm password/i), {
    target: { value: "correct horse battery staple" },
  });
  await tickSignUpBoxes();
  fireEvent.click(screen.getByRole("button", { name: /^create account$/i }));
  await acknowledgeRecoveryKey();
  await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
}

function openSettings() {
  fireEvent.click(screen.getByRole("button", { name: /^settings$/i }));
}

function isLocked(): boolean {
  return screen.queryByText(/redvaultsystem is locked/i) !== null;
}

beforeEach(() => {
  resetFakeBackend();
  localStorage.clear();
});

afterEach(() => {
  Object.defineProperty(document, "hidden", { configurable: true, get: () => false });
});

describe("backgrounding lock (App lock vs. Blur in app switcher independence)", () => {
  it("locks on backgrounding when App lock is on, even with Blur in app switcher off", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    fireEvent.click(screen.getByRole("switch", { name: /blur in app switcher/i })); // turn off
    expect(screen.getByRole("switch", { name: /^app lock$/i })).toHaveAttribute("aria-checked", "true");

    setHidden(true);
    expect(isLocked()).toBe(true);
  });

  it("does not lock on backgrounding when App lock is off", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    fireEvent.click(screen.getByRole("switch", { name: /^app lock$/i })); // turn off

    setHidden(true);
    expect(isLocked()).toBe(false);
  });

  it("blurs (without requiring unlock) on backgrounding when only Blur in app switcher is on", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    fireEvent.click(screen.getByRole("switch", { name: /^app lock$/i })); // turn off, leave blur on

    setHidden(true);
    expect(isLocked()).toBe(false);
    expect(document.querySelector('[class*="blurred"]')).not.toBeNull();

    setHidden(false);
    expect(document.querySelector('[class*="blurred"]')).toBeNull();
  });

  it("does nothing on backgrounding when both settings are off", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    fireEvent.click(screen.getByRole("switch", { name: /^app lock$/i }));
    fireEvent.click(screen.getByRole("switch", { name: /blur in app switcher/i }));

    setHidden(true);
    expect(isLocked()).toBe(false);
    expect(document.querySelector('[class*="blurred"]')).toBeNull();
  });
});

describe("idle timeout lock", () => {
  beforeEach(() => {
    // shouldAdvanceTime keeps real async work (registration's crypto +
    // fake-network promise chain, and RTL's waitFor polling) progressing in
    // real wall-clock time, while still letting advanceTimers() below jump
    // the idle timer forward deterministically.
    vi.useFakeTimers({ shouldAdvanceTime: true });
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it("locks after IDLE_LOCK_MS of no activity when App lock is on", async () => {
    render(<App />);
    await continueOnboarding();
    expect(isLocked()).toBe(false);

    advanceTimers(IDLE_LOCK_MS + 1);
    expect(isLocked()).toBe(true);
  });

  it("does not lock on timeout when App lock is off", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    fireEvent.click(screen.getByRole("switch", { name: /^app lock$/i }));

    advanceTimers(IDLE_LOCK_MS + 1);
    expect(isLocked()).toBe(false);
  });

  it("activity resets the idle timer", async () => {
    render(<App />);
    await continueOnboarding();

    advanceTimers(IDLE_LOCK_MS - 1000);
    dispatchActivity();
    advanceTimers(IDLE_LOCK_MS - 1000);
    expect(isLocked()).toBe(false); // reset kept it from reaching the threshold

    advanceTimers(2000);
    expect(isLocked()).toBe(true);
  });
});

describe("unlock resumes exactly where it left off", () => {
  it("returns to the settings screen after unlocking from there", async () => {
    render(<App />);
    await continueOnboarding();
    openSettings();
    expect(screen.getByRole("switch", { name: /^app lock$/i })).toBeInTheDocument();

    setHidden(true);
    expect(isLocked()).toBe(true);

    fireEvent.click(screen.getByRole("button", { name: /^unlock$/i }));
    expect(isLocked()).toBe(false);
    expect(screen.getByRole("switch", { name: /^app lock$/i })).toBeInTheDocument();
  });
});
