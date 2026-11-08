// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type FormEvent, type ReactNode, useEffect, useMemo, useRef, useState } from "react";
import {
  AlertTriangleIcon,
  CheckIcon,
  ChevronLeftIcon,
  CopyIcon,
  InfoIcon,
  LockIcon,
  PlusIcon,
  RefreshIcon,
  SearchIcon,
  ShareIcon,
  UserIcon,
} from "../components/icons24.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import { getUser } from "../lib/api.js";
import { hasKeyChanged } from "../lib/verifiedContacts.js";
import { avatarTint } from "../lib/avatarTint.js";
import { fromBase64 } from "../lib/e2eCrypto.js";
import {
  canUnlockWithPassword,
  DEFAULT_GENERATOR,
  estimateStrength,
  firstUrl,
  generatePassword,
  type GeneratorOptions,
  hostOf,
  parseTotp,
  secretOf,
  totpCode,
  totpSecondsLeft,
  type VaultData,
  type VaultEntry,
  type VaultItemType,
} from "../lib/passwordVault.js";
import {
  breachOf,
  deleteVaultEntry,
  type KeyLookup,
  lockVault,
  saveVaultItem,
  shareVaultEntry,
  unlockVault,
  unshareVaultEntry,
  useVault,
  type VaultKeys,
  VaultConflictError,
} from "../lib/vaultStore.js";
import { useAppState } from "../state/AppStateContext.js";
import type { DMConversation } from "../types.js";
import styles from "./Passwords.module.css";

type Category = "all" | "login" | "passkey" | "card" | "note" | "wifi" | "shared" | "exposed" | "reused" | "weak";
type Status = "exposed" | "reused" | "weak" | null;

const CATEGORIES: { key: Category; label: string }[] = [
  { key: "all", label: "All items" },
  { key: "login", label: "Logins" },
  { key: "passkey", label: "Passkeys" },
  { key: "card", label: "Payment cards" },
  { key: "note", label: "Secure notes" },
  { key: "wifi", label: "Wi-Fi" },
  { key: "shared", label: "Shared with me" },
];

const TYPE_LABELS: Record<VaultItemType, string> = { login: "Login", passkey: "Passkey", card: "Payment card", note: "Secure note", wifi: "Wi-Fi" };
const CLIPBOARD_CLEAR_MS = 30_000;

// --- Clipboard: cleared after 30 seconds ---

let clipboardToken = 0;
async function copySecret(text: string): Promise<boolean> {
  try {
    await navigator.clipboard.writeText(text);
    const mine = ++clipboardToken;
    setTimeout(() => {
      if (mine === clipboardToken) void navigator.clipboard.writeText("").catch(() => {});
    }, CLIPBOARD_CLEAR_MS);
    return true;
  } catch {
    return false;
  }
}

// --- Derived rows ---

interface Row {
  entry: VaultEntry;
  status: Status;
}

function rowsFor(entries: VaultEntry[]): Row[] {
  const uses = new Map<string, number>();
  for (const e of entries) {
    const secret = secretOf(e.data);
    if (secret && !e.incoming) uses.set(secret, (uses.get(secret) ?? 0) + 1);
  }
  return entries.map((entry) => {
    const secret = secretOf(entry.data);
    let status: Status = null;
    if (secret) {
      const breach = breachOf(entry.data);
      if (breach?.state === "done" && breach.count > 0) status = "exposed";
      else if (!entry.incoming && (uses.get(secret) ?? 0) > 1) status = "reused";
      else if (estimateStrength(secret).strength === "Weak") status = "weak";
    }
    return { entry, status };
  });
}

function typeOf(data: VaultData): string {
  return typeof data.type === "string" ? data.type : "login";
}

function subtitle(entry: VaultEntry, handleFor: (id: string) => string): string {
  const d = entry.data;
  const type = typeOf(d);
  if (entry.incoming) return `Shared by ${handleFor(entry.ownerId ?? "")}`;
  if (type === "card") {
    const n = d.card?.number ?? "";
    return `Expires ${d.card?.expiry || "—"} · Card ending ${n.slice(-4) || "—"}`;
  }
  if (type === "wifi") return `${d.wifi?.security || "Wi-Fi"} · Network: ${d.wifi?.ssid || "—"}`;
  if (type === "note") return "Secure note";
  if (type === "passkey") return `Passkey · ${d.passkey?.rpId ?? ""}`;
  return [d.username, hostOf(firstUrl(d))].filter(Boolean).join(" · ") || "Login";
}

function matchesCategory(row: Row, category: Category): boolean {
  const type = typeOf(row.entry.data);
  switch (category) {
    case "all":
      return true;
    case "shared":
      return row.entry.incoming;
    case "passkey":
      return !row.entry.incoming && (type === "passkey" || !!row.entry.data.passkey?.rpId);
    case "exposed":
    case "reused":
    case "weak":
      return row.status === category;
    default:
      return !row.entry.incoming && type === category;
  }
}

