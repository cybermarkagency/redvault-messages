import { useEffect, useState } from "react";
import { saveBlobAs } from "../lib/fileTransfer.js";
import { downloadDmFile } from "../state/actions.js";
import type { FileAttachmentView, Message } from "../types.js";
import styles from "./SharedMedia.module.css";

/** Decrypted thumbnails, kept for this tab only (never stored), so scrolling back doesn't re-download. */
const thumbnails = new Map<string, Promise<string | null>>();

function thumbnailFor(file: FileAttachmentView): Promise<string | null> {
  const id = file.descriptor.objectId;
  let pending = thumbnails.get(id);
  if (!pending) {
    pending = downloadDmFile(file.descriptor, () => {})
      .then((blob) => URL.createObjectURL(new Blob([blob], { type: file.descriptor.mimeType })))
      .catch(() => null);
    thumbnails.set(id, pending);
  }
  return pending;
}

/** Files sent in a chat, newest first: what the shared-media grid shows. */
export function sharedFiles(messages: Message[]): FileAttachmentView[] {
  return messages
    .filter((m) => m.file && m.file.status === "available")
    .map((m) => m.file!)
    .reverse();
}

function kindLabel(mimeType: string): string {
  if (mimeType.startsWith("image/")) return "photo";
  if (mimeType.startsWith("video/")) return "video";
  if (mimeType.startsWith("audio/")) return "audio";
  return "file";
}

function Tile({ file }: { file: FileAttachmentView }) {
  const isImage = file.descriptor.mimeType.startsWith("image/") && file.descriptor.originalSize <= 15 * 1024 * 1024;
  const [url, setUrl] = useState<string | null>(null);
  useEffect(() => {
    if (!isImage) return;
    let cancelled = false;
    thumbnailFor(file).then((u) => {
      if (!cancelled) setUrl(u);
    });
    return () => {
      cancelled = true;
    };
  }, [file, isImage]);

  async function open() {
    try {
      const blob = await downloadDmFile(file.descriptor, () => {});
      saveBlobAs(blob, file.descriptor.filename);
    } catch {
      // The chat's own file bubble shows download errors; the grid stays quiet.
    }
  }

  return (
    <button type="button" className={styles.tile} onClick={open} title={file.descriptor.filename} aria-label={`Save ${file.descriptor.filename}`}>
      {url ? <img src={url} alt="" className={styles.image} /> : <span className={`${styles.placeholder} type-mono`}>[{kindLabel(file.descriptor.mimeType)}]</span>}
    </button>
  );
}

/**
 * The shared-media grid. Images are downloaded and decrypted in this tab
 * to show them (the server only ever has ciphertext); anything else is a
 * labelled tile. Clicking saves the decrypted file.
 */
export function SharedMedia({ files, columns = 3, limit }: { files: FileAttachmentView[]; columns?: number; limit?: number }) {
  const shown = limit ? files.slice(0, limit) : files;
  if (files.length === 0) return <p className={`${styles.empty} type-secondary`}>No photos or files yet.</p>;
  return (
    <div className={styles.grid} style={{ gridTemplateColumns: `repeat(${columns}, 1fr)` }}>
      {shown.map((f) => (
        <Tile key={f.descriptor.objectId} file={f} />
      ))}
    </div>
  );
}
