// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { FileIcon, LockIcon } from "../components/icons.js";
import { FileDecryptError } from "../lib/fileCrypto.js";
import {
  downloadSharedFile,
  fetchSharedFileLink,
  openSharedFileMeta,
  secretFromFragment,
  sharedLinkKey,
  SharedFileLinkError,
  type SharedFileLinkInfo,
  type SharedFileMeta,
} from "../lib/sharedFileLink.js";
import styles from "./SharedFile.module.css";

const SECRET_KEY = "redvaultsystem:shared-file:";

/** Types the browser shows safely inline (no SVG or HTML: those can run script). */
const PREVIEW_IMAGE = new Set(["image/png", "image/jpeg", "image/gif", "image/webp", "image/avif", "image/bmp"]);
const PREVIEW_VIDEO = new Set(["video/mp4", "video/webm", "video/ogg"]);
const PREVIEW_AUDIO = new Set(["audio/mpeg", "audio/mp4", "audio/ogg", "audio/wav", "audio/webm", "audio/flac"]);
const PREVIEW_TEXT_MAX = 512 * 1024;

function previewKind(mime: string): "image" | "video" | "audio" | "text" | null {
  if (PREVIEW_IMAGE.has(mime)) return "image";
  if (PREVIEW_VIDEO.has(mime)) return "video";
  if (PREVIEW_AUDIO.has(mime)) return "audio";
  if (mime === "text/plain" || mime === "text/csv" || mime === "text/markdown" || mime === "application/json") return "text";
  return null;
}

function formatBytes(bytes: number): string {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${value} B` : `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}

/**
 * The secret travels in the fragment. Like call links, it's taken out of the
 * address bar and history right away and kept only for this tab, so a
 * reload still works but the secret doesn't linger in history.
 */
const taken = new Map<string, string>();