// --- Screen ---

export function Passwords() {
  const { state } = useAppState();
  const identity = state.identity;
  const vault = useVault();
  const [category, setCategory] = useState<Category>("all");
  const [search, setSearch] = useState("");
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [editing, setEditing] = useState<{ entry: VaultEntry | null; data: VaultData } | null>(null);
  const [sharing, setSharing] = useState<VaultEntry | null>(null);
  const [mobileDetail, setMobileDetail] = useState(false);

  const contacts = useMemo(
    () => state.conversations.filter((c): c is DMConversation => c.kind === "dm" && !!c.userId && !!c.publicKey),
    [state.conversations]
  );
  const handleFor = (userId: string) => {
    const c = contacts.find((x) => x.userId === userId);
    return c ? `@${c.handle}` : "a contact";
  };
  const lookup: KeyLookup = async (userId) => {
    const c = contacts.find((x) => x.userId === userId);
    if (c?.keyChanged) return null; // Their copy waits until the new safety number is checked.
    if (c?.publicKey) return c.publicKey;
    try {
      const user = await getUser(userId);
      return hasKeyChanged(keys.userId, userId, user.publicKey) ? null : fromBase64(user.publicKey);
    } catch {
      return null;
    }
  };

  const rows = useMemo(() => rowsFor(vault.entries), [vault.entries, vault.breaches]);
  const counts = useMemo(() => {
    const out = {} as Record<Category, number>;
    for (const c of [...CATEGORIES.map((x) => x.key), "exposed", "reused", "weak"] as Category[]) out[c] = rows.filter((r) => matchesCategory(r, c)).length;
    return out;
  }, [rows]);
  const q = search.trim().toLowerCase();
  const visible = rows
    .filter((r) => matchesCategory(r, category))
    .filter((r) => !q || [r.entry.data.title, r.entry.data.username, firstUrl(r.entry.data), r.entry.data.wifi?.ssid].some((v) => v?.toLowerCase().includes(q)))
    .sort((a, b) => (a.entry.data.title ?? "").localeCompare(b.entry.data.title ?? "", undefined, { sensitivity: "base" }));
  const selected = rows.find((r) => r.entry.id === selectedId && (r.entry.incoming ? true : !r.entry.incoming)) ?? visible[0] ?? null;

  if (!identity) return null;
  const keys: VaultKeys = { userId: identity.userId, publicKey: identity.publicKey, privateKey: identity.privateKey };
  const unlocked = vault.status === "unlocked";

  return (
    <div className={`${styles.shell} ${mobileDetail ? styles.showDetail : ""}`}>
      <nav className={styles.sidebar} aria-label="Password categories">
        <h1 className={styles.title}>Passwords</h1>
        <div className={`${styles.lockCard} ${unlocked ? styles.lockCardOn : ""}`}>
          <LockIcon size={16} />
          <span>
            <strong>{unlocked ? "Unlocked" : "Locked"}</strong>
            <span>{unlocked ? "Locks after 5 min idle" : "Unlock to view items"}</span>
          </span>
          {unlocked ? (
            <button type="button" onClick={lockVault}>
              Lock now
            </button>
          ) : (
            <button type="button" onClick={() => document.getElementById("vault-password")?.focus()}>
              Unlock
            </button>
          )}
        </div>
        <div className={styles.cats}>
          {CATEGORIES.map((c) => (
            <CategoryButton key={c.key} label={c.label} count={unlocked ? counts[c.key] : null} active={category === c.key} onClick={() => setCategory(c.key)} />
          ))}
          <span className={styles.catHead}>Security check</span>
          <CategoryButton
            label="Exposed in a breach"
            tone="danger"
            icon={<AlertTriangleIcon size={16} />}
            count={unlocked ? counts.exposed : null}
            active={category === "exposed"}
            onClick={() => setCategory("exposed")}
          />
          <CategoryButton label="Reused" tone="warn" icon={<RefreshIcon size={16} />} count={unlocked ? counts.reused : null} active={category === "reused"} onClick={() => setCategory("reused")} />
          <CategoryButton label="Weak" tone="warn" icon={<InfoIcon size={16} />} count={unlocked ? counts.weak : null} active={category === "weak"} onClick={() => setCategory("weak")} />
        </div>
        <div className={styles.syncCard}>
          <strong>Unlimited items and devices</strong>
          <span>The same vault as your desktop app. It's encrypted with a key only you hold.</span>
        </div>
      </nav>

      {!unlocked ? (
        <LockedView keys={keys} loading={vault.status === "loading"} error={vault.error} />
      ) : (
        <>
          <section className={styles.list} aria-label="Vault items">
            {vault.refused > 0 && (
              <p className={styles.error} role="alert">
                {vault.refused === 1 ? "One item isn't shown" : `${vault.refused} items aren't shown`}: they don't match what this device last saw, so they may
                have been changed on the server.
              </p>
            )}
            <div className={styles.listHead}>
              <label className={styles.search}>
                <SearchIcon size={16} />
                <input type="search" placeholder="Search vault" value={search} onChange={(e) => setSearch(e.target.value)} aria-label="Search vault" />
              </label>
              <button type="button" className={styles.add} aria-label="Add an item" onClick={() => setEditing({ entry: null, data: { type: "login" } })}>
                <PlusIcon />
              </button>
            </div>
            <ul className={styles.rows}>
              {visible.length === 0 && <li className={styles.empty}>{rows.length === 0 ? "Your vault is empty. Add a login with +." : "Nothing here."}</li>}
              {visible.map((r) => {
                const d = r.entry.data;
                const on = selected?.entry === r.entry;
                return (
                  <li key={`${r.entry.incoming ? "in" : "own"}-${r.entry.id}`}>
                    <button
                      type="button"
                      className={`${styles.row} ${on ? styles.rowOn : ""}`}
                      aria-current={on ? "true" : undefined}
                      onClick={() => {
                        setSelectedId(r.entry.id);
                        setMobileDetail(true);
                      }}
                    >
                      <Tile title={d.title ?? "?"} />
                      <span className={styles.rowText}>
                        <strong>{d.title || "Untitled"}</strong>
                        <span>{subtitle(r.entry, handleFor)}</span>
                      </span>
                      {r.status && <StatusTag status={r.status} />}
                    </button>
                  </li>
                );
              })}
            </ul>
          </section>
          {selected ? (
            <Detail
              key={`${selected.entry.incoming ? "in" : "own"}-${selected.entry.id}`}
              row={selected}
              rows={rows}
              handleFor={handleFor}
              onBack={() => setMobileDetail(false)}
              onEdit={(data) => setEditing({ entry: selected.entry, data: data ?? selected.entry.data })}
              onShare={() => setSharing(selected.entry)}
            />
          ) : (
            <section className={styles.detailEmpty}>
              <p>Select an item, or add one with +.</p>
            </section>
          )}
        </>
      )}

      {editing && unlocked && (
        <Editor
          entry={editing.entry}
          initial={editing.data}
          onClose={() => setEditing(null)}
          onSave={async (data) => {
            const saved = await saveVaultItem(keys, editing.entry, data, lookup);
            setSelectedId(saved.id);
            setEditing(null);
          }}
          onDelete={
            editing.entry
              ? async () => {
                  await deleteVaultEntry(editing.entry!.id);
                  setSelectedId(null);
                  setEditing(null);
                }
              : undefined
          }
        />
      )}
      {sharing && unlocked && (
        <ShareDialog
          entry={vault.entries.find((e) => e.id === sharing.id && !e.incoming) ?? sharing}
          contacts={contacts}
          onClose={() => setSharing(null)}
        />
      )}
    </div>
  );
}

