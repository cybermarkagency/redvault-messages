import { useRef, useState } from "react";
import { useDialogFocus } from "../components/useDialogFocus.js";
import * as communityApi from "../lib/communityApi.js";
import { describeError } from "./Communities.js";
import styles from "./Communities.module.css";

const CATEGORIES: { value: string; label: string; platformOnly?: boolean }[] = [
  { value: "spam", label: "Spam" },
  { value: "scam", label: "Scam or fraud" },
  { value: "harassment", label: "Harassment or hate" },
  { value: "impersonation", label: "Impersonation" },
  { value: "copyright", label: "Copyright" },
  { value: "refund_dispute", label: "Billing or refund problem" },
  { value: "child_safety", label: "Child safety", platformOnly: true },
  { value: "nonconsensual_imagery", label: "Intimate images shared without consent", platformOnly: true },
  { value: "illegal", label: "Other illegal content", platformOnly: true },
  { value: "other", label: "Something else" },
];

export type ReportTarget =
  | { kind: "community" }
  | { kind: "content"; messageId: string; senderId: string | null; text: string };

/**
 * Reporting never reads anything for you: for a message, the reporter
 * chooses to disclose its decrypted text, and is told exactly who will see it.
 */
export function CommunityReportDialog({ communityId, target, onClose, onDone }: { communityId: string; target: ReportTarget; onClose: () => void; onDone: () => void }) {
  const [category, setCategory] = useState("spam");
  const [details, setDetails] = useState("");
  const [share, setShare] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const dialogRef = useRef<HTMLDivElement>(null);
  useDialogFocus(dialogRef, onClose);
  const platformOnly = CATEGORIES.find((c) => c.value === category)?.platformOnly || target.kind === "community";

  async function submit() {
    setBusy(true);
    setError(null);
    try {
      await communityApi.reportToCommunity(communityId, {
        targetType: target.kind,
        targetUserId: target.kind === "content" ? target.senderId : null,
        category,
        details: details.trim(),
        ...(target.kind === "content" && share ? { evidenceMessageId: target.messageId, evidencePlaintext: target.text, disclosureAcknowledged: true } : {}),
      });
      onDone();
      onClose();
    } catch (err) {
      setError(describeError(err));
      setBusy(false);
    }
  }

  return (
    <div className={styles.dialogBackdrop} role="presentation" onClick={onClose}>
      <div className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="community-report-title" ref={dialogRef} onClick={(e) => e.stopPropagation()}>
        <h2 id="community-report-title" className={styles.dialogTitle}>
          {target.kind === "community" ? "Report this community" : "Report this message"}
        </h2>
        <label className={styles.field}>
          What's wrong?
          <select className={styles.select} value={category} onChange={(e) => setCategory(e.target.value)}>
            {CATEGORIES.map((c) => (
              <option key={c.value} value={c.value}>
                {c.label}
              </option>
            ))}
          </select>
        </label>
        <label className={styles.field}>
          Details (optional)
          <textarea className={styles.textarea} value={details} maxLength={2000} onChange={(e) => setDetails(e.target.value)} />
        </label>
        {target.kind === "content" && (
          <>
            <p className={styles.quote}>{target.text.slice(0, 600)}</p>
            <label className={styles.check}>
              <input type="checkbox" checked={share} onChange={(e) => setShare(e.target.checked)} />
              <span>
                Share this message's text with {platformOnly ? "RedVault's safety reviewers" : "this community's moderators"}. It's end-to-end encrypted, so they can only see it if you
                share it. Your name isn't shown to the community's moderators.
              </span>
            </label>
          </>
        )}
        <p className={styles.muted}>
          {platformOnly
            ? "This goes to RedVault's own reviewers, not to the community's moderators."
            : "This goes to the community's moderators. Serious categories go to RedVault instead."}
        </p>
        {error && <p className={styles.error}>{error}</p>}
        <div className={styles.actions}>
          <button type="button" className={styles.dangerButton} disabled={busy} onClick={submit}>
            Send report
          </button>
          <button type="button" className={styles.secondaryButton} onClick={onClose}>
            Cancel
          </button>
        </div>
      </div>
    </div>
  );
}
