// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { resetFakeBackend } from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

vi.mock("../lib/api.js", async () => (await import("./fakeApiBackend.js")).fakeApiModule);

describe("Delete account", () => {
  beforeEach(() => {
    resetFakeBackend();
    localStorage.clear();
  });

  it("needs the password and an explicit confirmation, then signs out", async () => {
    const user = userEvent.setup();
    render(<App />);
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await user.click(await screen.findByRole("button", { name: /^delete account$/i }));
    const submit = await screen.findByRole("button", { name: /delete my account/i });

    await user.type(screen.getByPlaceholderText(/^password$/i), "wrong password");
    expect(submit).toBeDisabled();
    await user.click(screen.getByRole("checkbox"));
    await user.click(submit);
    expect(await screen.findByText(/wrong password/i)).toBeInTheDocument();

    await user.clear(screen.getByPlaceholderText(/^password$/i));
    await user.type(screen.getByPlaceholderText(/^password$/i), "correct horse battery staple");
    await user.click(submit);
    await waitFor(() => expect(screen.queryByRole("button", { name: /^settings$/i })).not.toBeInTheDocument());
  }, 30_000);
});