function CategoryButton({ label, count, active, onClick, icon, tone }: { label: string; count: number | null; active: boolean; onClick: () => void; icon?: ReactNode; tone?: "danger" | "warn" }) {
  return (
    <button type="button" className={`${styles.cat} ${active ? styles.catOn : ""} ${tone ? styles[tone] : ""}`} aria-current={active ? "true" : undefined} onClick={onClick}>
      {icon}
      <span>{label}</span>
      {count !== null && <span className={styles.count}>{count}</span>}
    </button>
  );
}

function Tile({ title, size = 36 }: { title: string; size?: number }) {
  return (
    <span className={styles.tile} style={{ width: size, height: size, background: avatarTint(title), fontSize: size > 40 ? 22 : 14 }} aria-hidden="true">
      {(title.trim()[0] ?? "?").toUpperCase()}
    </span>
  );
}

function StatusTag({ status }: { status: Exclude<Status, null> }) {
  return <span className={`${styles.tag} ${status === "exposed" ? styles.tagDanger : styles.tagWarn}`}>{status === "exposed" ? "Exposed" : status === "reused" ? "Reused" : "Weak"}</span>;
}

// --- Locked ---

function LockedView({ keys, loading, error }: { keys: VaultKeys; loading: boolean; error: string | null }) {
  const [password, setPassword] = useState("");
  const [checking, setChecking] = useState(false);
  const available = canUnlockWithPassword();
  async function submit(e: FormEvent) {
    e.preventDefault();
    setChecking(true);
    // Let the button show "Unlocking…" before the (deliberately slow) key derivation runs.
    await new Promise((r) => setTimeout(r, 0));
    const ok = await unlockVault(keys, password);
    setChecking(false);
    if (ok) setPassword("");
  }
  return (
    <section className={styles.locked} aria-label="Vault locked">
      <span className={styles.lockedIcon}>
        <LockIcon size={30} />
      </span>
      <h2>Your vault is locked</h2>
      <p>Unlock to see your passwords. Nothing in the vault can be read until you do — not even by us.</p>
      {available ? (
        <form className={styles.unlockForm} onSubmit={(e) => void submit(e)}>
          <input
            id="vault-password"
            type="password"
            autoComplete="current-password"
            placeholder="Account password"
            aria-label="Account password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoFocus
          />
          <button type="submit" className={styles.primary} disabled={!password || checking || loading}>
            {checking || loading ? "Unlocking…" : "Unlock"}
          </button>
          {error && (
            <p className={styles.error} role="alert">
              {error}
            </p>
          )}
        </form>
      ) : (
        <p className={styles.error} role="alert">
          Sign out and sign in again to unlock the vault in this browser.
        </p>
      )}
    </section>
  );
}

