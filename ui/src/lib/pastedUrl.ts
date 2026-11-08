// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

/**
 * The address in something someone pasted: a bare link, or a whole message
 * with a link in it. Relative addresses (/?join=…) count too. Null when
 * there's nothing that parses as an address.
 */
export function pastedUrl(text: string): URL | null {
  const candidate = text.match(/https?:\/\/\S+/)?.[0] ?? text.trim();
  try {
    return new URL(candidate, window.location.origin);
  } catch {
    return null;
  }
}
