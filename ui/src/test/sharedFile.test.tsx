// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import sodium from "libsodium-wrappers-sumo";
import { afterEach, beforeAll, describe, expect, it, vi } from "vitest";
import { encryptFile, FileDecryptError } from "../lib/fileCrypto.js";
import { encryptWithConversationKey, ready, toBase64 } from "../lib/e2eCrypto.js";
import { downloadSharedFile, openSharedFileMeta, sharedLinkKey, type SharedFileLinkInfo } from "../lib/sharedFileLink.js";
import { SharedFile } from "../screens/SharedFile.js";

// jsdom's Blob has no text(); browsers all do.
beforeAll(() => {
  if (!Blob.prototype.text) {
    Blob.prototype.text = function text(this: Blob) {
      return new Promise<string>((resolve) => {
        const reader = new FileReader();
        reader.onload = () => resolve(reader.result as string);
        reader.readAsText(this);
      });
    };
  }
});

afterEach(() => {
  vi.unstubAllGlobals();
  sessionStorage.clear();
  window.history.replaceState(null, "", "/");
});

function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.readAsArrayBuffer(blob);
  });
}

/** A link the way the desktop app makes one (DriveRepository::createLink). */
async function makeLink(options: { password?: string; allowDownload: boolean; text: string }) {
  await ready();
  const encrypted = await encryptFile(new Blob([options.text]));
  const ciphertext = await readBlob(encrypted.blob);
  const secret = sodium.randombytes_buf(32);
  const salt = options.password ? sodium.randombytes_buf(sodium.crypto_pwhash_SALTBYTES) : null;
  const key = await sharedLinkKey(secret, options.password ?? "", salt ? toBase64(salt) : null);
  let json = JSON.stringify({
    v: 1,
    name: "notes.txt",
    mime: "text/plain",
    size: encrypted.plaintextSize,
    key: toBase64(encrypted.key),
    header: toBase64(encrypted.header),
    chunk: encrypted.chunkSize,
    sha: encrypted.ciphertextSha256,
  });
  json += " ".repeat((Math.floor(json.length / 128) + 1) * 128 - json.length);
  const sealed = encryptWithConversationKey(json, key);
  const info: SharedFileLinkInfo = {
    id: "link-1",
    sealedMeta: toBase64(sealed.ciphertext),
    metaNonce: toBase64(sealed.nonce),
    passwordSalt: salt ? toBase64(salt) : null,
    allowDownload: options.allowDownload,
    expiresAt: "2026-10-06T00:00:00.000Z",
    encryptedSize: encrypted.encryptedSize,
    ciphertextSha256: encrypted.ciphertextSha256,
  };
  const fragment = sodium.to_base64(secret, sodium.base64_variants.URLSAFE_NO_PADDING);
  return { info, ciphertext, secret, fragment };
}

function serve(info: SharedFileLinkInfo, ciphertext: Uint8Array) {
  const fetchMock = vi.fn(async (url: string) => {
    if (url.endsWith("/public/backup-links/link-1")) return new Response(JSON.stringify(info), { status: 200 });
    if (url.endsWith("/public/backup-links/link-1/content")) return new Response(ciphertext as BodyInit, { status: 200 });
    return new Response(JSON.stringify({ error: "not_found" }), { status: 404 });
  });
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

describe("shared file links", () => {
  it("asks for the password, refuses a wrong one, then previews without offering a download", async () => {
    const { info, ciphertext, fragment } = await makeLink({ password: "hunter22", allowDownload: false, text: "Quarterly notes" });
    const fetchMock = serve(info, ciphertext);
    window.history.replaceState(null, "", `/?file=link-1#${fragment}`);
    const user = userEvent.setup();
    render(<SharedFile linkId="link-1" />);

    await screen.findByText("This file needs a password");
    // The secret is out of the address bar straight away.
    expect(window.location.hash).toBe("");
    // Nothing but the link id goes to the server, and no cookies.
    for (const [url, init] of fetchMock.mock.calls as unknown as [string, RequestInit][]) {
      expect(url).not.toContain(fragment);
      expect(init.credentials).toBe("omit");
    }

    await user.type(screen.getByLabelText("Password"), "wrong");
    await user.click(screen.getByRole("button", { name: "Open file" }));
    await screen.findByText("That password isn't right.", {}, { timeout: 5000 });

    await user.clear(screen.getByLabelText("Password"));
    await user.type(screen.getByLabelText("Password"), "hunter22");
    await user.click(screen.getByRole("button", { name: "Open file" }));
    await screen.findByText("notes.txt", {}, { timeout: 5000 });
    expect(screen.queryByRole("button", { name: "Download" })).toBeNull();

    await user.click(screen.getByRole("button", { name: "Preview" }));
    await screen.findByText("Quarterly notes");
  });

  it("opens a link without a password and offers the download when allowed", async () => {
    const { info, ciphertext, fragment } = await makeLink({ allowDownload: true, text: "hello" });
    serve(info, ciphertext);
    window.history.replaceState(null, "", `/?file=link-1#${fragment}`);
    render(<SharedFile linkId="link-1" />);
    await screen.findByText("notes.txt");
    expect(screen.getByRole("button", { name: "Download" })).toBeTruthy();
  });

  it("says so when the secret is missing, or the link is gone", async () => {
    window.history.replaceState(null, "", "/?file=link-3");
    const { unmount } = render(<SharedFile linkId="link-3" />);
    await screen.findByText(/This link is incomplete/);
    unmount();

    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({ error: "link_expired" }), { status: 410 })));
    window.history.replaceState(null, "", "/?file=link-2#" + "A".repeat(43));
    render(<SharedFile linkId="link-2" />);
    await screen.findByText(/This link has expired/);
  });

  it("won't open a file the server swapped or damaged", async () => {
    const { info, ciphertext, secret } = await makeLink({ allowDownload: true, text: "original" });
    const meta = openSharedFileMeta(info, secret)!;
    expect(meta.name).toBe("notes.txt");

    // A different ciphertext (with its own hash) from the server.
    const other = await makeLink({ allowDownload: true, text: "swapped" });
    serve({ ...info, ciphertextSha256: other.info.ciphertextSha256, encryptedSize: other.info.encryptedSize }, other.ciphertext);
    await expect(downloadSharedFile({ ...info, ciphertextSha256: other.info.ciphertextSha256 }, meta)).rejects.toBeInstanceOf(FileDecryptError);

    // The right hash, but flipped bytes.
    const damaged = ciphertext.slice();
    damaged[10] ^= 1;
    serve(info, damaged);
    await expect(downloadSharedFile(info, meta)).rejects.toBeInstanceOf(FileDecryptError);
  });

  it("opens nothing with the wrong key", async () => {
    const { info } = await makeLink({ password: "pw", allowDownload: false, text: "x" });
    expect(openSharedFileMeta(info, sodium.randombytes_buf(32))).toBeNull();
  });
});
