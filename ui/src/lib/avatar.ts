// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Client-side prep for a profile picture upload. The image itself is real
// and shared — visible to every user, stored in Backblaze B2 (see
// server/src/storage/b2AvatarStorage.ts) — this module only handles
// getting a File down to a small, uploadable JPEG before it's sent to
// POST /api/users/me/avatar.
const MAX_DIMENSION = 128;
const MAX_FILE_BYTES = 10 * 1024 * 1024; // 10MB — generous, just a sanity cap on the *source* file

/** Crops to a centered square and downscales, so the upload stays a few KB. Returns plain base64 (no data: URL prefix) — what the upload endpoint expects. */
export function readImageAsAvatar(file: File): Promise<string> {
  if (!file.type.startsWith("image/")) {
    return Promise.reject(new Error("That's not an image file."));
  }
  if (file.size > MAX_FILE_BYTES) {
    return Promise.reject(new Error("That image is too large (max 10MB)."));
  }

  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(new Error("Couldn't read that file."));
    reader.onload = () => {
      const img = new Image();
      img.onerror = () => reject(new Error("Couldn't load that as an image."));
      img.onload = () => {
        const side = Math.min(img.width, img.height);
        const sx = (img.width - side) / 2;
        const sy = (img.height - side) / 2;
        const canvas = document.createElement("canvas");
        canvas.width = MAX_DIMENSION;
        canvas.height = MAX_DIMENSION;
        const ctx = canvas.getContext("2d");
        if (!ctx) {
          reject(new Error("Couldn't process that image."));
          return;
        }
        ctx.drawImage(img, sx, sy, side, side, 0, 0, MAX_DIMENSION, MAX_DIMENSION);
        const dataUrl = canvas.toDataURL("image/jpeg", 0.85);
        resolve(dataUrl.slice(dataUrl.indexOf(",") + 1)); // strip "data:image/jpeg;base64,"
      };
      img.src = reader.result as string;
    };
    reader.readAsDataURL(file);
  });
}
