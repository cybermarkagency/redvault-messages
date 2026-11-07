import { type FormEvent, type ReactNode, useRef, useState } from "react";
import { AlertTriangleIcon, CheckIcon, CopyIcon, InfoIcon, LinkIcon, ShareIcon } from "../components/icons24.js";
import { ToggleSwitch } from "../components/ToggleSwitch.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import * as api from "../lib/api.js";
import type { DriveEntry } from "../lib/driveMeta.js";
import { createShare, type OwnerShare, regenerateShare, relabelShare, setSharePassword, type ShareOptions } from "../lib/shareOwner.js";
import styles from "./Files.module.css";

// Dialogs for public share links (docs/public-share-links.md): making one,
// its settings, its password, regenerating it. Labels follow the spec's
// wording so people can tell exactly what a recipient can do.

export const PERMISSION_TEXT = {
  viewer: "Anyone with this link can view and download files. Uploading and editing are disabled.",
  contributor: "Anyone with this link can view and download files, and upload new files. They cannot delete or replace your files.",
};
export const CONTRIBUTOR_WARNING =
  "Anyone who receives or is forwarded this link can upload files until you change its permissions, revoke it, or it expires.";
export const PASSWORD_NOTE = "The password isn't part of the link. Send it separately (another app, or in person) so one leaked message isn't enough.";

const MB = 1024 * 1024;
const HOUR = 60 * 60 * 1000;

export type ExpiryChoice = "never" | "1h" | "1d" | "7d" | "30d" | "custom";

export function expiryDate(choice: ExpiryChoice, custom: string): Date | null {
  const now = Date.now();
  switch (choice) {
    case "never":
      return null;
    case "1h":
      return new Date(now + HOUR);
    case "1d":
      return new Date(now + 24 * HOUR);
    case "7d":
      return new Date(now + 7 * 24 * HOUR);
    case "30d":
      return new Date(now + 30 * 24 * HOUR);
    case "custom": {
      const d = new Date(custom);
      return Number.isNaN(d.getTime()) ? null : d;
    }
  }
}

