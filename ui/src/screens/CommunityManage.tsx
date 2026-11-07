import { useCallback, useEffect, useState } from "react";
import * as communityApi from "../lib/communityApi.js";
import { type CommunitiesConfigDto, type CommunityPageDto, formatMoney, type InvitationDto, type MemberDto, type MembershipStatus, type ReportDto } from "../lib/communityApi.js";
import { runCommunityKeyMaintenance } from "../lib/communityKeys.js";
import * as crypto from "../lib/e2eCrypto.js";
import { loadConversations } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { CATEGORY_LABELS, describeError, formatDate } from "./Communities.js";
import { parseDollars } from "./CommunityCreate.js";
import styles from "./Communities.module.css";

type Section = "settings" | "members" | "invitations" | "reports";

export function CommunityManage({ communityId, config, onBack }: { communityId: string; config: CommunitiesConfigDto | null; onBack: () => void }) {
  const [community, setCommunity] = useState<CommunityPageDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [section, setSection] = useState<Section>("settings");

  const load = useCallback(() => {
    communityApi
      .getCommunity(communityId)
      .then(setCommunity)
      .catch((err) => setError(describeError(err)));
  }, [communityId]);
  useEffect(load, [load]);

  if (error) return <p className={styles.error}>{error}</p>;
  if (!community) return <p className={styles.muted}>Loading…</p>;
  if (!community.staff) return <p className={styles.error}>Only the creator and staff manage this community.</p>;
  const role = community.viewer.membership?.role ?? "member";

  return (
    <>
      <div className={styles.toolbar}>
        <button type="button" className={styles.small} onClick={onBack}>
          ← Community page
        </button>
      </div>
      <div className={styles.hero}>
        <div className={styles.rowMain}>
          <h1 className={styles.heroTitle}>{community.name}</h1>
          <span className={styles.muted}>
            {community.kind === "channel" ? "Channel" : "Group"} · {community.accessType === "paid" ? "Paid" : "Free"} · you're {role}
          </span>
        </div>
      </div>
      <Lifecycle community={community} role={role} onChange={load} />
      <div className={styles.chips} role="tablist">
        {(["settings", "members", "invitations", "reports"] as Section[]).map((s) => (
          <button key={s} type="button" role="tab" aria-selected={section === s} className={section === s ? styles.chipAccent : styles.chip} onClick={() => setSection(s)}>
            {s[0].toUpperCase() + s.slice(1)}
          </button>
        ))}
      </div>
      {section === "settings" && <Settings community={community} role={role} config={config} onSaved={load} />}
      {section === "members" && <Members community={community} role={role} />}
      {section === "invitations" && <Invitations community={community} />}
      {section === "reports" && <Reports community={community} />}
    </>
  );
}

