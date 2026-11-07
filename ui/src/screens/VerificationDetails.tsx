import { TopBar } from "../components/TopBar.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./VerificationDetails.module.css";

/**
 * "View Verification Details" — see notification.md. Shows the sender's
 * locally-recomputed commitment against the transparency system's own
 * independently-computed one for this one message, plus an honest
 * statement of what a match does and doesn't prove. Never displays secret
 * keys, session keys, or any private E2EE state — only public hashes/ids.
 */
export function VerificationDetails({ conversationId, messageId }: { conversationId: string; messageId: string }) {
  const { state, dispatch } = useAppState();
  const conversation = state.conversations.find((c) => c.id === conversationId);
  const message = conversation?.kind === "dm" ? conversation.messages.find((m) => m.id === messageId) : undefined;

  const onBack = () => dispatch({ type: "OPEN_THREAD", conversationId });

  if (!message?.security) {
    return (
      <div className={styles.screen}>
        <TopBar title="Verification Details" onBack={onBack} />
        <div className={styles.body}>
          <p className="type-secondary">No verification record for this message yet.</p>
        </div>
      </div>
    );
  }

  const { security } = message;
  const matched = security.verification === "verified";
  const failed = security.verification === "failed";

  return (
    <div className={styles.screen}>
      <TopBar title="Verification Details" onBack={onBack} />
      <div className={styles.body}>
        <Row label="Public transaction" value={security.publicTransactionId ?? "—"} />
        <Row label="Protocol" value={security.protocolVersion ? `E2EE-v${security.protocolVersion}` : "—"} />
        <Row label="Local commitment" value={security.localCommitment ?? "—"} mono />
        <Row label="Network commitment" value={security.networkCommitment ?? "—"} mono />
        <div className={`${styles.verdict} ${failed ? styles.verdictFailed : matched ? styles.verdictMatch : styles.verdictUnavailable}`}>
          {matched ? "MATCH ✓" : failed ? "MISMATCH ✗ — do not trust this message's delivery status" : "Unavailable"}
        </div>
        <p className={`${styles.limitations} type-caption`}>
          A matching commitment proves the ciphertext this app relayed is byte-identical to what your device encrypted, and
          that the server independently recomputed the same hash from what it actually stored. It does not prove the
          encryption itself has no vulnerabilities, that either device is uncompromised, that the recipient is who you
          believe, or that this server is running unmodified published source.
        </p>
      </div>
    </div>
  );
}

function Row({ label, value, mono }: { label: string; value: string; mono?: boolean }) {
  return (
    <div className={styles.row}>
      <div className={`${styles.label} type-caption`}>{label}</div>
      <div className={`${styles.value} ${mono ? "type-mono" : ""}`}>{value}</div>
    </div>
  );
}