// --- Detail ---

function Field({ label, value, mono = true, secret, onReveal, revealed, copy, large, children }: { label: string; value: string; mono?: boolean; secret?: boolean; onReveal?: () => void; revealed?: boolean; copy?: string; large?: boolean; children?: ReactNode }) {
  const [copied, setCopied] = useState(false);
  return (
    <div className={styles.field}>
      <div className={styles.fieldMain}>
        <span className={styles.fieldLabel}>{label}</span>
        <span className={`${mono ? styles.mono : ""} ${styles.fieldValue} ${large ? styles.large : ""}`}>{secret && !revealed ? "•".repeat(Math.min(24, Math.max(8, value.length))) : value || "—"}</span>
        {children}
      </div>
      <div className={styles.fieldActions}>
        {secret && onReveal && (
          <button type="button" className={styles.iconButton} aria-label={revealed ? `Hide ${label.toLowerCase()}` : `Show ${label.toLowerCase()}`} aria-pressed={revealed} onClick={onReveal}>
            <EyeIcon off={revealed} />
          </button>
        )}
        {copy !== undefined && copy !== "" && (
          <button
            type="button"
            className={styles.iconButton}
            aria-label={`Copy ${label.toLowerCase()}`}
            onClick={() =>
              void copySecret(copy).then((ok) => {
                setCopied(ok);
                if (ok) setTimeout(() => setCopied(false), 1500);
              })
            }
          >
            {copied ? <CheckIcon size={17} /> : <CopyIcon size={17} />}
          </button>
        )}
      </div>
    </div>
  );
}

function EyeIcon({ off }: { off?: boolean }) {
  return (
    <svg viewBox="0 0 24 24" width={18} height={18} fill="none" stroke="currentColor" strokeWidth={1.9} strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z" />
      <circle cx="12" cy="12" r="3" />
      {off && <path d="M3 3l18 18" />}
    </svg>
  );
}

function StrengthBar({ password }: { password: string }) {
  const { strength } = estimateStrength(password);
  const tone = strength === "Strong" ? styles.good : strength === "Fair" ? styles.fair : styles.bad;
  const pct = strength === "Strong" ? 100 : strength === "Fair" ? 60 : 28;
  return (
    <span className={styles.strength}>
      <span className={styles.strengthTrack}>
        <span className={tone} style={{ width: `${pct}%` }} />
      </span>
      <span className={`${styles.strengthLabel} ${tone}`}>
        {strength} · {[...password].length} characters
      </span>
    </span>
  );
}

function Totp({ secret }: { secret: string }) {
  const params = useMemo(() => parseTotp(secret), [secret]);
  const [now, setNow] = useState(() => Date.now() / 1000);
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now() / 1000), 1000);
    return () => clearInterval(t);
  }, []);
  if (!params) return <Field label="One-time code" value="Invalid setup key" mono={false} />;
  const code = totpCode(params, now);
  const left = totpSecondsLeft(params, now);
  const r = 14;
  const c = 2 * Math.PI * r;
  return (
    <Field label="One-time code" value={code.replace(/^(\d{3})(\d+)$/, "$1 $2")} copy={code} large>
      <span className={styles.ring} aria-label={`${left} seconds left`}>
        <svg viewBox="0 0 36 36" width={36} height={36} aria-hidden="true">
          <circle cx="18" cy="18" r={r} className={styles.ringTrack} />
          <circle cx="18" cy="18" r={r} className={styles.ringFill} strokeDasharray={c} strokeDashoffset={c * (1 - left / params.period)} transform="rotate(-90 18 18)" />
        </svg>
        <span>{left}</span>
      </span>
    </Field>
  );
}

function lastChanged(data: VaultData): string {
  const t = typeof data.passwordChanged === "number" ? data.passwordChanged : typeof data.updated === "number" ? data.updated : 0;
  if (!t) return "—";
  const d = new Date(t);
  return d.getFullYear() === new Date().getFullYear() ? d.toLocaleDateString(undefined, { month: "short", day: "numeric" }) : String(d.getFullYear());
}

