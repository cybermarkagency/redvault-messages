// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The RedVault Messages Terms of Service and Privacy Policy, from legal/ at
// the repository root (the desktop and Android apps read the same files).
// Bundled into the app, so reading them never makes a network request.
import privacyPolicy from "../../../legal/privacy-policy.md?raw";
import termsOfService from "../../../legal/terms-of-service.md?raw";

export type LegalDocumentId = "terms" | "privacy";

export const LEGAL_DOCUMENTS: Record<LegalDocumentId, string> = {
  terms: termsOfService,
  privacy: privacyPolicy,
};

export { parseLegalDocument, type LegalBlock } from "./legalParse.js";
