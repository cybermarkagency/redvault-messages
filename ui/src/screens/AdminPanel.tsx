// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { LegalDocumentDialog } from "../components/LegalDocumentDialog.js";
import type { LegalDocumentId } from "../lib/legalDocuments.js";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import {
  adminCreateAccount,
  adminDeleteAccount,
  adminResolveReport,
  adminSetSubscriptionTier,
} from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { type SubscriptionTier, TIER_LABELS } from "../types.js";
import { MessagesTab } from "./AdminMessages.js";
import styles from "./AdminPanel.module.css";
import { AdminCommunities } from "./AdminCommunities.js";

type Tab = "users" | "reports" | "messages" | "communities";

/** Self-contained admin screen — Users/Reports as internal tabs (local state), matching how Settings.tsx already handles many concerns in one file. Reachable only from Sidebar's Admin tab, itself only rendered when identity.role === "super_admin". */
export function AdminPanel() {
  const { dispatch } = useAppState();
  const [tab, setTab] = useState<Tab>("users");

  return (
    <div className={styles.screen}>
      <TopBar title="Admin panel" onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
      <div className={styles.tabs}>
        <button
          type="button"
          className={`${styles.tab} ${tab === "users" ? styles.tabActive : ""}`}
          onClick={() => setTab("users")}
        >
          Users
        </button>
        <button
          type="button"
          className={`${styles.tab} ${tab === "reports" ? styles.tabActive : ""}`}
          onClick={() => setTab("reports")}
        >
          Reports
        </button>
        <button
          type="button"
          className={`${styles.tab} ${tab === "messages" ? styles.tabActive : ""}`}
          onClick={() => setTab("messages")}
        >
          Messages
        </button>
        <button
          type="button"
          className={`${styles.tab} ${tab === "communities" ? styles.tabActive : ""}`}
          onClick={() => setTab("communities")}
        >
          Communities
        </button>
      </div>
      {tab === "communities" ? (
        <AdminCommunities />
      ) : (
        <div className={styles.body}>{tab === "users" ? <UsersTab /> : tab === "reports" ? <ReportsTab /> : <MessagesTab />}</div>
      )}
    </div>
  );
}

function UsersTab() {
  const [users, setUsers] = useState<api.AdminUserDto[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);

  const [newHandle, setNewHandle] = useState("");
  const [newPassword, setNewPassword] = useState("");
  const [creating, setCreating] = useState(false);
  const [createError, setCreateError] = useState<string | null>(null);
  const [createdHandle, setCreatedHandle] = useState<string | null>(null);
  // Same three boxes as sign-up, ticked for the person the account is for.
  const [termsAccepted, setTermsAccepted] = useState(false);
  const [privacyAcknowledged, setPrivacyAcknowledged] = useState(false);
  const [age18Confirmed, setAge18Confirmed] = useState(false);
  const [reading, setReading] = useState<LegalDocumentId | null>(null);
  const allTicked = termsAccepted && privacyAcknowledged && age18Confirmed;

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const list = await api.adminListUsers({ limit: 100 });
      // The server already leaves deleted accounts out; this also covers an older server that still sends them.
      setUsers(list.filter((u) => !u.deletedAt));
    } catch {
      setError("Couldn't load users.");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
  }, []);

  async function handleCreate() {
    if (!newHandle.trim() || newPassword.length < 8 || !allTicked || creating) return;
    setCreating(true);
    setCreateError(null);
    try {
      await adminCreateAccount(newHandle.trim(), newPassword, { termsAccepted, privacyAcknowledged, age18Confirmed });
      setCreatedHandle(newHandle.trim());
      setNewHandle("");
      setNewPassword("");
      setTermsAccepted(false);
      setPrivacyAcknowledged(false);
      setAge18Confirmed(false);
      await load();
    } catch {
      setCreateError("Couldn't create that account — the handle may already be taken.");
    } finally {
      setCreating(false);
    }
  }

  async function handleDelete(userId: string) {
    if (!window.confirm("Delete this account? This can't be undone.")) return;
    setBusyId(userId);
    setError(null);
    try {
      await adminDeleteAccount(userId);
      await load();
    } catch {
      setError("Couldn't delete that account.");
    } finally {
      setBusyId(null);
    }
  }

  async function handleSetTier(userId: string, tier: SubscriptionTier) {
    setBusyId(userId);
    setError(null);
    try {
      await adminSetSubscriptionTier(userId, tier);
      await load();
    } catch {
      setError("Couldn't update that plan.");
    } finally {
      setBusyId(null);
    }
  }

  return (
    <>
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>CREATE ACCOUNT</span>
        {createdHandle && <p className={styles.success}>Created {createdHandle}.</p>}
        {createError && <p className={styles.error}>{createError}</p>}
        <input
          className={styles.input}
          placeholder="Handle"
          value={newHandle}
          onChange={(e) => setNewHandle(e.target.value)}
          autoCapitalize="none"
          autoCorrect="off"
        />
        <input
          className={styles.input}
          type="password"
          placeholder="Password (min. 8 characters)"
          value={newPassword}
          onChange={(e) => setNewPassword(e.target.value)}
        />
        <p className={`${styles.hint} type-secondary`}>Only for someone who has agreed to these:</p>
        <label className={styles.consentRow}>
          <input type="checkbox" checked={termsAccepted} onChange={(e) => setTermsAccepted(e.target.checked)} />
          <span>
            They agree to the RedVault Messages{" "}
            <button type="button" className={styles.consentLink} onClick={(e) => { e.preventDefault(); setReading("terms"); }}>
              Terms of Service
            </button>
          </span>
        </label>
        <label className={styles.consentRow}>
          <input type="checkbox" checked={privacyAcknowledged} onChange={(e) => setPrivacyAcknowledged(e.target.checked)} />
          <span>
            They acknowledge the RedVault Messages{" "}
            <button type="button" className={styles.consentLink} onClick={(e) => { e.preventDefault(); setReading("privacy"); }}>
              Privacy Policy
            </button>
          </span>
        </label>
        <label className={styles.consentRow}>
          <input type="checkbox" checked={age18Confirmed} onChange={(e) => setAge18Confirmed(e.target.checked)} />
          <span>They confirm they are 18 or older</span>
        </label>
        <button
          type="button"
          className={styles.primaryButton}
          disabled={creating || !newHandle.trim() || newPassword.length < 8 || !allTicked}
          onClick={handleCreate}
        >
          {creating ? "Creating…" : "Create account"}
        </button>
        {reading && <LegalDocumentDialog document={reading} onClose={() => setReading(null)} />}
      </div>

      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>USERS</span>
        {error && <p className={styles.error}>{error}</p>}
        {loading ? (
          <p className={`${styles.hint} type-secondary`}>Loading…</p>
        ) : users.length === 0 ? (
          <p className={`${styles.hint} type-secondary`}>No users.</p>
        ) : (
          users.map((u) => {
            const busy = busyId === u.userId;
            return (
              <div key={u.userId} className={styles.row}>
                <span>
                  <div className="type-row-primary">{u.username}</div>
                  <div className={`${styles.hint} type-secondary`}>
                    {u.role === "super_admin" ? "Super admin · " : ""}
                    {TIER_LABELS[u.subscriptionTier] ?? "Free"}
                  </div>
                </span>
                <div className={styles.rowActions}>
                  <select
                    className={styles.secondaryButton}
                    aria-label={`Plan for ${u.username}`}
                    disabled={busy}
                    value={u.subscriptionTier}
                    onChange={(e) => handleSetTier(u.userId, e.target.value as SubscriptionTier)}
                  >
                    {(["free", "pro", "premium"] as const).map((tier) => (
                      <option key={tier} value={tier}>
                        {TIER_LABELS[tier]}
                      </option>
                    ))}
                  </select>
                  <button
                    type="button"
                    className={styles.dangerButton}
                    disabled={busy}
                    onClick={() => handleDelete(u.userId)}
                  >
                    Delete
                  </button>
                </div>
              </div>
            );
          })
        )}
      </div>
    </>
  );
}