/** "2026-10-20T14:30" in local time, for <input type="datetime-local">. */
export function localInputValue(d: Date): string {
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

const positiveOrNull = (text: string, scale = 1): number | null => {
  const n = Number(text);
  return text.trim() === "" || !Number.isFinite(n) || n <= 0 ? null : Math.round(n * scale);
};

export function Modal({ title, onClose, children, wide }: { title: string; onClose: () => void; children: ReactNode; wide?: boolean }) {
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);
  return (
    <div className={styles.overlay} onClick={onClose}>
      <div
        ref={ref}
        className={`${styles.dialog} ${wide ? styles.dialogWide : ""}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        onClick={(e) => e.stopPropagation()}
      >
        <h2 className={styles.dialogTitle}>{title}</h2>
        {children}
      </div>
    </div>
  );
}

/** Copy, and the device's own share sheet where there is one. */
export function LinkResult({ url, hasPassword, onDone }: { url: string; hasPassword: boolean; onDone: () => void }) {
  const [copied, setCopied] = useState(false);
  async function copy() {
    try {
      await navigator.clipboard.writeText(url);
      setCopied(true);
    } catch {
      setCopied(false);
    }
  }
  const canShare = typeof navigator !== "undefined" && typeof navigator.share === "function";
  return (
    <div className={styles.result}>
      <label className={styles.field}>
        <span className={styles.fieldLabel}>Link</span>
        <input className={styles.input} readOnly value={url} onFocus={(e) => e.currentTarget.select()} aria-label="Share link" />
      </label>
      {hasPassword && (
        <p className={styles.note}>
          <InfoIcon size={16} /> {PASSWORD_NOTE}
        </p>
      )}
      <div className={styles.dialogActions}>
        {canShare && (
          <button type="button" className={styles.secondary} onClick={() => void navigator.share({ url }).catch(() => undefined)}>
            <ShareIcon size={16} /> Share…
          </button>
        )}
        <button type="button" className={styles.secondary} onClick={() => void copy()}>
          {copied ? <CheckIcon size={16} /> : <CopyIcon size={16} />} {copied ? "Copied" : "Copy link"}
        </button>
        <button type="button" className={styles.primary} onClick={onDone}>
          Done
        </button>
      </div>
    </div>
  );
}

function ExpiryField({ choice, setChoice, custom, setCustom }: { choice: ExpiryChoice; setChoice: (c: ExpiryChoice) => void; custom: string; setCustom: (v: string) => void }) {
  return (
    <div className={styles.fieldRow}>
      <label className={styles.field}>
        <span className={styles.fieldLabel}>Expires</span>
        <select className={styles.input} value={choice} onChange={(e) => setChoice(e.target.value as ExpiryChoice)}>
          <option value="never">Never</option>
          <option value="1h">In 1 hour</option>
          <option value="1d">In 1 day</option>
          <option value="7d">In 7 days</option>
          <option value="30d">In 30 days</option>
          <option value="custom">On a date…</option>
        </select>
      </label>
      {choice === "custom" && (
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Date and time (your time zone)</span>
          <input className={styles.input} type="datetime-local" value={custom} min={localInputValue(new Date())} onChange={(e) => setCustom(e.target.value)} />
        </label>
      )}
    </div>
  );
}

function PermissionField({
  value,
  onChange,
  folder,
}: {
  value: "viewer" | "contributor";
  onChange: (v: "viewer" | "contributor") => void;
  folder: boolean;
}) {
  return (
    <fieldset className={styles.permissions}>
      <legend className={styles.fieldLabel}>Recipients can</legend>
      <label className={`${styles.choice} ${value === "viewer" ? styles.choiceOn : ""}`}>
        <input type="radio" name="permission" checked={value === "viewer"} onChange={() => onChange("viewer")} />
        <span>
          <strong>View and download</strong>
          <span className={styles.choiceText}>{PERMISSION_TEXT.viewer}</span>
        </span>
      </label>
      <label className={`${styles.choice} ${value === "contributor" ? styles.choiceOn : ""} ${folder ? "" : styles.choiceOff}`}>
        <input type="radio" name="permission" checked={value === "contributor"} disabled={!folder} onChange={() => onChange("contributor")} />
        <span>
          <strong>View, download and upload</strong>
          <span className={styles.choiceText}>{folder ? PERMISSION_TEXT.contributor : "Folder links only: a file link can't take uploads."}</span>
        </span>
      </label>
      {value === "contributor" && (
        <p className={styles.warning}>
          <AlertTriangleIcon size={16} /> {CONTRIBUTOR_WARNING}
        </p>
      )}
    </fieldset>
  );
}

function UploadLimits({ size, setSize, allowance, setAllowance, files, setFiles }: Record<"size" | "allowance" | "files", string> & Record<"setSize" | "setAllowance" | "setFiles", (v: string) => void>) {
  return (
    <div className={styles.fieldRow}>
      <label className={styles.field}>
        <span className={styles.fieldLabel}>Largest file (MB)</span>
        <input className={styles.input} inputMode="decimal" placeholder="No limit" value={size} onChange={(e) => setSize(e.target.value)} />
      </label>
      <label className={styles.field}>
        <span className={styles.fieldLabel}>Total uploads (MB)</span>
        <input className={styles.input} inputMode="decimal" placeholder="Your free space" value={allowance} onChange={(e) => setAllowance(e.target.value)} />
      </label>
      <label className={styles.field}>
        <span className={styles.fieldLabel}>Most files</span>
        <input className={styles.input} inputMode="numeric" placeholder="No limit" value={files} onChange={(e) => setFiles(e.target.value)} />
      </label>
    </div>
  );
}

/** Share: the item's existing links, and a new one with its permission, password, expiry and limits. */
export function ShareDialog({
  entry,
  entries,
  existing,
  driveKey,
  onClose,
  onChanged,
  onManage,
}: {
  entry: DriveEntry;
  entries: DriveEntry[];
  existing: OwnerShare[];
  driveKey: Uint8Array;
  onClose: () => void;
  onChanged: () => void;
  onManage: () => void;
}) {
  const folder = entry.kind === "folder";
  const [label, setLabel] = useState("");
  const [permission, setPermission] = useState<"viewer" | "contributor">("viewer");
  const [usePassword, setUsePassword] = useState(false);
  const [password, setPassword] = useState("");
  const [expiry, setExpiry] = useState<ExpiryChoice>("never");
  const [custom, setCustom] = useState(localInputValue(new Date(Date.now() + 7 * 24 * HOUR)));
  const [maxDownloads, setMaxDownloads] = useState("");
  const [size, setSize] = useState("");
  const [allowance, setAllowance] = useState("");
  const [files, setFiles] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [made, setMade] = useState<OwnerShare | null>(null);
  const [copiedId, setCopiedId] = useState<string | null>(null);

  async function submit(e: FormEvent) {
    e.preventDefault();
    setError(null);
    if (usePassword && password.length < 8) {
      setError("Use a password of at least 8 characters.");
      return;
    }
    const expiresAt = expiryDate(expiry, custom);
    if (expiry === "custom" && (!expiresAt || expiresAt.getTime() <= Date.now())) {
      setError("Pick a date and time in the future.");
      return;
    }
    const options: ShareOptions = {
      permission,
      label: label || entry.name,
      password: usePassword ? password : null,
      expiresAt,
      maxDownloads: positiveOrNull(maxDownloads),
      upload:
        permission === "contributor"
          ? { maxFileSize: positiveOrNull(size, MB), allowance: positiveOrNull(allowance, MB), maxFiles: positiveOrNull(files) }
          : null,
    };
    setBusy(true);
    try {
      const share = await createShare(entry, entries, options, driveKey);
      setMade(share);
      setPassword("");
      onChanged();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't make the link. Try again.");
    } finally {
      setBusy(false);
    }
  }

  if (made?.url) {
    return (
      <Modal title="Link ready" onClose={onClose}>
        <p className={styles.dialogText}>{PERMISSION_TEXT[made.dto.permission]}</p>
        <LinkResult url={made.url} hasPassword={made.dto.password !== null} onDone={onClose} />
      </Modal>
    );
  }

  return (
    <Modal title={`Share “${entry.name}”`} onClose={onClose} wide>
      {existing.length > 0 && (
        <section className={styles.existing} aria-label="Links to this item">
          <h3 className={styles.subTitle}>Links you made</h3>
          <ul className={styles.existingList}>
            {existing.map((s) => (
              <li key={s.dto.id} className={styles.existingRow}>
                <LinkIcon size={16} />
                <span className={styles.existingName}>{s.box?.label || entry.name}</span>
                <span className={styles.chip}>{s.dto.permission === "contributor" ? "Contributor" : "Viewer"}</span>
                <span className={`${styles.chip} ${s.status !== "active" ? styles.chipOff : ""}`}>{statusText(s)}</span>
                {s.url && s.status === "active" && (
                  <button
                    type="button"
                    className={styles.linkButton}
                    onClick={() => {
                      void navigator.clipboard.writeText(s.url!).then(() => setCopiedId(s.dto.id));
                    }}
                  >
                    {copiedId === s.dto.id ? "Copied" : "Copy"}
                  </button>
                )}
              </li>
            ))}
          </ul>
          <button type="button" className={styles.linkButton} onClick={onManage}>
            Manage links
          </button>
        </section>
      )}
      <form className={styles.form} onSubmit={submit}>
        <h3 className={styles.subTitle}>New link</h3>
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Name (only you see it)</span>
          <input className={styles.input} placeholder={entry.name} value={label} maxLength={120} onChange={(e) => setLabel(e.target.value)} />
        </label>
        <PermissionField value={permission} onChange={setPermission} folder={folder} />
        {permission === "contributor" && (
          <UploadLimits size={size} setSize={setSize} allowance={allowance} setAllowance={setAllowance} files={files} setFiles={setFiles} />
        )}
        <div className={styles.toggleRow}>
          <span>
            <strong>Password</strong>
            <span className={styles.choiceText}>Needed to open the link, on top of the link itself.</span>
          </span>
          <ToggleSwitch checked={usePassword} onChange={setUsePassword} label="Require a password" />
        </div>
        {usePassword && (
          <>
            <input
              className={styles.input}
              type="password"
              autoComplete="new-password"
              placeholder="At least 8 characters"
              aria-label="Link password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
            />
            <p className={styles.note}>
              <InfoIcon size={16} /> {PASSWORD_NOTE}
            </p>
          </>
        )}
        <ExpiryField choice={expiry} setChoice={setExpiry} custom={custom} setCustom={setCustom} />
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Download limit</span>
          <input className={styles.input} inputMode="numeric" placeholder="No limit" value={maxDownloads} onChange={(e) => setMaxDownloads(e.target.value)} />
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
            {busy ? "Encrypting…" : "Create link"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

export function statusText(s: OwnerShare): string {
  if (s.status === "revoked") return "Revoked";
  if (s.status === "expired") return "Expired";
  if (s.dto.maxDownloads !== null && s.dto.downloadCount >= s.dto.maxDownloads) return "Download limit reached";
  return "Active";
}

/** Permission, expiry, limits and name of an existing link: applied on the server at once. */
export function LinkSettingsDialog({ share, folder, driveKey, onClose, onSaved }: { share: OwnerShare; folder: boolean; driveKey: Uint8Array; onClose: () => void; onSaved: () => void }) {
  const d = share.dto;
  const [label, setLabel] = useState(share.box?.label ?? "");
  const [permission, setPermission] = useState(d.permission);
  const [expiry, setExpiry] = useState<ExpiryChoice | "keep">(d.expiresAt ? "keep" : "never");
  const [custom, setCustom] = useState(localInputValue(d.expiresAt ? new Date(d.expiresAt) : new Date(Date.now() + 7 * 24 * HOUR)));
  const [maxDownloads, setMaxDownloads] = useState(d.maxDownloads ? String(d.maxDownloads) : "");
  const [size, setSize] = useState(d.upload.maxFileSize ? String(+(d.upload.maxFileSize / MB).toFixed(2)) : "");
  const [allowance, setAllowance] = useState(d.upload.allowance ? String(+(d.upload.allowance / MB).toFixed(2)) : "");
  const [files, setFiles] = useState(d.upload.maxFiles ? String(d.upload.maxFiles) : "");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(e: FormEvent) {
    e.preventDefault();
    setError(null);
    const input: api.ShareSettingsInput = { permission, maxDownloads: positiveOrNull(maxDownloads) };
    if (expiry !== "keep") {
      const date = expiryDate(expiry, custom);
      if (expiry === "custom" && (!date || date.getTime() <= Date.now())) {
        setError("Pick a date and time in the future.");
        return;
      }
      input.expiresAt = date?.toISOString() ?? null;
    }
    if (permission === "contributor") input.upload = { maxFileSize: positiveOrNull(size, MB), allowance: positiveOrNull(allowance, MB), maxFiles: positiveOrNull(files) };
    setBusy(true);
    try {
      await api.updateShareLink(d.id, input);
      if (share.box && label.trim() !== share.box.label) await relabelShare(share, label, driveKey);
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't save. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Modal title="Link settings" onClose={onClose} wide>
      <form className={styles.form} onSubmit={save}>
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Name (only you see it)</span>
          <input className={styles.input} value={label} maxLength={120} onChange={(e) => setLabel(e.target.value)} />
        </label>
        <PermissionField value={permission} onChange={setPermission} folder={folder} />
        {d.permission === "contributor" && permission === "viewer" && (
          <p className={styles.note}>
            <InfoIcon size={16} /> Uploads stop at once: any still in progress are cancelled. Files already received stay in Incoming.
          </p>
        )}
        {permission === "contributor" && (
          <UploadLimits size={size} setSize={setSize} allowance={allowance} setAllowance={setAllowance} files={files} setFiles={setFiles} />
        )}
        <div className={styles.fieldRow}>
          <label className={styles.field}>
            <span className={styles.fieldLabel}>Expires</span>
            <select className={styles.input} value={expiry} onChange={(e) => setExpiry(e.target.value as ExpiryChoice | "keep")}>
              {d.expiresAt && <option value="keep">Keep: {new Date(d.expiresAt).toLocaleString()}</option>}
              <option value="never">Never</option>
              <option value="1h">In 1 hour</option>
              <option value="1d">In 1 day</option>
              <option value="7d">In 7 days</option>
              <option value="30d">In 30 days</option>
              <option value="custom">On a date…</option>
            </select>
          </label>
          {expiry === "custom" && (
            <label className={styles.field}>
              <span className={styles.fieldLabel}>Date and time (your time zone)</span>
              <input className={styles.input} type="datetime-local" value={custom} onChange={(e) => setCustom(e.target.value)} />
            </label>
          )}
        </div>
        <label className={styles.field}>
          <span className={styles.fieldLabel}>Download limit</span>
          <input className={styles.input} inputMode="numeric" placeholder="No limit" value={maxDownloads} onChange={(e) => setMaxDownloads(e.target.value)} />
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
            {busy ? "Saving…" : "Save"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

/** Set, change or remove the password. The link itself stays the same. */
export function PasswordDialog({ share, driveKey, onClose, onSaved }: { share: OwnerShare; driveKey: Uint8Array; onClose: () => void; onSaved: () => void }) {
  const has = share.dto.password !== null;
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function apply(value: string | null) {
    setError(null);
    if (value !== null && value.length < 8) {
      setError("Use a password of at least 8 characters.");
      return;
    }
    setBusy(true);
    try {
      await setSharePassword(share, value, driveKey);
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't change the password. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <Modal title={has ? "Change the password" : "Add a password"} onClose={onClose}>
      <form
        className={styles.form}
        onSubmit={(e) => {
          e.preventDefault();
          void apply(password);
        }}
      >
        <p className={styles.dialogText}>Anyone opening the link will need it. People who already downloaded files keep them.</p>
        <input
          className={styles.input}
          type="password"
          autoComplete="new-password"
          placeholder="At least 8 characters"
          aria-label="New password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
        />
        <p className={styles.note}>
          <InfoIcon size={16} /> {PASSWORD_NOTE}
        </p>
        {error && (
          <p className={styles.error} role="alert">
            {error}
          </p>
        )}
        <div className={styles.dialogActions}>
          {has && (
            <button type="button" className={styles.danger} disabled={busy} onClick={() => void apply(null)}>
              Remove password
            </button>
          )}
          <button type="button" className={styles.secondary} onClick={onClose}>
            Cancel
          </button>
          <button type="submit" className={styles.primary} disabled={busy || password === ""}>
            {busy ? "Saving…" : "Save"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

/** A new address and key for the same item: the old link stops working. */
export function RegenerateDialog({
  share,
  entries,
  driveKey,
  onClose,
  onSaved,
}: {
  share: OwnerShare;
  entries: DriveEntry[];
  driveKey: Uint8Array;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [usePassword, setUsePassword] = useState(share.dto.password !== null);
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [made, setMade] = useState<OwnerShare | null>(null);

  async function regenerate(e: FormEvent) {
    e.preventDefault();
    setError(null);
    if (usePassword && password.length < 8) {
      setError("Use a password of at least 8 characters.");
      return;
    }
    setBusy(true);
    try {
      setMade(await regenerateShare(share, usePassword ? password : null, entries, driveKey));
      onSaved();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Couldn't make a new link. Try again.");
    } finally {
      setBusy(false);
    }
  }

  if (made?.url) {
    return (
      <Modal title="New link ready" onClose={onClose}>
        <p className={styles.dialogText}>The old link no longer works. Send this one to the people who should still have access.</p>
        <LinkResult url={made.url} hasPassword={made.dto.password !== null} onDone={onClose} />
      </Modal>
    );
  }

  return (
    <Modal title="Make a new link" onClose={onClose}>
      <form className={styles.form} onSubmit={regenerate}>
        <p className={styles.dialogText}>
          The current link stops working at once, including downloads in progress. Its settings and counts stay. Files people already downloaded stay with them.
        </p>
        <div className={styles.toggleRow}>
          <span>
            <strong>Password</strong>
            <span className={styles.choiceText}>{share.dto.password ? "Type it again (or a new one): it isn't stored anywhere." : "Needed to open the new link."}</span>
          </span>
          <ToggleSwitch checked={usePassword} onChange={setUsePassword} label="Require a password" />
        </div>
        {usePassword && (
          <input
            className={styles.input}
            type="password"
            autoComplete="new-password"
            placeholder="At least 8 characters"
            aria-label="Password for the new link"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        )}
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
            {busy ? "Encrypting…" : "Make new link"}
          </button>
        </div>
      </form>
    </Modal>
  );
}

export function ConfirmDialog({
  title,
  text,
  confirm,
  danger,
  onConfirm,
  onClose,
}: {
  title: string;
  text: string;
  confirm: string;
  danger?: boolean;
  onConfirm: () => Promise<void> | void;
  onClose: () => void;
}) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  return (
    <Modal title={title} onClose={onClose}>
      <p className={styles.dialogText}>{text}</p>
      {error && (
        <p className={styles.error} role="alert">
          {error}
        </p>
      )}
      <div className={styles.dialogActions}>
        <button type="button" className={styles.secondary} onClick={onClose}>
          Cancel
        </button>
        <button
          type="button"
          className={danger ? styles.dangerSolid : styles.primary}
          disabled={busy}
          onClick={async () => {
            setBusy(true);
            setError(null);
            try {
              await onConfirm();
              onClose();
            } catch (err) {
              setError(err instanceof Error ? err.message : "That didn't work. Try again.");
            } finally {
              setBusy(false);
            }
          }}
        >
          {confirm}
        </button>
      </div>
    </Modal>
  );
}
