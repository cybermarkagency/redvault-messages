// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type DragEvent, type FormEvent, useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  AlertTriangleIcon,
  CheckIcon,
  ChevronRightIcon,
  CloseIcon,
  DownloadIcon,
  EyeIcon,
  FileIcon,
  FolderIcon,
  InfoIcon,
  LockIcon,
  RefreshIcon,
  UploadIcon,
} from "../components/icons24.js";
import { blobText } from "../lib/blobBytes.js";
import { formatBytes } from "../lib/fileMessage.js";
import { likelyPreview, type PreviewKind, previewOf, safeFileName } from "../lib/safePreview.js";
import { ShareCryptoError, parseShareLocation, type ShareFileMeta } from "../lib/shareCrypto.js";
import { ShareLinkError, type ShareLookupDto } from "../lib/sharePublicApi.js";
import { lookupLink, type OpenedShare, type ShareNode, ShareUploadJob, unlockLink } from "../lib/shareTransfer.js";
import { buildZip, ZIP_MAX_BYTES, ZIP_MAX_FILES } from "../lib/zipStore.js";
import styles from "./SharePage.module.css";

type Phase =
  | { kind: "loading" }
  | { kind: "error"; title: string; message: string }
  | { kind: "password"; lookup: ShareLookupDto; error: string | null; busy: boolean }
  | { kind: "open"; share: OpenedShare };

const ERROR_TITLES: Record<string, string> = {
  share_revoked: "This link was turned off",
  share_expired: "This link has expired",
  share_unavailable: "No longer available",
  not_found: "Link not found",
  invalid: "This link is incomplete",
};

function errorPhase(err: unknown): Phase {
  if (err instanceof ShareLinkError) {
    return { kind: "error", title: ERROR_TITLES[err.code] ?? "Couldn't open the link", message: err.message };
  }
  if (err instanceof ShareCryptoError) return { kind: "error", title: "Couldn't open the link", message: err.message };
  return { kind: "error", title: "Couldn't open the link", message: "Something went wrong. Reload the page to try again." };
}

function saveBlob(blob: Blob, name: string) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = safeFileName(name);
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

const dateFormat: Intl.DateTimeFormatOptions = { year: "numeric", month: "short", day: "numeric", hour: "numeric", minute: "2-digit" };