function Lifecycle({ community, role, onChange }: { community: CommunityPageDto; role: string; onChange: () => void }) {
  const { state, dispatch } = useAppState();
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const staff = community.staff!;

  async function run(fn: () => Promise<unknown>, done: string) {
    setBusy(true);
    setMessage(null);
    try {
      await fn();
      setMessage(done);
      onChange();
      if (state.identity) await loadConversations(dispatch, state.identity);
    } catch (err) {
      setMessage(describeError(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.panel}>
      <div className={styles.chips}>
        <span className={community.status === "published" ? styles.chipAccent : styles.chipWarn}>{community.status}</span>
        <span className={styles.chip}>{community.visibility === "discoverable" ? "Listed in Discover" : "Unlisted"}</span>
        {community.price && <span className={styles.chip}>{formatMoney(community.price.amountMinor, community.price.currency)}/month</span>}
        <span className={styles.chip}>Key epoch {staff.keyEpoch}</span>
        {staff.rekeyRequired && <span className={styles.chipWarn}>New key needed</span>}
      </div>
      {staff.rekeyRequired && (
        <p className={styles.notice}>Someone left, so the next message switches to a new key that only current members get. Any posting member's app does this automatically.</p>
      )}
      <div className={styles.actions}>
        {role === "owner" && (community.status === "draft" || community.status === "unpublished") && (
          <button type="button" className={styles.primaryButton} disabled={busy} onClick={() => run(() => communityApi.publishCommunity(community.id), "Published.")}>
            Publish
          </button>
        )}
        {role === "owner" && community.status === "published" && (
          <button type="button" className={styles.secondaryButton} disabled={busy} onClick={() => run(() => communityApi.unpublishCommunity(community.id), "Unpublished: hidden and closed to new members.")}>
            Unpublish
          </button>
        )}
        <button type="button" className={styles.secondaryButton} disabled={busy} onClick={() => run(() => runCommunityKeyMaintenance(), "Keys are up to date.")}>
          Deliver keys now
        </button>
        {role === "owner" && community.status !== "closed" && community.status !== "closing" && (
          <button
            type="button"
            className={styles.dangerButton}
            disabled={busy}
            onClick={() => {
              const paid = community.accessType === "paid";
              const text = paid
                ? "Close this community? Nobody is charged again. Paying members keep access until their paid period ends, then it closes for good. Refunds, if any, are handled by RedVault support."
                : "Close this community? Everyone except you loses access now.";
              if (window.confirm(text)) void run(() => communityApi.closeCommunity(community.id), paid ? "Closing: no more renewals." : "Closed.");
            }}
          >
            Close community
          </button>
        )}
      </div>
      {message && <p className={styles.notice}>{message}</p>}
    </div>
  );
}

async function fileToBase64(file: File | undefined, maxBytes: number): Promise<string | null | undefined> {
  if (!file) return undefined;
  if (file.size > maxBytes) throw new Error(`Images can be at most ${Math.floor(maxBytes / 1024)} KB.`);
  await crypto.ready();
  return crypto.toBase64(new Uint8Array(await file.arrayBuffer()));
}

function Settings({ community, role, config, onSaved }: { community: CommunityPageDto; role: string; config: CommunitiesConfigDto | null; onSaved: () => void }) {
  const staff = community.staff!;
  const canEdit = role === "owner" || role === "admin";
  const [form, setForm] = useState({
    name: community.name,
    description: community.description,
    category: community.category,
    rules: community.rules,
    previewText: community.previewText,
    visibility: community.visibility,
    memberLimit: staff.memberLimit ? String(staff.memberLimit) : "",
    slowModeSeconds: String(staff.slowModeSeconds),
    joinApproval: staff.joinApproval,
    newMembersPaused: community.newMembersPaused,
    invitationsSuspended: staff.invitationsSuspended,
  });
  const [price, setPrice] = useState(community.price ? (community.price.amountMinor / 100).toFixed(2) : "");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const set = <K extends keyof typeof form>(key: K, value: (typeof form)[K]) => setForm((f) => ({ ...f, [key]: value }));

  async function save(extra: Record<string, unknown> = {}) {
    setBusy(true);
    setMessage(null);
    try {
      await communityApi.updateCommunity(community.id, {
        name: form.name.trim(),
        description: form.description,
        category: form.category,
        rules: form.rules,
        previewText: form.previewText,
        visibility: form.visibility,
        memberLimit: form.memberLimit.trim() ? Number(form.memberLimit) : null,
        slowModeSeconds: Number(form.slowModeSeconds) || 0,
        joinApproval: form.joinApproval,
        newMembersPaused: form.newMembersPaused,
        invitationsSuspended: form.invitationsSuspended,
        expectedVersion: staff.version,
        ...extra,
      });
      setMessage("Saved.");
      onSaved();
    } catch (err) {
      setMessage(err instanceof Error && !(err as { code?: string }).code ? err.message : describeError(err));
    } finally {
      setBusy(false);
    }
  }

  async function savePrice() {
    const cents = parseDollars(price);
    if (cents === null) {
      setMessage("Enter a monthly price like 10 or 9.99.");
      return;
    }
    setBusy(true);
    try {
      await communityApi.setCommunityPrice(community.id, cents);
      setMessage("New price saved. It applies to new members; current members keep the price they signed up at.");
      onSaved();
    } catch (err) {
      setMessage(describeError(err));
    } finally {
      setBusy(false);
    }
  }

  if (!canEdit) return <p className={styles.muted}>Moderators manage members, invitations and reports. Settings are for the creator and admins.</p>;

  return (
    <div className={styles.form}>
      <label className={styles.field}>
        Name
        <input className={styles.input} value={form.name} maxLength={80} onChange={(e) => set("name", e.target.value)} />
      </label>
      <label className={styles.field}>
        Description
        <textarea className={styles.textarea} value={form.description} maxLength={2000} onChange={(e) => set("description", e.target.value)} />
      </label>
      <div className={styles.fieldRow}>
        <label className={styles.field}>
          Category
          <select className={styles.select} value={form.category} onChange={(e) => set("category", e.target.value)}>
            {(config?.categories ?? Object.keys(CATEGORY_LABELS)).map((c) => (
              <option key={c} value={c}>
                {CATEGORY_LABELS[c] ?? c}
              </option>
            ))}
          </select>
        </label>
        <label className={styles.field}>
          Who can find it
          <select className={styles.select} value={form.visibility} onChange={(e) => set("visibility", e.target.value as "unlisted" | "discoverable")}>
            <option value="unlisted">Unlisted: invite link only</option>
            <option value="discoverable">Listed in Discover</option>
          </select>
        </label>
        <label className={styles.field}>
          Member limit
          <input className={styles.input} inputMode="numeric" value={form.memberLimit} onChange={(e) => set("memberLimit", e.target.value.replace(/\D/g, ""))} />
        </label>
        <label className={styles.field}>
          Slow mode
          <select className={styles.select} value={form.slowModeSeconds} onChange={(e) => set("slowModeSeconds", e.target.value)}>
            <option value="0">Off</option>
            <option value="10">10 seconds</option>
            <option value="60">1 minute</option>
            <option value="300">5 minutes</option>
          </select>
        </label>
      </div>
      <label className={styles.field}>
        Rules
        <textarea className={styles.textarea} value={form.rules} maxLength={4000} onChange={(e) => set("rules", e.target.value)} />
      </label>
      <label className={styles.field}>
        Public preview
        <textarea className={styles.textarea} value={form.previewText} maxLength={1000} onChange={(e) => set("previewText", e.target.value)} />
      </label>
      <label className={styles.check}>
        <input type="checkbox" checked={form.newMembersPaused} onChange={(e) => set("newMembersPaused", e.target.checked)} />
        <span>Pause new memberships</span>
      </label>
      <label className={styles.check}>
        <input type="checkbox" checked={form.invitationsSuspended} onChange={(e) => set("invitationsSuspended", e.target.checked)} />
        <span>Suspend invite links</span>
      </label>
      {community.accessType === "free" && (
        <label className={styles.check}>
          <input type="checkbox" checked={form.joinApproval} onChange={(e) => set("joinApproval", e.target.checked)} />
          <span>Approve each person who asks to join</span>
        </label>
      )}
      <div className={styles.fieldRow}>
        <label className={styles.field}>
          Picture (≤ 300 KB)
          <input
            type="file"
            accept="image/png,image/jpeg,image/webp"
            onChange={async (e) => {
              try {
                const avatar = await fileToBase64(e.target.files?.[0], 300 * 1024);
                if (avatar !== undefined) await save({ avatar });
              } catch (err) {
                setMessage(describeError(err, (err as Error).message));
              }
            }}
          />
        </label>
        <label className={styles.field}>
          Banner (≤ 800 KB)
          <input
            type="file"
            accept="image/png,image/jpeg,image/webp"
            onChange={async (e) => {
              try {
                const banner = await fileToBase64(e.target.files?.[0], 800 * 1024);
                if (banner !== undefined) await save({ banner });
              } catch (err) {
                setMessage(describeError(err, (err as Error).message));
              }
            }}
          />
        </label>
      </div>
      <div className={styles.actions}>
        <button type="button" className={styles.primaryButton} disabled={busy || !form.name.trim()} onClick={() => save()}>
          Save settings
        </button>
      </div>

      {community.accessType === "paid" && role === "owner" && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>PRICE</span>
          <span className={styles.muted}>
            A new price applies to new members only. Everyone already subscribed keeps the price they agreed to.
            {config ? ` Allowed: ${formatMoney(config.minPriceMinor, config.currency)} to ${formatMoney(config.maxPriceMinor, config.currency)} a month.` : ""}
          </span>
          <div className={styles.toolbar}>
            <input className={styles.input} inputMode="decimal" aria-label="Monthly price in USD" value={price} onChange={(e) => setPrice(e.target.value)} />
            <button type="button" className={styles.secondaryButton} style={{ width: "auto" }} disabled={busy} onClick={savePrice}>
              Set new price
            </button>
          </div>
        </div>
      )}
      {message && <p className={styles.notice}>{message}</p>}
    </div>
  );
}

const MEMBER_FILTERS: { value: MembershipStatus; label: string }[] = [
  { value: "active", label: "Members" },
  { value: "pending_approval", label: "Requests" },
  { value: "pending_enrollment", label: "Waiting for key" },
  { value: "banned", label: "Banned" },
];

const RANK: Record<string, number> = { owner: 3, admin: 2, moderator: 1, member: 0 };

function Members({ community, role }: { community: CommunityPageDto; role: string }) {
  const [filter, setFilter] = useState<MembershipStatus>("active");
  const [members, setMembers] = useState<MemberDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);

  const load = useCallback(() => {
    setMembers(null);
    communityApi
      .listCommunityMembers(community.id, filter)
      .then(setMembers)
      .catch((err) => setError(describeError(err)));
  }, [community.id, filter]);
  useEffect(load, [load]);

  async function run(userId: string, fn: () => Promise<unknown>) {
    setBusy(userId);
    setError(null);
    try {
      await fn();
      load();
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(null);
    }
  }

  return (
    <>
      <div className={styles.chips}>
        {MEMBER_FILTERS.map((f) => (
          <button key={f.value} type="button" className={filter === f.value ? styles.chipAccent : styles.chip} onClick={() => setFilter(f.value)}>
            {f.label}
          </button>
        ))}
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {members === null && <p className={styles.muted}>Loading…</p>}
      {members?.length === 0 && <div className={styles.empty}>Nobody here.</div>}
      <div className={styles.list}>
        {members?.map((m) => (
          <div key={m.userId} className={styles.row}>
            <span className={styles.rowMain}>
              <span className={styles.cardTitle}>{m.username}</span>
              <span className={styles.muted}>
                {m.role}
                {community.kind === "channel" && m.role === "member" ? (m.canPost ? " · can post" : " · reads only") : ""}
                {m.entitlement ? ` · billing: ${m.entitlement.replace("_", " ")}` : ""}
                {m.statusReason ? ` · ${m.statusReason}` : ""}
              </span>
            </span>
            <span className={styles.rowActions}>
              {filter === "pending_approval" && (
                <>
                  <button type="button" className={styles.small} disabled={busy === m.userId} onClick={() => run(m.userId, () => communityApi.decideJoinRequest(community.id, m.userId, true))}>
                    Approve
                  </button>
                  <button type="button" className={styles.smallDanger} disabled={busy === m.userId} onClick={() => run(m.userId, () => communityApi.decideJoinRequest(community.id, m.userId, false))}>
                    Decline
                  </button>
                </>
              )}
              {filter === "banned" && RANK[role] >= 2 && (
                <button type="button" className={styles.small} disabled={busy === m.userId} onClick={() => run(m.userId, () => communityApi.unbanCommunityMember(community.id, m.userId))}>
                  Unban
                </button>
              )}
              {filter === "active" && m.role !== "owner" && RANK[role] > RANK[m.role] && (
                <>
                  {RANK[role] >= 2 && (
                    <select
                      className={styles.small}
                      aria-label={`Role for ${m.username}`}
                      value={m.role}
                      disabled={busy === m.userId}
                      onChange={(e) => run(m.userId, () => communityApi.setCommunityMemberRole(community.id, m.userId, { role: e.target.value as "admin" | "moderator" | "member" }))}
                    >
                      <option value="member">Member</option>
                      <option value="moderator">Moderator</option>
                      {role === "owner" && <option value="admin">Admin</option>}
                    </select>
                  )}
                  {RANK[role] >= 2 && m.role === "member" && (
                    <button type="button" className={styles.small} disabled={busy === m.userId} onClick={() => run(m.userId, () => communityApi.setCommunityMemberRole(community.id, m.userId, { canPost: !m.canPost }))}>
                      {m.canPost ? "Mute" : "Allow posting"}
                    </button>
                  )}
                  <button
                    type="button"
                    className={styles.smallDanger}
                    disabled={busy === m.userId}
                    onClick={() => {
                      if (window.confirm(`Remove ${m.username}? They lose access now${m.entitlement ? " and their membership stops billing (no automatic refund)" : ""}.`)) {
                        void run(m.userId, () => communityApi.removeCommunityMember(community.id, m.userId, false, null));
                      }
                    }}
                  >
                    Remove
                  </button>
                  <button
                    type="button"
                    className={styles.smallDanger}
                    disabled={busy === m.userId}
                    onClick={() => {
                      const reason = window.prompt(`Ban ${m.username}? They can't rejoin or subscribe again. Reason (optional):`);
                      if (reason !== null) void run(m.userId, () => communityApi.removeCommunityMember(community.id, m.userId, true, reason.trim() || null));
                    }}
                  >
                    Ban
                  </button>
                </>
              )}
            </span>
          </div>
        ))}
      </div>
    </>
  );
}

