// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The AGPL-3.0-only source offer (AGPL section 13): where people using the web
// app over the network can get its Corresponding Source, and whom to tell if
// they can't. Both come from build configuration, never from code, so no
// address is ever made up:
//
//   VITE_SOURCE_CODE_URL  https base URL of the published source (empty or
//                         unset: not published yet, and the app says so)
//   VITE_SOURCE_CONTACT   where to report problems getting the source: an
//                         email address or an https URL (empty or unset: the
//                         app points to the contact in the Terms of Service)

/** The web app's version, from package.json (vite.config.ts defines it). */
export const APP_VERSION: string = typeof __APP_VERSION__ === "string" ? __APP_VERSION__ : "unknown";

export type SourceContact = { kind: "email"; address: string } | { kind: "url"; url: string } | { kind: "text"; text: string };

export interface SourceOffer {
  /** The https source URL, or null when it isn't published (or the configured value isn't a valid https URL). */
  sourceUrl: string | null;
  /** Where to report problems, or null to fall back to the Terms of Service contact. */
  contact: SourceContact | null;
  version: string;
}

export interface SourceOfferEnv {
  VITE_SOURCE_CODE_URL?: string;
  VITE_SOURCE_CONTACT?: string;
}

/** An absolute https URL with a host and no embedded credentials, or null. */
export function parseHttpsUrl(raw: string | undefined | null): string | null {
  const value = raw?.trim();
  if (!value) return null;
  let url: URL;
  try {
    url = new URL(value);
  } catch {
    return null;
  }
  if (url.protocol !== "https:" || !url.hostname || url.username || url.password) return null;
  return url.href;
}

const EMAIL = /^[^\s@<>()"]+@[^\s@<>()"]+\.[^\s@<>()"]+$/;

export function parseSourceContact(raw: string | undefined | null): SourceContact | null {
  const value = raw?.trim();
  if (!value) return null;
  const mail = value.replace(/^mailto:/i, "");
  if (EMAIL.test(mail)) return { kind: "email", address: mail };
  const url = parseHttpsUrl(value);
  if (url) return { kind: "url", url };
  // Anything else (an http:// or other-scheme link included) is shown as plain text, never as a link.
  return { kind: "text", text: value };
}

export function resolveSourceOffer(env: SourceOfferEnv, version: string = APP_VERSION): SourceOffer {
  return {
    sourceUrl: parseHttpsUrl(env.VITE_SOURCE_CODE_URL),
    contact: parseSourceContact(env.VITE_SOURCE_CONTACT),
    version,
  };
}

/** What this build was configured with. */
export const SOURCE_OFFER: SourceOffer = resolveSourceOffer(import.meta.env);

/** A file under public/legal/, at the app's base path (e.g. /redvaultmessages/legal/LICENSE.txt). */
export function legalFileUrl(file: "LICENSE.txt" | "THIRD_PARTY_NOTICES.txt", base: string = import.meta.env.BASE_URL): string {
  return `${base.replace(/\/?$/, "/")}legal/${file}`;
}