function ReportsTab() {
  const [status, setStatus] = useState<api.ReportStatus | "all">("open");
  const [reports, setReports] = useState<api.AdminReportDto[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);

  async function load() {
    setLoading(true);
    setError(null);
    try {
      const list = await api.adminListReports(status === "all" ? undefined : status);
      setReports(list);
    } catch {
      setError("Couldn't load reports.");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [status]);

  async function handleResolve(reportId: string, next: "reviewed" | "dismissed") {
    setBusyId(reportId);
    setError(null);
    try {
      await adminResolveReport(reportId, next);
      await load();
    } catch {
      setError("Couldn't update that report.");
    } finally {
      setBusyId(null);
    }
  }

  return (
    <div className={styles.section}>
      <span className={`${styles.sectionLabel} type-caption`}>REPORTS</span>
      <div className={styles.filterRow}>
        {(["open", "reviewed", "dismissed", "all"] as const).map((s) => (
          <button
            key={s}
            type="button"
            className={`${styles.filterButton} ${status === s ? styles.filterButtonActive : ""}`}
            onClick={() => setStatus(s)}
          >
            {s[0].toUpperCase() + s.slice(1)}
          </button>
        ))}
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {loading ? (
        <p className={`${styles.hint} type-secondary`}>Loading…</p>
      ) : reports.length === 0 ? (
        <p className={`${styles.hint} type-secondary`}>No reports.</p>
      ) : (
        reports.map((r) => {
          const busy = busyId === r.id;
          return (
            <div key={r.id} className={styles.reportCard}>
              <div className="type-row-primary">
                {r.reportedUsername ?? r.reportedId} reported by {r.reporterUsername ?? r.reporterId}
              </div>
              <p className={`${styles.hint} type-secondary`}>{r.reason}</p>
              <div className={`${styles.hint} type-secondary`}>Status: {r.status}</div>
              {r.status === "open" && (
                <div className={styles.rowActions}>
                  <button
                    type="button"
                    className={styles.secondaryButton}
                    disabled={busy}
                    onClick={() => handleResolve(r.id, "dismissed")}
                  >
                    Dismiss
                  </button>
                  <button
                    type="button"
                    className={styles.primaryButton}
                    disabled={busy}
                    onClick={() => handleResolve(r.id, "reviewed")}
                  >
                    Mark reviewed
                  </button>
                </div>
              )}
            </div>
          );
        })
      )}
    </div>
  );
}
