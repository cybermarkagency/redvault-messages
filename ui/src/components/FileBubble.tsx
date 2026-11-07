import { useEffect, useState } from "react";
import { formatBytes } from "../lib/fileMessage.js";
import { saveBlobAs } from "../lib/fileTransfer.js";
import { downloadDmFile, FileActionError, type FileDownloadStage } from "../state/actions.js";
import type { FileAttachmentView } from "../types.js";
import styles from "./FileBubble.module.css";
import { FileIcon } from "./icons.js";

const STAGE_LABEL: Record<FileDownloadStage, string> = {
  downloading: "Downloading encrypted file…",
  decrypting: "Decrypting…",
};

/**
 * One end-to-end encrypted file. The filename shown here was sanitized
 * when the descriptor was parsed (lib/fileMessage.ts) and is rendered as
 * plain React text, never HTML. The decrypted bytes only ever go to a
 * local download — never back to the server, never into storage.
 */
/** Audio types a browser can play; anything else is only ever offered as a download. */
const PLAYABLE_AUDIO = new Set(["audio/mp4", "audio/aac", "audio/mpeg", "audio/ogg", "audio/webm", "audio/wav"]);

export function FileBubble({ file, sent }: { file: FileAttachmentView; sent: boolean }) {
  const [stage, setStage] = useState<FileDownloadStage | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [audioUrl, setAudioUrl] = useState<string | null>(null);
  const { descriptor, status } = file;
  const isVoice = PLAYABLE_AUDIO.has(descriptor.mimeType);

  // The decrypted audio lives only in this tab's memory; release it when the bubble goes away.
  useEffect(() => () => {
    if (audioUrl) URL.revokeObjectURL(audioUrl);
  }, [audioUrl]);

  async function play() {
    if (stage || audioUrl) return;
    setError(null);
    try {
      const blob = await downloadDmFile(descriptor, setStage);
      setAudioUrl(URL.createObjectURL(new Blob([blob], { type: descriptor.mimeType })));
    } catch (err) {
      setError(err instanceof FileActionError ? err.message : "Couldn't play this voice message — try again.");
    } finally {
      setStage(null);
    }
  }

  const unavailable =
    status === "deleted" ? "File deleted" : status === "expired" ? "File expired" : status === "failed" ? "Upload failed" : null;

  async function download() {
    if (stage) return;
    setError(null);
    try {
      const blob = await downloadDmFile(descriptor, setStage);
      saveBlobAs(blob, descriptor.filename);
    } catch (err) {
      setError(err instanceof FileActionError ? err.message : "Download failed — try again.");
    } finally {
      setStage(null);
    }
  }

  return (
    <div className={styles.file}>
      <div className={styles.row}>
        <span className={styles.icon} aria-hidden="true">
          {(descriptor.filename.split(".").pop() ?? "").slice(0, 4).toUpperCase() || <FileIcon />}
        </span>
        <div className={styles.info}>
          <div className={styles.name} title={descriptor.filename}>
            {descriptor.filename}
          </div>
          <div className={`${styles.detail} type-mono`}>
            {formatBytes(descriptor.originalSize)} ·{" "}
            {isVoice ? "encrypted voice message" : "encrypted file"}
          </div>
        </div>
      </div>
      {unavailable ? (
        <div className={styles.status} role="status">
          {unavailable}
        </div>
      ) : isVoice && audioUrl ? (
        <audio className={styles.audio} controls autoPlay src={audioUrl} aria-label="Voice message" />
      ) : isVoice ? (
        <button type="button" className={styles.downloadButton} onClick={play} disabled={stage !== null}>
          {stage ? STAGE_LABEL[stage] : "▶ Play voice message"}
        </button>
      ) : (
        <button type="button" className={styles.downloadButton} onClick={download} disabled={stage !== null}>
          {stage ? STAGE_LABEL[stage] : "Download"}
        </button>
      )}
      {error && (
        <div className={styles.error} role="alert">
          {error}
        </div>
      )}
    </div>
  );
}
