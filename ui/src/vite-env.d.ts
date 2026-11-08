// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/// <reference types="vite/client" />

declare module "*.module.css" {
  const classes: Record<string, string>;
  export default classes;
}

/** package.json's version, defined by vite.config.ts. */
declare const __APP_VERSION__: string;

interface ImportMetaEnv {
  /** https base URL of the published source code (AGPL section 13); unset until it's published. */
  readonly VITE_SOURCE_CODE_URL?: string;
  /** Where to report problems getting the source: an email address or https URL. */
  readonly VITE_SOURCE_CONTACT?: string;
}
