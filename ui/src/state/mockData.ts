// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { AppSettings } from "../types.js";

export const initialSettings: AppSettings = {
  readReceipts: false,
  typingIndicators: false,
  defaultDisappearing: "off",
  appLock: true,
  switcherBlur: true,
  acceptMessageRequests: true,
  transparencyMode: false,
  showOnlineStatus: false,
};
