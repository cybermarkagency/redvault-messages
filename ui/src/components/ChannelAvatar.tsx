// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { fetchChannelAvatar } from "../lib/api.js";
import { Avatar } from "./Avatar.js";

/** Photos already fetched, by versioned URL (+ access token for a private channel): a new photo has a new URL. */
const cache = new Map<string, Promise<string | null>>();

function photoFor(avatarUrl: string, access: string | null): Promise<string | null> {
  const key = `${avatarUrl}|${access ?? ""}`;
  let photo = cache.get(key);
  if (!photo) {
    photo = fetchChannelAvatar(avatarUrl, access)
      .then((blob) => URL.createObjectURL(blob))
      .catch(() => null);
    cache.set(key, photo);
  }
  return photo;
}

/**
 * A channel's photo, or its initials. Fetched without the session cookie
 * (and with the private link's token when the channel is private), so
 * loading a photo never tells the server which account looked.
 */
export function ChannelAvatar({ name, avatarUrl, access = null, size = 40 }: { name: string; avatarUrl: string | null; access?: string | null; size?: number }) {
  const [src, setSrc] = useState<string | null>(null);
  useEffect(() => {
    let cancelled = false;
    setSrc(null);
    if (avatarUrl) {
      void photoFor(avatarUrl, access).then((url) => {
        if (!cancelled) setSrc(url);
      });
    }
    return () => {
      cancelled = true;
    };
  }, [avatarUrl, access]);
  return <Avatar avatarUrl={src} seed={name} size={size} />;
}