function takeSecret(linkId: string): Uint8Array | null {
  const fragment = window.location.hash.replace(/^#/, "") || taken.get(linkId) || "";
  if (fragment) {
    taken.set(linkId, fragment);
    try {
      sessionStorage.setItem(SECRET_KEY + linkId, fragment);
    } catch {
      // Without session storage a reload just needs the whole link again.
    }
    window.history.replaceState(null, "", `${window.location.pathname}${window.location.search}`);
    return secretFromFragment(fragment);
  }
  try {
    const kept = sessionStorage.getItem(SECRET_KEY + linkId);
    return kept ? secretFromFragment(kept) : null;
  } catch {
    return null;
  }
}

type Phase =
  | { kind: "loading" }
  | { kind: "error"; message: string }
  | { kind: "password"; info: SharedFileLinkInfo; secret: Uint8Array; wrong: boolean; busy: boolean }
  | { kind: "ready"; info: SharedFileLinkInfo; meta: SharedFileMeta };

/** Someone opened a share link to a file in Files: no sign-in, decrypted in this browser. */
export function SharedFile({ linkId }: { linkId: string }) {
  const [phase, setPhase] = useState<Phase>({ kind: "loading" });
  const [password, setPassword] = useState("");

  useEffect(() => {
    let cancelled = false;
    (async () => {
      const secret = takeSecret(linkId);
      if (!secret) {
        setPhase({ kind: "error", message: "This link is incomplete. Ask for it again, and open the whole link." });
        return;
      }
      try {
        const info = await fetchSharedFileLink(linkId);
        if (cancelled) return;
        if (info.passwordSalt) {
          setPhase({ kind: "password", info, secret, wrong: false, busy: false });
          return;
        }
        const meta = openSharedFileMeta(info, await sharedLinkKey(secret, "", null));
        if (cancelled) return;
        setPhase(meta ? { kind: "ready", info, meta } : { kind: "error", message: "This link is damaged. Ask for it again." });
      } catch (err) {
        if (!cancelled) setPhase({ kind: "error", message: err instanceof SharedFileLinkError ? err.message : "Couldn't open the link. Try again." });
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [linkId]);

  async function unlock(event: React.FormEvent) {
    event.preventDefault();
    if (phase.kind !== "password" || phase.busy || password.length === 0) return;
    const { info, secret } = phase;
    setPhase({ ...phase, busy: true, wrong: false });
    // Let the "Unlocking…" state paint before Argon2id blocks the thread.
    await new Promise((resolve) => setTimeout(resolve, 30));
    const key = await sharedLinkKey(secret, password, info.passwordSalt);
    const meta = openSharedFileMeta(info, key);
    if (meta) {
      setPassword("");
      setPhase({ kind: "ready", info, meta });
    } else {
      setPhase({ kind: "password", info, secret, wrong: true, busy: false });
    }
  }

  return (
    <div className={styles.screen}>
      <div className={styles.card}>
        {phase.kind === "loading" && <p className={styles.muted}>Opening the link…</p>}
        {phase.kind === "error" && (
          <p className={styles.error} role="alert">
            {phase.message}
          </p>
        )}
        {phase.kind === "password" && (
          <form className={styles.form} onSubmit={unlock}>
            <span className={styles.glyph} aria-hidden>
              <LockIcon />
            </span>
            <h1 className={`${styles.title} type-display`}>This file needs a password</h1>
            <p className={styles.muted}>Its owner set one. Ask them for it: it isn't in the link.</p>
            <input
              className={styles.input}
              type="password"
              autoComplete="off"
              autoFocus
              placeholder="Password"
              aria-label="Password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
            />
            {phase.wrong && (
              <p className={styles.error} role="alert">
                That password isn't right.
              </p>
            )}
            <button type="submit" className={styles.primary} disabled={phase.busy || password.length === 0}>
              {phase.busy ? "Unlocking…" : "Open file"}
            </button>
          </form>
        )}
        {phase.kind === "ready" && <OpenedFile info={phase.info} meta={phase.meta} />}
      </div>
    </div>
  );
}

function OpenedFile({ info, meta }: { info: SharedFileLinkInfo; meta: SharedFileMeta }) {
  const kind = previewKind(meta.mime);
  const [blobUrl, setBlobUrl] = useState<string | null>(null);
  const [text, setText] = useState<string | null>(null);
  const [progress, setProgress] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const urlRef = useRef<string | null>(null);

  useEffect(
    () => () => {
      if (urlRef.current) URL.revokeObjectURL(urlRef.current);
    },
    []
  );

  async function fetchFile(): Promise<Blob | null> {
    setError(null);
    setProgress(0);
    try {
      const blob = await downloadSharedFile(info, meta, setProgress);
      setProgress(null);
      return blob;
    } catch (err) {
      setProgress(null);
      setError(
        err instanceof SharedFileLinkError
          ? err.message
          : err instanceof FileDecryptError
            ? "This file didn't pass its integrity check, so it wasn't opened."
            : "Couldn't get the file. Try again."
      );
      return null;
    }
  }

  async function preview() {
    const blob = await fetchFile();
    if (!blob) return;
    if (kind === "text") {
      setText(await blob.text());
      return;
    }
    const url = URL.createObjectURL(blob);
    urlRef.current = url;
    setBlobUrl(url);
  }

  async function download() {
    let url = urlRef.current;
    if (!url) {
      const blob = await fetchFile();
      if (!blob) return;
      url = URL.createObjectURL(blob);
      urlRef.current = url;
    }
    const a = document.createElement("a");
    a.href = url;
    a.download = meta.name;
    a.rel = "noopener";
    document.body.appendChild(a);
    a.click();
    a.remove();
  }

  const canPreview = kind !== null && (kind !== "text" || meta.size <= PREVIEW_TEXT_MAX);
  const expires = info.expiresAt ? new Date(info.expiresAt) : null;

  return (
    <div className={styles.opened}>
      <span className={styles.glyph} aria-hidden>
        <FileIcon />
      </span>
      <h1 className={`${styles.name} type-display`}>{meta.name}</h1>
      <p className={styles.muted}>
        {formatBytes(meta.size)}
        {expires && ` · Link expires ${expires.toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" })}`}
      </p>
      <p className={styles.note}>
        <LockIcon width={16} height={16} />
        <span>Encrypted end to end. It's decrypted here in your browser; the server only ever had the scrambled copy.</span>
      </p>

      {blobUrl && kind === "image" && <img className={styles.media} src={blobUrl} alt={meta.name} />}
      {blobUrl && kind === "video" && <video className={styles.media} src={blobUrl} controls />}
      {blobUrl && kind === "audio" && <audio className={styles.audio} src={blobUrl} controls />}
      {text !== null && <pre className={styles.text}>{text}</pre>}

      {progress !== null && (
        <div className={styles.progress} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(progress * 100)}>
          <span style={{ width: `${Math.round(progress * 100)}%` }} />
        </div>
      )}
      {error && (
        <p className={styles.error} role="alert">
          {error}
        </p>
      )}

      <div className={styles.actions}>
        {canPreview && !blobUrl && text === null && (
          <button type="button" className={info.allowDownload ? styles.secondary : styles.primary} disabled={progress !== null} onClick={preview}>
            Preview
          </button>
        )}
        {info.allowDownload && (
          <button type="button" className={styles.primary} disabled={progress !== null} onClick={download}>
            Download
          </button>
        )}
      </div>
      {!info.allowDownload && !canPreview && (
        <p className={styles.muted}>The owner turned off downloads, and this kind of file can't be previewed in a browser.</p>
      )}
    </div>
  );
}
