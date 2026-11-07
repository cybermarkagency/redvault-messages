import { type ChangeEvent, type FormEvent, useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  AlertTriangleIcon,
  CheckIcon,
  ChevronRightIcon,
  CloudIcon,
  CopyIcon,
  DownloadIcon,
  EyeIcon,
  FileIcon,
  FolderIcon,
  FolderPlusIcon,
  InboxIcon,
  InfoIcon,
  KeyIcon,
  LinkIcon,
  LockIcon,
  RefreshIcon,
  ShareIcon,
  SlidersIcon,
  TrashIcon,
  UploadIcon,
} from "../components/icons24.js";
import * as api from "../lib/api.js";
import { blobText } from "../lib/blobBytes.js";
import { currentEntries, downloadFile, isInside, loadDrive, type DriveListing, uploadFile, createFolder } from "../lib/driveFiles.js";
import { driveKey as deriveDriveKey, type DriveEntry } from "../lib/driveMeta.js";
import { formatBytes } from "../lib/fileMessage.js";
import { isRisky, likelyPreview, type PreviewKind, previewOf, safeFileName } from "../lib/safePreview.js";
import {
  acceptIncoming,
  decodeIncoming,
  decodeOwnerShare,
  downloadIncoming,
  type IncomingFile,
  type OwnerShare,
  reconcileAll,
} from "../lib/shareOwner.js";
import { INCOMING_REFRESH_EVENT, setIncomingCount } from "../lib/shareWatch.js";
import { useAppState } from "../state/AppStateContext.js";
import type { FilesPage } from "../state/reducer.js";
import styles from "./Files.module.css";
import { ConfirmDialog, LinkSettingsDialog, Modal, PasswordDialog, PERMISSION_TEXT, RegenerateDialog, ShareDialog, statusText } from "./ShareDialogs.js";

const PAGES: { key: FilesPage; label: string; icon: JSX.Element }[] = [
  { key: "files", label: "My files", icon: <CloudIcon size={18} /> },
  { key: "links", label: "Shared links", icon: <LinkIcon size={18} /> },
  { key: "incoming", label: "Incoming", icon: <InboxIcon size={18} /> },
];