function Invitations({ community }: { community: CommunityPageDto }) {
  const [list, setList] = useState<InvitationDto[] | null>(null);
  const [created, setCreated] = useState<string | null>(null);
  const [maxUses, setMaxUses] = useState("");
  const [days, setDays] = useState("30");
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(() => {
    communityApi
      .listCommunityInvitations(community.id)
      .then(setList)
      .catch((err) => setError(describeError(err)));
  }, [community.id]);
  useEffect(load, [load]);

  async function create() {
    setError(null);
    try {
      const inv = await communityApi.createCommunityInvitation(community.id, { maxUses: maxUses ? Number(maxUses) : null, expiresInDays: days ? Number(days) : null });
      const base = `${window.location.origin}${import.meta.env.BASE_URL}`.replace(/\/?$/, "/");
      setCreated(`${base}?community=${community.id}&invite=${inv.token}`);
      load();
    } catch (err) {
      setError(describeError(err));
    }
  }

  return (
    <>
      <p className={styles.muted}>
        An invite link lets someone see this community{community.visibility === "unlisted" ? " (it's unlisted)" : ""} and {community.accessType === "paid" ? "subscribe" : "join"}. It never skips payment.
      </p>
      <div className={styles.toolbar}>
        <input className={styles.input} inputMode="numeric" placeholder="Max uses (blank = unlimited)" aria-label="Max uses" value={maxUses} onChange={(e) => setMaxUses(e.target.value.replace(/\D/g, ""))} />
        <select className={styles.select} aria-label="Expires" value={days} onChange={(e) => setDays(e.target.value)}>
          <option value="1">Expires in 1 day</option>
          <option value="7">Expires in 7 days</option>
          <option value="30">Expires in 30 days</option>
          <option value="">Never expires</option>
        </select>
        <button type="button" className={styles.primaryButton} style={{ width: "auto" }} onClick={create}>
          Create link
        </button>
      </div>
      {created && (
        <div className={styles.panelAccent}>
          <strong>Copy this link now: it's shown only once.</strong>
          <span className={styles.code}>{created}</span>
          <div className={styles.actions}>
            <button type="button" className={styles.small} onClick={() => navigator.clipboard?.writeText(created)}>
              Copy
            </button>
          </div>
        </div>
      )}
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.list}>
        {list?.length === 0 && <div className={styles.empty}>No invite links.</div>}
        {list?.map((i) => (
          <div key={i.id} className={styles.row}>
            <span className={styles.rowMain}>
              <span>
                Used {i.useCount}
                {i.maxUses ? ` of ${i.maxUses}` : ""} time(s)
              </span>
              <span className={styles.muted}>
                Created {formatDate(i.createdAt)} · {i.revokedAt ? `revoked ${formatDate(i.revokedAt)}` : i.expiresAt ? `expires ${formatDate(i.expiresAt)}` : "no expiry"}
              </span>
            </span>
            {!i.revokedAt && (
              <button type="button" className={styles.smallDanger} onClick={() => communityApi.revokeCommunityInvitation(community.id, i.id).then(load).catch((err) => setError(describeError(err)))}>
                Revoke
              </button>
            )}
          </div>
        ))}
      </div>
    </>
  );
}

