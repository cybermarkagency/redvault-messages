// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Small helpers shared by the community pages (memberships, buying one, and
// the admin panel's Communities tab).
import { ApiError } from "./api.js";

export function describeError(err: unknown, fallback = "Something went wrong. Try again."): string {
  return err instanceof ApiError ? err.message || fallback : fallback;
}

export function formatDate(iso: string | null): string {
  return iso ? new Date(iso).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) : "—";
}

/** "$4.99" or "4.99" as cents; null when it isn't an amount. */
export function parseDollars(raw: string): number | null {
  const trimmed = raw.trim().replace(/^\$/, "");
  if (!/^\d{1,6}(\.\d{1,2})?$/.test(trimmed)) return null;
  const [whole, fraction = ""] = trimmed.split(".");
  return Number(whole) * 100 + Number(fraction.padEnd(2, "0"));
}