function saveBlob(blob: Blob, name: string) {
  const url = URL.createObjectURL(new Blob([blob], { type: "application/octet-stream" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = safeFileName(name);
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

const shortDate = (ms: number) => (ms > 0 ? new Date(ms).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) : "");

/**
 * Files on the web: your encrypted storage, the public links you made to
 * files and folders, and the files people uploaded to you through them.
 * Everything is decrypted here with a key derived from your account key.
 */
export function Files({ page }: { page: FilesPage }) {
  const { state, dispatch } = useAppState();
  const privateKey = state.identity?.privateKey;
  const key = useMemo(() => (privateKey ? deriveDriveKey(privateKey) : null), [privateKey]);
  const [drive, setDrive] = useState<DriveListing | null>(null);
  const [shares, setShares] = useState<OwnerShare[]>([]);
  const [incoming, setIncoming] = useState<IncomingFile[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const syncing = useRef(false);

  const loadIncoming = useCallback(async () => {
    if (!key) return;
    const { items } = await api.listIncomingFiles();
    setIncoming(items.map((i) => decodeIncoming(i, key)));
    setIncomingCount(items.length);
  }, [key]);

  const reload = useCallback(async () => {
    if (!key) return;
    setError(null);
    try {
      const [listing, links] = await Promise.all([loadDrive(key), api.listShareLinks()]);
      const owned = links.links.map((l) => decodeOwnerShare(l, key));
      setDrive(listing);
      setShares(owned);
      await loadIncoming();
      // Folder links follow Files: new uploads, renames, moves and the Trash, once per refresh.
      if (!syncing.current) {
        syncing.current = true;
        void reconcileAll(owned, listing.entries, key).finally(() => {
          syncing.current = false;
        });
      }
    } catch (err) {
      setError(err instanceof api.ApiError && err.code === "backup_disabled" ? "Files isn't available on this server." : "Couldn't load your files. Try again.");
    } finally {
      setLoading(false);
    }
  }, [key, loadIncoming]);

  useEffect(() => {
    void reload();
  }, [reload]);

  useEffect(() => {
    const onRefresh = () => void loadIncoming().catch(() => undefined);
    window.addEventListener(INCOMING_REFRESH_EVENT, onRefresh);
    return () => window.removeEventListener(INCOMING_REFRESH_EVENT, onRefresh);
  }, [loadIncoming]);

  const go = (p: FilesPage) => dispatch({ type: "OPEN_FILES", page: p });
  const activeLinks = shares.filter((s) => s.status === "active").length;

  if (!key) return null;
  return (
    <div className={styles.shell}>
      <nav className={styles.sidebar} aria-label="Files">
        <h1 className={styles.sideTitle}>Files</h1>
        <div className={styles.sideNav}>
          {PAGES.map((p) => (
            <button key={p.key} type="button" className={`${styles.sideItem} ${page === p.key ? styles.sideItemOn : ""}`} aria-current={page === p.key ? "page" : undefined} onClick={() => go(p.key)}>
              {p.icon}
              <span>{p.label}</span>
              {p.key === "links" && activeLinks > 0 && <span className={styles.count}>{activeLinks}</span>}
              {p.key === "incoming" && incoming.length > 0 && <span className={`${styles.count} ${styles.countHot}`}>{incoming.length}</span>}
            </button>
          ))}
        </div>
        {drive && (
          <div className={styles.usage}>
            <div className={styles.usageBar} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round((drive.usage.used / drive.usage.limit) * 100)}>
              <span style={{ width: `${Math.min(100, (drive.usage.used / Math.max(1, drive.usage.limit)) * 100)}%` }} />
            </div>
            <span>
              {formatBytes(drive.usage.used)} of {formatBytes(drive.usage.limit)} used
            </span>
          </div>
        )}
      </nav>
      <main className={styles.main}>
        {error && (
          <p className={styles.error} role="alert">
            {error}{" "}
            <button type="button" className={styles.linkButton} onClick={() => void reload()}>
              Try again
            </button>
          </p>
        )}
        {loading && !drive && <p className={styles.muted}>Decrypting your files on this device…</p>}
        {drive && page === "files" && <MyFiles drive={drive} shares={shares} driveKey={key} reload={reload} openLinks={() => go("links")} />}
        {drive && page === "links" && <SharedLinks drive={drive} shares={shares} driveKey={key} reload={reload} />}
        {drive && page === "incoming" && <Incoming drive={drive} incoming={incoming} shares={shares} driveKey={key} reload={reload} />}
      </main>
    </div>
  );
}

interface Upload {
  id: number;
  name: string;
  progress: number;
  error: string | null;
}

function MyFiles({ drive, shares, driveKey, reload, openLinks }: { drive: DriveListing; shares: OwnerShare[]; driveKey: Uint8Array; reload: () => Promise<void>; openLinks: () => void }) {
  const [folderId, setFolderId] = useState("");
  const [sharing, setSharing] = useState<DriveEntry | null>(null);
  const [naming, setNaming] = useState(false);
  const [uploads, setUploads] = useState<Upload[]>([]);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const input = useRef<HTMLInputElement>(null);
  const nextId = useRef(1);

  const visible = useMemo(() => currentEntries(drive.entries).filter((e) => !e.trashed && !e.unreadable), [drive]);
  const byId = useMemo(() => new Map(drive.entries.map((e) => [e.id, e])), [drive]);
  const children = visible
    .filter((e) => e.parent === folderId)
    .sort((a, b) => (a.kind === b.kind ? a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" }) : a.kind === "folder" ? -1 : 1));
  const crumbs: DriveEntry[] = [];
  for (let e = byId.get(folderId), i = 0; e && i < 64; e = byId.get(e.parent), i += 1) crumbs.unshift(e);

  const liveShares = shares.filter((s) => s.status === "active");
  const sharedState = (entry: DriveEntry): "direct" | "inside" | null => {
    if (liveShares.some((s) => s.dto.backupId === entry.id)) return "direct";
    if (liveShares.some((s) => s.dto.kind === "folder" && isInside(byId, entry.id, s.dto.backupId))) return "inside";
    return null;
  };

  async function onFiles(e: ChangeEvent<HTMLInputElement>) {
    const files = Array.from(e.target.files ?? []);
    e.target.value = "";
    for (const file of files) {
      const id = nextId.current++;
      setUploads((u) => [...u, { id, name: file.name, progress: 0, error: null }]);
      try {
        await uploadFile(file, folderId, driveKey, (p) => setUploads((u) => u.map((x) => (x.id === id ? { ...x, progress: p } : x))));
        setUploads((u) => u.filter((x) => x.id !== id));
      } catch (err) {
        setUploads((u) => u.map((x) => (x.id === id ? { ...x, error: err instanceof Error ? err.message : "Upload failed." } : x)));
      }
    }
    await reload();
  }

  async function onNewFolder(name: string) {
    await createFolder(name, folderId, driveKey);
    await reload();
  }

  async function download(entry: DriveEntry) {
    setMessage(null);
    setBusy(entry.name);
    try {
      saveBlob(await downloadFile(entry), entry.name);
    } catch (err) {
      setMessage(err instanceof Error ? err.message : "Couldn't download the file.");
    } finally {
      setBusy(null);
    }
  }

  return (
    <>
      <header className={styles.header}>
        <nav className={styles.crumbs} aria-label="Folder">
          <button type="button" className={styles.crumbButton} onClick={() => setFolderId("")} aria-current={folderId === "" ? "page" : undefined}>
            My files
          </button>
          {crumbs.map((c) => (
            <span key={c.id} className={styles.crumb}>
              <ChevronRightIcon size={14} />
              <button type="button" className={styles.crumbButton} onClick={() => setFolderId(c.id)} aria-current={c.id === folderId ? "page" : undefined}>
                {c.name}
              </button>
            </span>
          ))}
        </nav>
        <div className={styles.headActions}>
          {folderId !== "" && byId.get(folderId) && (
            <button type="button" className={styles.secondary} onClick={() => setSharing(byId.get(folderId)!)}>
              <ShareIcon size={16} /> Share folder
            </button>
          )}
          <button type="button" className={styles.secondary} onClick={() => setNaming(true)}>
            <FolderPlusIcon size={16} /> New folder
          </button>
          <button type="button" className={styles.primary} onClick={() => input.current?.click()}>
            <UploadIcon size={16} /> Upload
          </button>
          <input ref={input} type="file" multiple hidden onChange={(e) => void onFiles(e)} />
        </div>
      </header>

      {uploads.length > 0 && (
        <ul className={styles.uploads}>
          {uploads.map((u) => (
            <li key={u.id} className={styles.uploadRow}>
              <span className={styles.rowName}>{u.name}</span>
              {u.error ? (
                <span className={styles.errorInline}>{u.error}</span>
              ) : (
                <span className={styles.bar}>
                  <span style={{ width: `${Math.round(u.progress * 100)}%` }} />
                </span>
              )}
              {u.error && (
                <button type="button" className={styles.linkButton} onClick={() => setUploads((list) => list.filter((x) => x.id !== u.id))}>
                  Dismiss
                </button>
              )}
            </li>
          ))}
        </ul>
      )}
      {message && (
        <p className={styles.error} role="alert">
          {message}
        </p>
      )}
      {busy && <p className={styles.muted}>Downloading {busy}…</p>}

      {children.length === 0 ? (
        <div className={styles.empty}>
          <CloudIcon size={28} />
          <p>{folderId === "" ? "Nothing here yet. Upload files, or make a folder to share." : "This folder is empty."}</p>
        </div>
      ) : (
        <ul className={styles.table} aria-label="Files">
          {children.map((e) => {
            const shared = sharedState(e);
            return (
              <li key={e.id} className={styles.tableRow}>
                <button
                  type="button"
                  className={styles.rowMain}
                  onClick={() => (e.kind === "folder" ? setFolderId(e.id) : void download(e))}
                  title={e.kind === "folder" ? `Open ${e.name}` : `Download ${e.name}`}
                >
                  <span className={styles.rowIcon}>{e.kind === "folder" ? <FolderIcon size={18} /> : e.locked ? <LockIcon size={18} /> : <FileIcon size={18} />}</span>
                  <span className={styles.rowName}>{e.name}</span>
                  {shared && (
                    <span className={styles.sharedTag} title={shared === "direct" ? "Has a share link" : "Inside a shared folder"}>
                      <LinkIcon size={13} /> {shared === "direct" ? "Shared" : "In a shared folder"}
                    </span>
                  )}
                  <span className={styles.rowMeta}>{e.kind === "file" ? formatBytes(e.size) : ""}</span>
                  <span className={styles.rowMeta}>{shortDate(e.modified)}</span>
                </button>
                <button
                  type="button"
                  className={styles.iconButton}
                  onClick={() => setSharing(e)}
                  disabled={e.locked}
                  aria-label={`Share ${e.name}`}
                  title={e.locked ? "Unlock it first to share it" : "Share"}
                >
                  <ShareIcon size={17} />
                </button>
                {e.kind === "file" && (
                  <button type="button" className={styles.iconButton} onClick={() => void download(e)} disabled={e.locked} aria-label={`Download ${e.name}`} title="Download">
                    <DownloadIcon size={17} />
                  </button>
                )}
              </li>
            );
          })}
        </ul>
      )}

      {naming && <NewFolderDialog onClose={() => setNaming(false)} onCreate={onNewFolder} />}
      {sharing && (
        <ShareDialog
          entry={sharing}
          entries={drive.entries}
          existing={shares.filter((s) => s.dto.backupId === sharing.id)}
          driveKey={driveKey}
          onClose={() => setSharing(null)}
          onChanged={() => void reload()}
          onManage={() => {
            setSharing(null);
            openLinks();
          }}
        />
      )}
    </>
  );
}

function NewFolderDialog({ onClose, onCreate }: { onClose: () => void; onCreate: (name: string) => Promise<void> }) {
  const [name, setName] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  async function submit(e: FormEvent) {
    e.preventDefault();
    const clean = name.trim();
    if (!clean || /[\\/]/.test(clean)) {
      setError("Give the folder a name without slashes.");
      return;
    }
    setBusy(true);
    try {
      await onCreate(clean);
      onClose();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't make the folder.");
      setBusy(false);
    }
  }
  return (
    <Modal title="New folder" onClose={onClose}>
      <form className={styles.form} onSubmit={submit}>
        <input className={styles.input} placeholder="Folder name" aria-label="Folder name" value={name} maxLength={200} onChange={(e) => setName(e.target.value)} />
        {error && (
          <p className={styles.error} role="alert">
            {error}
          </p>
        )}
        <div className={styles.dialogActions}>
          <button type="button" className={styles.secondary} onClick={onClose}>
            Cancel
          </button>
          <button type="submit" className={styles.primary} disabled={busy}>
            Create
          </button>
        </div>
      </form>
    </Modal>
  );
}

type LinkDialog =
  | { kind: "settings"; share: OwnerShare }
  | { kind: "password"; share: OwnerShare }
  | { kind: "regenerate"; share: OwnerShare }
  | { kind: "revoke"; share: OwnerShare }
  | { kind: "delete"; share: OwnerShare };

function SharedLinks({ drive, shares, driveKey, reload }: { drive: DriveListing; shares: OwnerShare[]; driveKey: Uint8Array; reload: () => Promise<void> }) {
  const [dialog, setDialog] = useState<LinkDialog | null>(null);
  const [copied, setCopied] = useState<string | null>(null);
  const byId = useMemo(() => new Map(drive.entries.map((e) => [e.id, e])), [drive]);
  const order = { active: 0, expired: 1, revoked: 2 };
  const sorted = [...shares].sort((a, b) => order[a.status] - order[b.status] || (b.box?.created ?? 0) - (a.box?.created ?? 0));

  async function copy(share: OwnerShare) {
    if (!share.url) return;
    await navigator.clipboard.writeText(share.url);
    setCopied(share.dto.id);
    setTimeout(() => setCopied((c) => (c === share.dto.id ? null : c)), 2000);
  }

  return (
    <>
      <header className={styles.header}>
        <div>
          <h2 className={styles.pageTitle}>Shared links</h2>
          <p className={styles.subtitle}>Links anyone can open in a browser, without an account. Each one is separate: its own permission, password and expiry.</p>
        </div>
      </header>
      {sorted.length === 0 ? (
        <div className={styles.empty}>
          <LinkIcon size={28} />
          <p>No links yet. In My files, use Share on a file or folder.</p>
        </div>
      ) : (
        <ul className={styles.cards}>
          {sorted.map((s) => {
            const item = byId.get(s.dto.backupId);
            const d = s.dto;
            const active = s.status === "active";
            return (
              <li key={d.id} className={`${styles.card} ${active ? "" : styles.cardOff}`}>
                <div className={styles.cardHead}>
                  <span className={styles.rowIcon}>{d.kind === "folder" ? <FolderIcon size={18} /> : <FileIcon size={18} />}</span>
                  <div className={styles.cardTitle}>
                    <strong>{s.box?.label || item?.name || "Shared item"}</strong>
                    <span className={styles.muted}>{item ? `${d.kind === "folder" ? "Folder" : "File"} · ${item.name}` : "The shared item is no longer in your Files"}</span>
                  </div>
                  <span className={styles.chip}>{d.permission === "contributor" ? "View, download, upload" : "View and download"}</span>
                  <span className={`${styles.chip} ${active ? styles.chipOn : styles.chipOff}`}>{statusText(s)}</span>
                  {d.password && (
                    <span className={styles.chip} title="Password protected">
                      <KeyIcon size={13} /> Password
                    </span>
                  )}
                </div>
                <dl className={styles.facts}>
                  <div>
                    <dt>Created</dt>
                    <dd>{s.box ? new Date(s.box.created).toLocaleString() : "—"}</dd>
                  </div>
                  <div>
                    <dt>Expires</dt>
                    <dd>{d.expiresAt ? new Date(d.expiresAt).toLocaleString() : "Never"}</dd>
                  </div>
                  <div>
                    <dt>Opened</dt>
                    <dd>{d.openCount}×</dd>
                  </div>
                  <div>
                    <dt>Downloads</dt>
                    <dd>
                      {d.downloadCount}
                      {d.maxDownloads !== null ? ` of ${d.maxDownloads}` : ""}
                    </dd>
                  </div>
                  {(d.permission === "contributor" || d.upload.count > 0) && (
                    <div>
                      <dt>Uploads</dt>
                      <dd>
                        {d.upload.count} ({formatBytes(d.upload.bytes)})
                        {d.upload.inProgress > 0 ? ` · ${d.upload.inProgress} in progress` : ""}
                        {d.incoming > 0 ? ` · ${d.incoming} to review` : ""}
                      </dd>
                    </div>
                  )}
                </dl>
                <p className={styles.small}>{PERMISSION_TEXT[d.permission]} Visitors are anonymous: counts only, no names or addresses.</p>
                <div className={styles.cardActions}>
                  {active && s.url && (
                    <button type="button" className={styles.secondary} onClick={() => void copy(s)}>
                      {copied === d.id ? <CheckIcon size={16} /> : <CopyIcon size={16} />} {copied === d.id ? "Copied" : "Copy link"}
                    </button>
                  )}
                  {active && s.url && typeof navigator.share === "function" && (
                    <button type="button" className={styles.secondary} onClick={() => void navigator.share({ url: s.url! }).catch(() => undefined)}>
                      <ShareIcon size={16} /> Share…
                    </button>
                  )}
                  {d.revoked ? null : (
                    <>
                      <button type="button" className={styles.secondary} onClick={() => setDialog({ kind: "settings", share: s })} disabled={!s.box}>
                        <SlidersIcon size={16} /> Settings
                      </button>
                      {d.permission === "contributor" && (
                        <button
                          type="button"
                          className={styles.secondary}
                          onClick={() => void api.updateShareLink(d.id, { permission: "viewer" }).then(() => reload())}
                        >
                          <UploadIcon size={16} /> Turn off uploads
                        </button>
                      )}
                      <button type="button" className={styles.secondary} onClick={() => setDialog({ kind: "password", share: s })} disabled={!s.box}>
                        <KeyIcon size={16} /> {d.password ? "Password" : "Add password"}
                      </button>
                      <button type="button" className={styles.secondary} onClick={() => setDialog({ kind: "regenerate", share: s })} disabled={!s.box}>
                        <RefreshIcon size={16} /> New link
                      </button>
                      <button type="button" className={styles.danger} onClick={() => setDialog({ kind: "revoke", share: s })}>
                        Revoke
                      </button>
                    </>
                  )}
                  <button type="button" className={styles.danger} onClick={() => setDialog({ kind: "delete", share: s })} aria-label="Delete link record">
                    <TrashIcon size={16} /> Delete
                  </button>
                </div>
              </li>
            );
          })}
        </ul>
      )}
      {dialog?.kind === "settings" && (
        <LinkSettingsDialog
          share={dialog.share}
          folder={dialog.share.dto.kind === "folder"}
          driveKey={driveKey}
          onClose={() => setDialog(null)}
          onSaved={() => void reload()}
        />
      )}
      {dialog?.kind === "password" && <PasswordDialog share={dialog.share} driveKey={driveKey} onClose={() => setDialog(null)} onSaved={() => void reload()} />}
      {dialog?.kind === "regenerate" && (
        <RegenerateDialog share={dialog.share} entries={drive.entries} driveKey={driveKey} onClose={() => setDialog(null)} onSaved={() => void reload()} />
      )}
      {dialog?.kind === "revoke" && (
        <ConfirmDialog
          title="Revoke this link?"
          text="It stops working at once for everyone: no more opening, downloads or uploads. Files people already downloaded stay on their devices; nothing can take those back. This can't be undone."
          confirm="Revoke link"
          danger
          onConfirm={async () => {
            await api.revokeShareLink(dialog.share.dto.id);
            await reload();
          }}
          onClose={() => setDialog(null)}
        />
      )}
      {dialog?.kind === "delete" && (
        <ConfirmDialog
          title="Delete this link?"
          text={`${dialog.share.status === "active" ? "It stops working at once, and " : ""}it leaves your list. Files that came in through it stay in Incoming.`}
          confirm="Delete link"
          danger
          onConfirm={async () => {
            await api.deleteShareLink(dialog.share.dto.id);
            await reload();
          }}
          onClose={() => setDialog(null)}
        />
      )}
    </>
  );
}

function Incoming({
  drive,
  incoming,
  shares,
  driveKey,
  reload,
}: {
  drive: DriveListing;
  incoming: IncomingFile[];
  shares: OwnerShare[];
  driveKey: Uint8Array;
  reload: () => Promise<void>;
}) {
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const [accepting, setAccepting] = useState<IncomingFile | null>(null);
  const [rejecting, setRejecting] = useState<IncomingFile | null>(null);
  const [risky, setRisky] = useState<IncomingFile | null>(null);
  const [preview, setPreview] = useState<IncomingFile | null>(null);
  const folders = useMemo(
    () => currentEntries(drive.entries).filter((e) => e.kind === "folder" && !e.trashed && !e.unreadable),
    [drive]
  );
  const labelOf = (shareId: string | null) => {
    const share = shares.find((s) => s.dto.id === shareId);
    return share ? share.box?.label || "a share link" : "a link you deleted";
  };

  async function download(file: IncomingFile) {
    setMessage(null);
    setBusy(file.dto.id);
    try {
      saveBlob(await downloadIncoming(file), file.name);
    } catch (err) {
      setMessage({ text: err instanceof Error ? err.message : "Couldn't download the file.", error: true });
    } finally {
      setBusy(null);
    }
  }

  return (
    <>
      <header className={styles.header}>
        <div>
          <h2 className={styles.pageTitle}>Incoming</h2>
          <p className={styles.subtitle}>Files people uploaded through your links. Review them, then add them to your files or reject them.</p>
        </div>
      </header>
      <p className={styles.warning}>
        <AlertTriangleIcon size={16} />
        <span>
          Anyone with a Contributor link can upload, and uploaders are anonymous. These files are encrypted for you alone, so RedVault can't scan them for
          malware: only open what you expect, and scan downloads with your own antivirus.
        </span>
      </p>
      {message && (
        <p className={message.error ? styles.error : styles.success} role={message.error ? "alert" : "status"}>
          {message.text}
        </p>
      )}
      {incoming.length === 0 ? (
        <div className={styles.empty}>
          <InboxIcon size={28} />
          <p>Nothing waiting for review.</p>
        </div>
      ) : (
        <ul className={styles.table} aria-label="Incoming files">
          {incoming.map((f) => (
            <li key={f.dto.id} className={styles.tableRow}>
              <div className={styles.rowMain}>
                <span className={styles.rowIcon}>{f.meta && isRisky(f.name) ? <AlertTriangleIcon size={18} /> : <FileIcon size={18} />}</span>
                <span className={styles.rowName} title={f.folder ? `${f.folder}/${f.name}` : f.name}>
                  {f.meta ? f.name : "Can't be opened on this device"}
                  {f.folder && <span className={styles.muted}> · from {f.folder}/</span>}
                </span>
                <span className={styles.rowMeta}>{formatBytes(f.meta?.size ?? f.dto.encryptedSize)}</span>
                <span className={styles.rowMeta} title={`Through ${labelOf(f.dto.shareId)}`}>
                  {f.meta?.sent ? shortDate(f.meta.sent) : ""} · {labelOf(f.dto.shareId)}
                </span>
              </div>
              {f.meta && likelyPreview(f.name, f.meta.size) && (
                <button type="button" className={styles.iconButton} onClick={() => setPreview(f)} aria-label={`Preview ${f.name}`} title="Preview">
                  <EyeIcon size={17} />
                </button>
              )}
              <button
                type="button"
                className={styles.iconButton}
                disabled={!f.meta || busy === f.dto.id}
                onClick={() => (isRisky(f.name) ? setRisky(f) : void download(f))}
                aria-label={`Download ${f.name}`}
                title="Download"
              >
                <DownloadIcon size={17} />
              </button>
              <button type="button" className={styles.secondary} disabled={!f.meta} onClick={() => setAccepting(f)}>
                Add to Files
              </button>
              <button type="button" className={styles.danger} onClick={() => setRejecting(f)}>
                Reject
              </button>
            </li>
          ))}
        </ul>
      )}
      {accepting && (
        <AcceptDialog
          file={accepting}
          folders={folders}
          onClose={() => setAccepting(null)}
          onAccept={async (parent) => {
            await acceptIncoming(accepting, parent, driveKey);
            setMessage({ text: `“${accepting.name}” was added to your files.`, error: false });
            await reload();
          }}
        />
      )}
      {rejecting && (
        <ConfirmDialog
          title="Reject this file?"
          text={`“${rejecting.name}” is deleted for good and stops counting toward your storage. Whoever uploaded it isn't told.`}
          confirm="Reject"
          danger
          onConfirm={async () => {
            await api.rejectIncomingFile(rejecting.dto.id);
            await reload();
          }}
          onClose={() => setRejecting(null)}
        />
      )}
      {risky && (
        <ConfirmDialog
          title="Download a file that can run code?"
          text={`“${risky.name}” is a kind of file that can run programs or scripts when opened. Someone anonymous uploaded it. Only open it if you know what it is.`}
          confirm="Download anyway"
          danger
          onConfirm={() => download(risky)}
          onClose={() => setRisky(null)}
        />
      )}
      {preview && <IncomingPreview file={preview} onClose={() => setPreview(null)} />}
    </>
  );
}

function AcceptDialog({ file, folders, onClose, onAccept }: { file: IncomingFile; folders: DriveEntry[]; onClose: () => void; onAccept: (parent: string) => Promise<void> }) {
  const [parent, setParent] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const byId = new Map(folders.map((f) => [f.id, f]));
  const pathOf = (f: DriveEntry) => {
    const parts = [f.name];
    for (let p = byId.get(f.parent), i = 0; p && i < 64; p = byId.get(p.parent), i += 1) parts.unshift(p.name);
    return parts.join(" / ");
  };
  const options = folders.map((f) => ({ id: f.id, path: pathOf(f) })).sort((a, b) => a.path.localeCompare(b.path));
  return (
    <Modal title={`Add “${file.name}” to your files`} onClose={onClose}>
      <form
        className={styles.form}
        onSubmit={async (e) => {
          e.preventDefault();
          setBusy(true);
          setError(null);
          try {
            await onAccept(parent);
            onClose();
          } catch (err) {
            setError(err instanceof Error ? err.message : "Couldn't add the file.");
            setBusy(false);
          }
        }}
      >
        <p className={styles.dialogText}>
          <InfoIcon size={16} /> It's moved, not copied: it stays encrypted, now with your own Files key, and keeps counting toward your storage.
        </p>
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Folder</span>
          <select className={styles.input} value={parent} onChange={(e) => setParent(e.target.value)}>
            <option value="">My files (top level)</option>
            {options.map((o) => (
              <option key={o.id} value={o.id}>
                {o.path}
              </option>
            ))}
          </select>
        </label>
        {error && (
          <p className={styles.error} role="alert">
            {error}
          </p>
        )}
        <div className={styles.dialogActions}>
          <button type="button" className={styles.secondary} onClick={onClose}>
            Cancel
          </button>
          <button type="submit" className={styles.primary} disabled={busy}>
            {busy ? "Adding…" : "Add to Files"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

/** Previews only inert formats, re-typed from the file's own bytes; never HTML, SVG or scripts. */
function IncomingPreview({ file, onClose }: { file: IncomingFile; onClose: () => void }) {
  const [state, setState] = useState<{ kind: "loading" } | { kind: "ready"; preview: PreviewKind; url: string; text?: string } | { kind: "none" } | { kind: "error"; message: string }>({
    kind: "loading",
  });
  useEffect(() => {
    let url: string | null = null;
    let cancelled = false;
    (async () => {
      try {
        const preview = await previewOf(file.name, await downloadIncoming(file));
        if (cancelled) return;
        if (!preview) return setState({ kind: "none" });
        url = URL.createObjectURL(preview.blob);
        setState({ kind: "ready", preview: preview.kind, url, text: preview.kind === "text" ? await blobText(preview.blob) : undefined });
      } catch (err) {
        if (!cancelled) setState({ kind: "error", message: err instanceof Error ? err.message : "Couldn't open the preview." });
      }
    })();
    return () => {
      cancelled = true;
      if (url) URL.revokeObjectURL(url);
    };
  }, [file]);
  return (
    <Modal title={file.name} onClose={onClose} wide>
      <div className={styles.previewBody}>
        {state.kind === "loading" && <p className={styles.muted}>Decrypting…</p>}
        {state.kind === "none" && <p className={styles.muted}>This kind of file can't be previewed safely in a browser.</p>}
        {state.kind === "error" && (
          <p className={styles.error} role="alert">
            {state.message}
          </p>
        )}
        {state.kind === "ready" && state.preview === "image" && <img className={styles.media} src={state.url} alt={file.name} />}
        {state.kind === "ready" && state.preview === "video" && <video className={styles.media} src={state.url} controls playsInline />}
        {state.kind === "ready" && state.preview === "audio" && <audio src={state.url} controls />}
        {state.kind === "ready" && state.preview === "pdf" && <iframe className={styles.pdf} src={state.url} title={file.name} referrerPolicy="no-referrer" />}
        {state.kind === "ready" && state.preview === "text" && <pre className={styles.text}>{state.text}</pre>}
      </div>
      <div className={styles.dialogActions}>
        <button type="button" className={styles.primary} onClick={onClose}>
          Close
        </button>
      </div>
    </Modal>
  );
}
