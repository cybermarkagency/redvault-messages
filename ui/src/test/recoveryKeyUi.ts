// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { fireEvent, screen } from "@testing-library/react";

/** Sign-up now shows the new recovery key once; tick "saved" and continue, as a user would. */
export async function acknowledgeRecoveryKey(): Promise<void> {
  fireEvent.click(await screen.findByRole("checkbox", { name: /saved my recovery key/i }));
  fireEvent.click(screen.getByRole("button", { name: /^done$/i }));
}