function Reports({ community }: { community: CommunityPageDto }) {
  const [reports, setReports] = useState<ReportDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(() => {
    communityApi
      .listCommunityReports(community.id)
      .then(setReports)
      .catch((err) => setError(describeError(err)));
  }, [community.id]);
  useEffect(load, [load]);

  async function resolve(id: string, status: "actioned" | "dismissed") {
    try {
      await communityApi.resolveCommunityReport(community.id, id, status, null);
      load();
    } catch (err) {
      setError(describeError(err));
    }
  }

  return (
    <>
      <p className={styles.muted}>Reports from members. Who reported is never shown. Reports about child safety, non-consensual images or other illegal content go to RedVault, not here.</p>
      {error && <p className={styles.error}>{error}</p>}
      {reports?.length === 0 && <div className={styles.empty}>No reports.</div>}
      {reports?.map((r) => (
        <div key={r.id} className={styles.panel}>
          <div className={styles.chips}>
            <span className={r.status === "open" ? styles.chipWarn : styles.chip}>{r.status}</span>
            <span className={styles.chip}>{r.category.replace("_", " ")}</span>
            <span className={styles.chip}>{r.targetType}</span>
            <span className={styles.muted}>{formatDate(r.createdAt)}</span>
          </div>
          {r.details && <p className={styles.text}>{r.details}</p>}
          {r.evidencePlaintext && <p className={styles.quote}>{r.evidencePlaintext}</p>}
          {r.evidencePurgedAt && <span className={styles.muted}>Evidence removed after the retention period.</span>}
          {r.status === "open" && (
            <div className={styles.actions}>
              <button type="button" className={styles.small} onClick={() => resolve(r.id, "actioned")}>
                Mark handled
              </button>
              <button type="button" className={styles.small} onClick={() => resolve(r.id, "dismissed")}>
                Dismiss
              </button>
            </div>
          )}
        </div>
      ))}
    </>
  );
}