/** The public page for a share link: …/share/?id=<publicId>#<secret>. No account, no app. */
export function SharePage({ location = window.location }: { location?: Pick<Location, "search" | "hash"> }) {
  // The fragment can change without a reload (a link pasted into the same tab): read it again then.
  const [hash, setHash] = useState(location.hash);
  useEffect(() => {
    const onHash = () => setHash(window.location.hash);
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);
  const link = useMemo(() => parseShareLocation(location.search, hash), [location.search, hash]);
  const [phase, setPhase] = useState<Phase>(() =>
    link ? { kind: "loading" } : { kind: "error", title: ERROR_TITLES.invalid, message: "The link is missing part of its address. Copy the whole link, including everything after #." }
  );
  const [password, setPassword] = useState("");

  useEffect(() => {
    if (!link) {
      setPhase({ kind: "error", title: ERROR_TITLES.invalid, message: "The link is missing part of its address. Copy the whole link, including everything after #." });
      return;
    }
    setPhase({ kind: "loading" });
    let cancelled = false;
    (async () => {
      try {
        const lookup = await lookupLink(link.publicId, link.secret);
        if (cancelled) return;
        if (lookup.password) {
          setPhase({ kind: "password", lookup, error: null, busy: false });
          return;
        }
        const share = await unlockLink(link.publicId, link.secret, null, lookup);
        if (!cancelled) setPhase({ kind: "open", share });
      } catch (err) {
        if (!cancelled) setPhase(errorPhase(err));
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [link]);

  async function unlock(e: FormEvent) {
    e.preventDefault();
    if (!link || phase.kind !== "password" || password === "") return;
    const lookup = phase.lookup;
    setPhase({ kind: "password", lookup, error: null, busy: true });
    try {
      const share = await unlockLink(link.publicId, link.secret, password, lookup);
      setPassword("");
      setPhase({ kind: "open", share });
    } catch (err) {
      if (err instanceof ShareLinkError && (err.code === "share_password_wrong" || err.code === "share_password_locked")) {
        setPhase({ kind: "password", lookup, error: err.message, busy: false });
      } else {
        setPhase(errorPhase(err));
      }
    }
  }

  return (
    <div className={styles.page}>
      <header className={styles.header}>
        <span className={`${styles.brand} type-wordmark`}>RedVault Systems</span>
        <span className={styles.badge}>
          <LockIcon size={14} />
          End-to-end encrypted
        </span>
      </header>
      <main className={styles.main}>
        {phase.kind === "loading" && (
          <div className={styles.card} aria-busy="true">
            <p className={styles.muted}>Opening the link…</p>
          </div>
        )}
        {phase.kind === "error" && (
          <div className={styles.card} role="alert">
            <span className={styles.glyph} aria-hidden>
              <AlertTriangleIcon />
            </span>
            <h1 className={`${styles.title} type-display`}>{phase.title}</h1>
            <p className={styles.muted}>{phase.message}</p>
          </div>
        )}
        {phase.kind === "password" && (
          <form className={styles.card} onSubmit={unlock}>
            <span className={styles.glyph} aria-hidden>
              <LockIcon />
            </span>
            <h1 className={`${styles.title} type-display`}>This link needs a password</h1>
            <p className={styles.muted}>Its owner set one. Ask them for it: it isn't part of the link.</p>
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
            {phase.error && (
              <p className={styles.error} role="alert">
                {phase.error}
              </p>
            )}
            <button type="submit" className={styles.primary} disabled={phase.busy || password === ""}>
              {phase.busy ? "Unlocking…" : "Open"}
            </button>
            {phase.busy && <p className={styles.small}>Checking the password takes a few seconds on purpose: it makes guessing slow.</p>}
          </form>
        )}
        {phase.kind === "open" && <OpenShare share={phase.share} />}
      </main>
      <footer className={styles.footer}>
        Files are decrypted in this browser. RedVault's servers only ever store the encrypted copy and never see the link's key.
      </footer>
    </div>
  );
}

function OpenShare({ share }: { share: OpenedShare }) {
  const root = share.root;
  const [folderId, setFolderId] = useState(root?.id ?? "");
  const [preview, setPreview] = useState<ShareNode | null>(null);
  const [busy, setBusy] = useState<{ label: string; progress: number } | null>(null);
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const [, refresh] = useState(0);

  if (!root) {
    return (
      <div className={styles.card} role="alert">
        <h1 className={`${styles.title} type-display`}>Nothing to show</h1>
        <p className={styles.muted}>What this link shared is empty, or no longer available.</p>
      </div>
    );
  }

  const isFolder = root.kind === "folder";
  const expires = share.contents.expiresAt ? new Date(share.contents.expiresAt) : null;
  const children = isFolder ? share.children(folderId) : [];
  const path = isFolder ? share.path(folderId) : [];
  const allFiles = isFolder ? share.filesUnder(root.id) : [];

  async function download(node: ShareNode) {
    setMessage(null);
    setBusy({ label: `Downloading ${node.name}`, progress: 0 });
    try {
      const blob = await share.download(node, { onProgress: (p) => setBusy({ label: `Downloading ${node.name}`, progress: p }) });
      saveBlob(blob, node.name);
    } catch (err) {
      setMessage({ text: err instanceof Error ? err.message : "Couldn't download the file.", error: true });
    } finally {
      setBusy(null);
      refresh((n) => n + 1);
    }
  }

  async function downloadAll() {
    setMessage(null);
    const files = share.filesUnder(folderId);
    if (files.length === 0) return;
    const total = files.reduce((n, f) => n + (f.node.meta as ShareFileMeta).size, 0);
    if (files.length > ZIP_MAX_FILES || total > ZIP_MAX_BYTES) {
      setMessage({ text: "This folder is too big to download as one archive. Download its files one by one.", error: true });
      return;
    }
    const folderName = share.nodes.get(folderId)?.name ?? "Shared files";
    try {
      const ticket = await share.ticketFor("all");
      const entries = [];
      let done = 0;
      for (const { node, path: filePath } of files) {
        const size = (node.meta as ShareFileMeta).size;
        setBusy({ label: `Downloading ${files.length} files`, progress: total === 0 ? 0 : done / total });
        const blob = await share.download(node, {
          ticket,
          onProgress: (p) => setBusy({ label: `Downloading ${files.length} files`, progress: total === 0 ? 0 : (done + p * size) / total }),
        });
        done += size;
        entries.push({ path: filePath, data: blob, modified: (node.meta as ShareFileMeta).modified });
      }
      setBusy({ label: "Packing the archive", progress: 1 });
      saveBlob(await buildZip(entries), `${folderName}.zip`);
    } catch (err) {
      setMessage({ text: err instanceof Error ? err.message : "Couldn't download the files.", error: true });
    } finally {
      setBusy(null);
      refresh((n) => n + 1);
    }
  }

  const remaining = share.downloadsRemaining;

  return (
    <div className={styles.shell}>
      <section className={styles.summary}>
        <span className={styles.glyph} aria-hidden>
          {isFolder ? <FolderIcon /> : <FileIcon />}
        </span>
        <div className={styles.summaryText}>
          <h1 className={`${styles.title} type-display`}>{root.name}</h1>
          <p className={styles.muted}>
            {isFolder ? `${allFiles.length} ${allFiles.length === 1 ? "file" : "files"}` : formatBytes((root.meta as ShareFileMeta).size)}
            {expires && ` · Link expires ${expires.toLocaleString(undefined, dateFormat)}`}
            {remaining !== null && ` · ${remaining} ${remaining === 1 ? "download" : "downloads"} left`}
          </p>
        </div>
      </section>

      <p className={styles.note}>
        <InfoIcon size={16} />
        <span>
          {share.canUpload
            ? "You can view, download and add files here. You can't delete, rename or replace anything."
            : "You can view and download. Uploading and editing are turned off."}
        </span>
      </p>
      {share.unreadable > 0 && (
        <p className={styles.warning}>
          {share.unreadable} {share.unreadable === 1 ? "item" : "items"} couldn't be opened and {share.unreadable === 1 ? "isn't" : "aren't"} shown.
        </p>
      )}

      {busy && (
        <div className={styles.busy} role="status">
          <span>{busy.label}…</span>
          <div className={styles.progress} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(busy.progress * 100)}>
            <span style={{ width: `${Math.round(busy.progress * 100)}%` }} />
          </div>
        </div>
      )}
      {message && (
        <p className={message.error ? styles.error : styles.success} role={message.error ? "alert" : "status"}>
          {message.text}
        </p>
      )}

      {isFolder ? (
        <section className={styles.browser} aria-label="Shared files">
          <div className={styles.toolbar}>
            <nav className={styles.crumbs} aria-label="Folder">
              {path.map((node, i) => (
                <span key={node.id} className={styles.crumb}>
                  {i > 0 && <ChevronRightIcon size={14} />}
                  {i < path.length - 1 ? (
                    <button type="button" className={styles.crumbButton} onClick={() => setFolderId(node.id)}>
                      {node.name}
                    </button>
                  ) : (
                    <span aria-current="page">{node.name}</span>
                  )}
                </span>
              ))}
            </nav>
            {share.filesUnder(folderId).length > 0 && (
              <button type="button" className={styles.secondary} disabled={busy !== null} onClick={downloadAll}>
                <DownloadIcon size={16} />
                Download all
              </button>
            )}
          </div>
          {children.length === 0 ? (
            <p className={styles.empty}>This folder is empty.</p>
          ) : (
            <ul className={styles.list}>
              {children.map((node) => (
                <Row
                  key={node.id}
                  node={node}
                  disabled={busy !== null}
                  onOpen={() => (node.kind === "folder" ? setFolderId(node.id) : setPreview(node))}
                  onDownload={() => download(node)}
                />
              ))}
            </ul>
          )}
        </section>
      ) : (
        <div className={styles.actions}>
          {likelyPreview(root.name, (root.meta as ShareFileMeta).size) && (
            <button type="button" className={styles.secondary} disabled={busy !== null} onClick={() => setPreview(root)}>
              <EyeIcon size={16} />
              Preview
            </button>
          )}
          <button type="button" className={styles.primary} disabled={busy !== null} onClick={() => download(root)}>
            <DownloadIcon size={16} />
            Download
          </button>
        </div>
      )}

      {share.canUpload && <UploadPanel share={share} />}
      {preview && <PreviewDialog share={share} node={preview} onClose={() => setPreview(null)} onDownload={() => download(preview)} />}
    </div>
  );
}

function Row({ node, disabled, onOpen, onDownload }: { node: ShareNode; disabled: boolean; onOpen: () => void; onDownload: () => void }) {
  const meta = node.meta.kind === "file" ? (node.meta as ShareFileMeta) : null;
  const canPreview = meta ? likelyPreview(node.name, meta.size) !== null : true;
  return (
    <li className={styles.row}>
      <button type="button" className={styles.rowMain} onClick={onOpen} disabled={disabled || (!canPreview && node.kind === "file")} title={node.name}>
        <span className={styles.rowIcon} aria-hidden>
          {node.kind === "folder" ? <FolderIcon size={18} /> : <FileIcon size={18} />}
        </span>
        <span className={styles.rowName}>{node.name}</span>
        <span className={styles.rowMeta}>
          {meta ? `${formatBytes(meta.size)}${meta.modified ? ` · ${new Date(meta.modified).toLocaleDateString()}` : ""}` : "Folder"}
        </span>
      </button>
      {meta && (
        <button type="button" className={styles.iconButton} onClick={onDownload} disabled={disabled} aria-label={`Download ${node.name}`} title="Download">
          <DownloadIcon size={18} />
        </button>
      )}
    </li>
  );
}

function PreviewDialog({ share, node, onClose, onDownload }: { share: OpenedShare; node: ShareNode; onClose: () => void; onDownload: () => void }) {
  const [state, setState] = useState<
    { kind: "loading"; progress: number } | { kind: "ready"; preview: PreviewKind; url: string; text?: string } | { kind: "none" } | { kind: "error"; message: string }
  >({ kind: "loading", progress: 0 });
  const closeRef = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    closeRef.current?.focus();
    let url: string | null = null;
    let cancelled = false;
    const controller = new AbortController();
    (async () => {
      try {
        const blob = await share.download(node, { signal: controller.signal, onProgress: (p) => !cancelled && setState({ kind: "loading", progress: p }) });
        const preview = await previewOf(node.name, blob);
        if (cancelled) return;
        if (!preview) {
          setState({ kind: "none" });
          return;
        }
        url = URL.createObjectURL(preview.blob);
        setState({ kind: "ready", preview: preview.kind, url, text: preview.kind === "text" ? await blobText(preview.blob) : undefined });
      } catch (err) {
        if (!cancelled) setState({ kind: "error", message: err instanceof Error ? err.message : "Couldn't open the preview." });
      }
    })();
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", onKey);
    return () => {
      cancelled = true;
      controller.abort();
      window.removeEventListener("keydown", onKey);
      if (url) URL.revokeObjectURL(url);
    };
  }, [share, node, onClose]);

  return (
    <div className={styles.overlay} role="dialog" aria-modal="true" aria-label={`Preview of ${node.name}`}>
      <div className={styles.previewCard}>
        <div className={styles.previewHeader}>
          <span className={styles.previewName}>{node.name}</span>
          <button type="button" className={styles.iconButton} onClick={onDownload} aria-label="Download" title="Download">
            <DownloadIcon size={18} />
          </button>
          <button ref={closeRef} type="button" className={styles.iconButton} onClick={onClose} aria-label="Close preview" title="Close">
            <CloseIcon size={18} />
          </button>
        </div>
        <div className={styles.previewBody}>
          {state.kind === "loading" && (
            <div className={styles.progress} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(state.progress * 100)}>
              <span style={{ width: `${Math.round(state.progress * 100)}%` }} />
            </div>
          )}
          {state.kind === "none" && <p className={styles.muted}>This kind of file can't be previewed safely in a browser. Download it instead.</p>}
          {state.kind === "error" && (
            <p className={styles.error} role="alert">
              {state.message}
            </p>
          )}
          {state.kind === "ready" && state.preview === "image" && <img className={styles.media} src={state.url} alt={node.name} />}
          {state.kind === "ready" && state.preview === "video" && <video className={styles.media} src={state.url} controls playsInline />}
          {state.kind === "ready" && state.preview === "audio" && <audio className={styles.audio} src={state.url} controls />}
          {state.kind === "ready" && state.preview === "pdf" && (
            <iframe className={styles.pdf} src={state.url} title={node.name} referrerPolicy="no-referrer" />
          )}
          {state.kind === "ready" && state.preview === "text" && <pre className={styles.text}>{state.text}</pre>}
        </div>
      </div>
    </div>
  );
}

function UploadPanel({ share }: { share: OpenedShare }) {
  const [jobs, setJobs] = useState<ShareUploadJob[]>([]);
  const [dragging, setDragging] = useState(false);
  const [, bump] = useState(0);
  const fileInput = useRef<HTMLInputElement>(null);
  const folderInput = useRef<HTMLInputElement>(null);
  const running = useRef(false);
  const queue = useRef<ShareUploadJob[]>([]);
  const limits = share.contents.uploads!;

  // One file at a time, in the order they were added (including ones added while others upload).
  const pump = useCallback(async () => {
    if (running.current) return;
    running.current = true;
    try {
      for (;;) {
        const next = queue.current.find((j) => j.state === "waiting");
        if (!next) break;
        await next.start();
      }
    } finally {
      running.current = false;
    }
  }, []);

  useEffect(() => {
    // Warn before leaving while files are still going up.
    const onLeave = (e: BeforeUnloadEvent) => {
      if (jobs.some((j) => j.state === "uploading" || j.state === "waiting" || j.state === "finishing")) e.preventDefault();
    };
    window.addEventListener("beforeunload", onLeave);
    return () => window.removeEventListener("beforeunload", onLeave);
  }, [jobs]);

  function add(files: FileList | File[]) {
    const list = Array.from(files);
    if (list.length === 0) return;
    const target = { publicId: share.publicId, access: share.keys.access, uploadPublicKey: share.envelope.upload!, limits };
    const added = list.map((file) => new ShareUploadJob(target, file, () => bump((n) => n + 1)));
    queue.current = [...queue.current, ...added];
    setJobs(queue.current);
    void pump();
  }

  function onDrop(e: DragEvent) {
    e.preventDefault();
    setDragging(false);
    add(e.dataTransfer.files);
  }

  const done = jobs.filter((j) => j.state === "done").length;
  const active = jobs.some((j) => j.state === "uploading" || j.state === "waiting" || j.state === "finishing");

  return (
    <section className={styles.upload} aria-label="Add files">
      <h2 className={styles.sectionTitle}>Add files</h2>
      <div
        className={`${styles.drop} ${dragging ? styles.dropActive : ""}`}
        onDragOver={(e) => {
          e.preventDefault();
          setDragging(true);
        }}
        onDragLeave={() => setDragging(false)}
        onDrop={onDrop}
      >
        <UploadIcon size={22} />
        <p className={styles.dropText}>Drop files here, or</p>
        <div className={styles.actions}>
          <button type="button" className={styles.primary} onClick={() => fileInput.current?.click()}>
            Choose files
          </button>
          <button type="button" className={styles.secondary} onClick={() => folderInput.current?.click()}>
            Choose a folder
          </button>
        </div>
        <p className={styles.small}>
          Encrypted in this browser before upload; only the owner can open them. They go to the owner for review and can't replace anything in this folder.
          {limits.maxFileSize !== null && ` Up to ${formatBytes(limits.maxFileSize)} per file.`}
          {limits.remainingFiles !== null && ` ${Math.max(0, limits.remainingFiles - done)} more ${limits.remainingFiles - done === 1 ? "file" : "files"} allowed.`}
        </p>
        <input
          ref={fileInput}
          type="file"
          multiple
          hidden
          onChange={(e) => {
            if (e.target.files) add(e.target.files);
            e.target.value = "";
          }}
        />
        <input
          ref={folderInput}
          type="file"
          multiple
          hidden
          {...({ webkitdirectory: "", directory: "" } as Record<string, string>)}
          onChange={(e) => {
            if (e.target.files) add(e.target.files);
            e.target.value = "";
          }}
        />
      </div>

      {jobs.length > 0 && (
        <ul className={styles.jobs}>
          {jobs.map((job, i) => (
            <li key={i} className={styles.job}>
              <span className={styles.jobName} title={job.folder ? `${job.folder}/${job.name}` : job.name}>
                {job.folder ? `${job.folder}/` : ""}
                {job.name}
              </span>
              <span className={styles.jobSize}>{formatBytes(job.file.size)}</span>
              <span className={styles.jobState}>
                {job.state === "done" && (
                  <span className={styles.successInline}>
                    <CheckIcon size={16} /> Sent
                  </span>
                )}
                {job.state === "waiting" && "Waiting"}
                {job.state === "finishing" && "Finishing"}
                {job.state === "uploading" && `${Math.round(job.progress * 100)}%`}
                {job.state === "cancelled" && "Cancelled"}
                {(job.state === "failed" || job.state === "paused") && <span className={styles.errorInline}>{job.error}</span>}
              </span>
              {(job.state === "uploading" || job.state === "waiting") && (
                <div className={styles.jobBar}>
                  <span style={{ width: `${Math.round(job.progress * 100)}%` }} />
                </div>
              )}
              <span className={styles.jobActions}>
                {job.state === "paused" && (
                  <button type="button" className={styles.iconButton} onClick={() => void job.resume()} aria-label={`Retry ${job.name}`} title="Retry">
                    <RefreshIcon size={16} />
                  </button>
                )}
                {(job.state === "uploading" || job.state === "waiting" || job.state === "paused") && (
                  <button type="button" className={styles.iconButton} onClick={() => void job.cancel()} aria-label={`Cancel ${job.name}`} title="Cancel">
                    <CloseIcon size={16} />
                  </button>
                )}
              </span>
            </li>
          ))}
        </ul>
      )}
      {done > 0 && !active && (
        <p className={styles.success} role="status">
          {done === 1 ? "1 file was" : `${done} files were`} sent to the owner, encrypted. They'll see {done === 1 ? "it" : "them"} once they review new files.
        </p>
      )}
    </section>
  );
}