function Detail({
  row,
  rows,
  handleFor,
  onBack,
  onEdit,
  onShare,
}: {
  row: Row;
  rows: Row[];
  handleFor: (id: string) => string;
  onBack: () => void;
  onEdit: (data?: VaultData) => void;
  onShare: () => void;
}) {
  const vault = useVault();
  const { entry, status } = row;
  const d = entry.data;
  const type = typeOf(d);
  const [reveal, setReveal] = useState<Record<string, boolean>>({});
  const flip = (k: string) => setReveal((r) => ({ ...r, [k]: !r[k] }));
  const url = firstUrl(d);
  const secret = secretOf(d);
  const reusedWith = secret ? rows.filter((r) => r !== row && !r.entry.incoming && secretOf(r.entry.data) === secret).length : 0;
  const breach = breachOf(d);
  const breachText = !secret
    ? "No password"
    : vault.breachCheckOff
      ? "Off on this server"
      : !breach || breach.state === "checking"
        ? "Checking privately…"
        : breach.state === "error"
          ? "Couldn't check"
          : breach.count > 0
            ? `Found in ${breach.count.toLocaleString()} breaches`
            : "Checked privately";

  return (
    <section className={styles.detail} aria-label={d.title || "Item"}>
      <div className={styles.detailHead}>
        <button type="button" className={styles.back} onClick={onBack} aria-label="Back to the list">
          <ChevronLeftIcon />
        </button>
        <Tile title={d.title ?? "?"} size={56} />
        <span className={styles.detailTitle}>
          <h2>{d.title || "Untitled"}</h2>
          <span className={styles.mono}>{hostOf(url) || TYPE_LABELS[type as VaultItemType] || "Item"}</span>
        </span>
        {entry.incoming ? (
          <span className={styles.readOnly}>Shared by {handleFor(entry.ownerId ?? "")} · read-only</span>
        ) : (
          <span className={styles.headActions}>
            <button type="button" className={styles.secondary} onClick={() => onEdit()}>
              Edit
            </button>
            <button type="button" className={styles.secondary} onClick={onShare}>
              <ShareIcon size={16} /> Share securely
            </button>
          </span>
        )}
      </div>

      {status === "exposed" && (
        <div className={styles.bannerDanger} role="alert">
          <AlertTriangleIcon size={20} />
          <span>This password appeared in a known data breach. Change it on the site, then update it here.</span>
          {!entry.incoming && (
            <button type="button" className={styles.dangerButton} onClick={() => onEdit()}>
              Change password
            </button>
          )}
        </div>
      )}
      {status === "reused" && (
        <div className={styles.bannerWarn}>
          <RefreshIcon size={20} />
          <span>
            This password is also used for {reusedWith} other item{reusedWith === 1 ? "" : "s"}. If one site leaks it, the others are exposed too.
          </span>
          <button type="button" className={styles.warnButton} onClick={() => onEdit()}>
            Change password
          </button>
        </div>
      )}

      <div className={styles.detailBody}>
        <div className={styles.fields}>
          {(type === "login" || type === "passkey") && (
            <>
              <Field label="Username" value={d.username ?? ""} copy={d.username ?? ""} />
              {type === "login" && (
                <Field label="Password" value={d.password ?? ""} secret revealed={reveal.password} onReveal={() => flip("password")} copy={d.password ?? ""}>
                  {d.password && (
                    <>
                      <StrengthBar password={d.password} />
                      <span className={styles.hint}>Copied passwords clear from the clipboard after 30 seconds.</span>
                    </>
                  )}
                </Field>
              )}
              {d.totp && <Totp secret={d.totp} />}
              {d.passkey?.rpId && (
                <div className={styles.field}>
                  <UserIcon size={18} />
                  <div className={styles.fieldMain}>
                    <strong>Passkey saved</strong>
                    <span className={styles.hint}>
                      Sign in without a password on {d.passkey.rpId}. Use it from the desktop app or your device's passkey manager.
                    </span>
                  </div>
                </div>
              )}
              {url && (
                <div className={styles.field}>
                  <div className={styles.fieldMain}>
                    <span className={styles.fieldLabel}>Website</span>
                    <a className={styles.link} href={/^https?:\/\//i.test(url) ? url : `https://${url}`} target="_blank" rel="noopener noreferrer">
                      {url}
                    </a>
                  </div>
                </div>
              )}
            </>
          )}
          {type === "card" && (
            <>
              <Field label="Cardholder" value={d.card?.holder ?? ""} mono={false} copy={d.card?.holder ?? ""} />
              <Field label="Card number" value={d.card?.number ?? ""} secret revealed={reveal.number} onReveal={() => flip("number")} copy={(d.card?.number ?? "").replace(/\s/g, "")} />
              <Field label="Expires" value={d.card?.expiry ?? ""} />
              <Field label="Security code" value={d.card?.cvv ?? ""} secret revealed={reveal.cvv} onReveal={() => flip("cvv")} copy={d.card?.cvv ?? ""} />
            </>
          )}
          {type === "wifi" && (
            <>
              <Field label="Network" value={d.wifi?.ssid ?? ""} copy={d.wifi?.ssid ?? ""} />
              <Field label="Password" value={d.wifi?.password ?? ""} secret revealed={reveal.wifi} onReveal={() => flip("wifi")} copy={d.wifi?.password ?? ""}>
                {d.wifi?.password && <StrengthBar password={d.wifi.password} />}
              </Field>
              <Field label="Security" value={d.wifi?.security ?? ""} mono={false} />
            </>
          )}
          {d.notes && (
            <div className={styles.field}>
              <div className={styles.fieldMain}>
                <span className={styles.fieldLabel}>{type === "note" ? "Note" : "Notes"}</span>
                <p className={styles.notes}>{d.notes}</p>
              </div>
            </div>
          )}
        </div>

        <div className={styles.side}>
          {!entry.incoming && (type === "login" || type === "wifi") && (
            <Generator
              onUse={(password) => onEdit(type === "wifi" ? { ...d, wifi: { ...d.wifi, password } } : { ...d, password })}
            />
          )}
          <dl className={styles.meta}>
            <div>
              <dt>Shared with</dt>
              <dd>{entry.incoming ? "You (read-only copy)" : entry.sharedWith.length ? entry.sharedWith.map(handleFor).join(", ") : "Only you"}</dd>
            </div>
            <div>
              <dt>Last changed</dt>
              <dd>{lastChanged(d)}</dd>
            </div>
            <div>
              <dt>Breach check</dt>
              <dd className={breach?.state === "done" && breach.count > 0 ? styles.dangerText : secret && breachText === "Checked privately" ? styles.goodText : undefined}>{breachText}</dd>
            </div>
          </dl>
        </div>
      </div>
    </section>
  );
}

function Generator({ onUse }: { onUse?: (password: string) => void }) {
  const [options, setOptions] = useState<GeneratorOptions>(DEFAULT_GENERATOR);
  const [value, setValue] = useState(() => generatePassword(DEFAULT_GENERATOR));
  const update = (next: GeneratorOptions) => {
    setOptions(next);
    setValue(generatePassword(next));
  };
  const toggle = (key: "upper" | "digits" | "symbols" | "words", label: string) => (
    <button type="button" className={`${styles.toggle} ${options[key] ? styles.toggleOn : ""}`} aria-pressed={options[key]} onClick={() => update({ ...options, [key]: !options[key] })}>
      {label}
    </button>
  );
  const max = options.words ? 12 : 40;
  const min = options.words ? 4 : 8;
  return (
    <div className={styles.generator} aria-label="Password generator" role="group">
      <div className={styles.genHead}>
        <strong>Password generator</strong>
        <button type="button" className={styles.iconButton} aria-label="Generate another" onClick={() => setValue(generatePassword(options))}>
          <RefreshIcon size={17} />
        </button>
      </div>
      <output className={styles.genValue}>{value}</output>
      <label className={styles.genLength}>
        <span>{options.words ? "Words" : "Length"}</span>
        <span className={styles.mono}>{Math.min(max, Math.max(min, options.length))}</span>
        <input
          type="range"
          min={min}
          max={max}
          value={Math.min(max, Math.max(min, options.length))}
          onChange={(e) => update({ ...options, length: Number(e.target.value) })}
          aria-label={options.words ? "Number of words" : "Length"}
        />
      </label>
      <div className={styles.toggles}>
        {toggle("upper", "A–Z")}
        {toggle("digits", "0–9")}
        {!options.words && toggle("symbols", "!@#")}
        <button
          type="button"
          className={`${styles.toggle} ${options.words ? styles.toggleOn : ""}`}
          aria-pressed={options.words}
          onClick={() => update({ ...options, words: !options.words, length: options.words ? 24 : 6 })}
        >
          Words
        </button>
      </div>
      {onUse ? (
        <button type="button" className={styles.primary} onClick={() => onUse(value)}>
          Use this password
        </button>
      ) : (
        <button type="button" className={styles.primary} onClick={() => void copySecret(value)}>
          Copy
        </button>
      )}
    </div>
  );
}

// --- Editor ---

function Editor({
  entry,
  initial,
  onClose,
  onSave,
  onDelete,
}: {
  entry: VaultEntry | null;
  initial: VaultData;
  onClose: () => void;
  onSave: (data: VaultData) => Promise<void>;
  onDelete?: () => Promise<void>;
}) {
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);
  const [data, setData] = useState<VaultData>(initial);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [showPassword, setShowPassword] = useState(initial.password !== entry?.data.password);
  const type = typeOf(data) as VaultItemType;
  const set = (patch: Partial<VaultData>) => setData((d) => ({ ...d, ...patch }));

  async function submit(e: FormEvent) {
    e.preventDefault();
    if (!data.title?.trim()) return setError("Give it a name.");
    if (data.totp?.trim() && !parseTotp(data.totp)) return setError("That one-time code setup key isn't valid. Paste the key or otpauth:// link from the site.");
    const out: VaultData = { ...data, title: data.title.trim(), username: data.username?.trim() ?? "", totp: data.totp?.trim() ?? "" };
    const url = (Array.isArray(data.urls) ? data.urls[0] : "")?.trim() ?? "";
    out.urls = url ? [url] : [];
    if (type !== "card") delete out.card;
    if (type !== "wifi") delete out.wifi;
    if (type !== "passkey" && !data.passkey?.rpId) delete out.passkey;
    if (secretOf(out) !== secretOf(entry?.data ?? {})) out.passwordChanged = Date.now();
    setBusy(true);
    setError(null);
    try {
      await onSave(out);
    } catch (err) {
      setError(err instanceof VaultConflictError ? err.message : "Couldn't save. Check your connection and try again.");
      setBusy(false);
    }
  }

  return (
    <div className={styles.backdrop} onClick={onClose}>
      <div ref={ref} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="editor-title" onClick={(e) => e.stopPropagation()}>
        <h2 id="editor-title">{entry ? `Edit ${entry.data.title || "item"}` : "New item"}</h2>
        <form className={styles.form} onSubmit={(e) => void submit(e)}>
          {!entry && (
            <div className={styles.typePicker} role="group" aria-label="Type">
              {(Object.keys(TYPE_LABELS) as VaultItemType[]).map((t) => (
                <button key={t} type="button" aria-pressed={type === t} className={`${styles.toggle} ${type === t ? styles.toggleOn : ""}`} onClick={() => set({ type: t })}>
                  {TYPE_LABELS[t]}
                </button>
              ))}
            </div>
          )}
          <label>
            Name
            <input value={data.title ?? ""} onChange={(e) => set({ title: e.target.value })} placeholder={type === "wifi" ? "Home Wi-Fi" : type === "card" ? "Personal card" : "Work email"} autoFocus />
          </label>
          {(type === "login" || type === "passkey") && (
            <>
              <label>
                Username
                <input value={data.username ?? ""} onChange={(e) => set({ username: e.target.value })} autoComplete="off" />
              </label>
              {type === "login" && (
                <label>
                  Password
                  <span className={styles.inputRow}>
                    <input
                      type={showPassword ? "text" : "password"}
                      value={data.password ?? ""}
                      onChange={(e) => set({ password: e.target.value })}
                      autoComplete="new-password"
                      className={styles.monoInput}
                    />
                    <button type="button" className={styles.iconButton} aria-label={showPassword ? "Hide password" : "Show password"} onClick={() => setShowPassword(!showPassword)}>
                      <EyeIcon off={showPassword} />
                    </button>
                    <button
                      type="button"
                      className={styles.iconButton}
                      aria-label="Generate a password"
                      onClick={() => {
                        set({ password: generatePassword(DEFAULT_GENERATOR) });
                        setShowPassword(true);
                      }}
                    >
                      <RefreshIcon size={17} />
                    </button>
                  </span>
                  {data.password && <StrengthBar password={data.password} />}
                </label>
              )}
              <label>
                Website
                <input value={(Array.isArray(data.urls) ? data.urls[0] : "") ?? ""} onChange={(e) => set({ urls: [e.target.value] })} placeholder="https://example.com" />
              </label>
              <label>
                One-time code setup key (optional)
                <input value={data.totp ?? ""} onChange={(e) => set({ totp: e.target.value })} placeholder="Key or otpauth:// link" className={styles.monoInput} autoComplete="off" />
              </label>
              {type === "passkey" && (
                <label>
                  Passkey site
                  <input value={data.passkey?.rpId ?? ""} onChange={(e) => set({ passkey: { ...data.passkey, rpId: e.target.value } })} placeholder="example.com" />
                </label>
              )}
            </>
          )}
          {type === "card" && (
            <>
              <label>
                Cardholder
                <input value={data.card?.holder ?? ""} onChange={(e) => set({ card: { ...data.card, holder: e.target.value } })} autoComplete="off" />
              </label>
              <label>
                Card number
                <input value={data.card?.number ?? ""} onChange={(e) => set({ card: { ...data.card, number: e.target.value } })} inputMode="numeric" autoComplete="off" className={styles.monoInput} />
              </label>
              <span className={styles.twoFields}>
                <label>
                  Expires
                  <input value={data.card?.expiry ?? ""} onChange={(e) => set({ card: { ...data.card, expiry: e.target.value } })} placeholder="MM/YY" autoComplete="off" />
                </label>
                <label>
                  Security code
                  <input value={data.card?.cvv ?? ""} onChange={(e) => set({ card: { ...data.card, cvv: e.target.value } })} inputMode="numeric" autoComplete="off" />
                </label>
              </span>
            </>
          )}
          {type === "wifi" && (
            <>
              <label>
                Network name
                <input value={data.wifi?.ssid ?? ""} onChange={(e) => set({ wifi: { ...data.wifi, ssid: e.target.value } })} autoComplete="off" />
              </label>
              <label>
                Password
                <span className={styles.inputRow}>
                  <input
                    type={showPassword ? "text" : "password"}
                    value={data.wifi?.password ?? ""}
                    onChange={(e) => set({ wifi: { ...data.wifi, password: e.target.value } })}
                    autoComplete="new-password"
                    className={styles.monoInput}
                  />
                  <button type="button" className={styles.iconButton} aria-label={showPassword ? "Hide password" : "Show password"} onClick={() => setShowPassword(!showPassword)}>
                    <EyeIcon off={showPassword} />
                  </button>
                </span>
              </label>
              <label>
                Security
                <select value={data.wifi?.security ?? "WPA2"} onChange={(e) => set({ wifi: { ...data.wifi, security: e.target.value } })}>
                  {["WPA3", "WPA2", "WPA", "WEP", "None"].map((s) => (
                    <option key={s}>{s}</option>
                  ))}
                </select>
              </label>
            </>
          )}
          <label>
            {type === "note" ? "Note" : "Notes"}
            <textarea rows={type === "note" ? 8 : 3} value={data.notes ?? ""} onChange={(e) => set({ notes: e.target.value })} />
          </label>
          {error && (
            <p className={styles.error} role="alert">
              {error}
            </p>
          )}
          <div className={styles.dialogActions}>
            {onDelete &&
              (confirmDelete ? (
                <button
                  type="button"
                  className={styles.dangerButton}
                  disabled={busy}
                  onClick={() => {
                    setBusy(true);
                    void onDelete().catch(() => {
                      setError("Couldn't delete. Try again.");
                      setBusy(false);
                    });
                  }}
                >
                  Delete from every device
                </button>
              ) : (
                <button type="button" className={styles.textDanger} onClick={() => setConfirmDelete(true)}>
                  Delete…
                </button>
              ))}
            <span className={styles.spacer} />
            <button type="button" className={styles.secondary} onClick={onClose}>
              Cancel
            </button>
            <button type="submit" className={styles.primary} disabled={busy}>
              {busy ? "Saving…" : "Save"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}

// --- Sharing ---

function ShareDialog({ entry, contacts, onClose }: { entry: VaultEntry; contacts: DMConversation[]; onClose: () => void }) {
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  async function toggle(c: DMConversation) {
    setBusy(c.userId!);
    setError(null);
    try {
      if (entry.sharedWith.includes(c.userId!)) await unshareVaultEntry(entry, c.userId!);
      else if (c.keyChanged) setError("Their safety number changed. Check it in their chat before sharing with them.");
      else await shareVaultEntry(entry, c.userId!, c.publicKey!);
    } catch {
      setError("Couldn't change sharing. Try again.");
    } finally {
      setBusy(null);
    }
  }
  return (
    <div className={styles.backdrop} onClick={onClose}>
      <div ref={ref} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="share-title" onClick={(e) => e.stopPropagation()}>
        <h2 id="share-title">Share “{entry.data.title || "item"}” securely</h2>
        <p className={styles.hint}>
          They get a read-only copy, encrypted so only they can open it. It updates when you edit the item, and stops when you stop sharing. Anyone you share a
          password with can keep a copy of it.
        </p>
        {contacts.length === 0 ? (
          <p className={styles.hint}>Add a contact first: items can be shared with people you've started a conversation with.</p>
        ) : (
          <ul className={styles.shareList}>
            {contacts.map((c) => {
              const on = entry.sharedWith.includes(c.userId!);
              return (
                <li key={c.userId}>
                  <Tile title={c.handle} size={32} />
                  <span>
                    @{c.handle}
                    {c.verified && <span className={styles.verified}> · verified</span>}
                  </span>
                  <button type="button" className={on ? styles.secondary : styles.primarySmall} disabled={busy !== null} onClick={() => void toggle(c)}>
                    {busy === c.userId ? "…" : on ? "Stop sharing" : "Share"}
                  </button>
                </li>
              );
            })}
          </ul>
        )}
        {error && (
          <p className={styles.error} role="alert">
            {error}
          </p>
        )}
        <div className={styles.dialogActions}>
          <span className={styles.spacer} />
          <button type="button" className={styles.secondary} onClick={onClose}>
            Done
          </button>
        </div>
      </div>
    </div>
  );
}
