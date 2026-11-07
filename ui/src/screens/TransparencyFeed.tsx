import { useEffect, useRef, useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { API_BASE } from "../lib/apiBase.js";
import { getTransparencyFeed, type PublicTransparencyEventDto } from "../lib/api.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./TransparencyFeed.module.css";

const MAX_EVENTS = 50;

/** "tx_AbC123..." -> "TX-ABC123" — a short, glanceable label. The full id is still shown below it. */
function shortLabel(publicTransactionId: string): string {
  const raw = publicTransactionId.replace(/^tx_/, "");
  return `TX-${raw.slice(0, 6).toUpperCase()}`;
}

/**
 * The public live transparency feed — see notification.md and
 * server/src/services/transparencyService.ts. Reachable with no account:
 * a fixed sandbox conversation always appears here, and any signed-in user
 * can additionally opt their own messages in via Settings > Transparency
 * Mode. Never shows plaintext, a sender/recipient identity, or which
 * conversation a message belongs to — only a public transaction id, a
 * cryptographic commitment, and (only for the sandbox or an opted-in
 * sender) the raw ciphertext itself.
 */
export function TransparencyFeed({ returnTo }: { returnTo: "onboarding" | "settings" }) {
  const { dispatch } = useAppState();
  const [events, setEvents] = useState<PublicTransparencyEventDto[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [live, setLive] = useState(false);
  const seenIds = useRef(new Set<string>());

  useEffect(() => {
    let cancelled = false;
    getTransparencyFeed({ limit: 20 })
      .then((initial) => {
        if (cancelled) return;
        for (const e of initial) seenIds.current.add(e.publicTransactionId);
        setEvents(initial);
        setError(null);
      })
      .catch(() => {
        if (!cancelled) setError("Couldn't load the feed — try again shortly.");
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    // Not every environment has EventSource (older browsers, some test
    // environments, restrictive proxies that strip SSE) — degrade to the
    // initial list load above rather than crashing the screen.
    if (typeof EventSource === "undefined") return;
    const source = new EventSource(`${API_BASE}/public/transparency-feed/stream`);
    source.addEventListener("open", () => setLive(true));
    source.addEventListener("error", () => setLive(false));
    source.addEventListener("transaction", (e: MessageEvent<string>) => {
      try {
        const event = JSON.parse(e.data) as PublicTransparencyEventDto;
        if (seenIds.current.has(event.publicTransactionId)) return;
        seenIds.current.add(event.publicTransactionId);
        setEvents((prev) => [event, ...prev].slice(0, MAX_EVENTS));
      } catch {
        // Malformed event — ignore rather than crash the feed.
      }
    });
    return () => source.close();
  }, []);

  return (
    <div className={styles.screen}>
      <TopBar
        title="Live Encrypted Network"
        onBack={() =>
          returnTo === "settings" ? dispatch({ type: "OPEN_SETTINGS" }) : dispatch({ type: "GO_ONBOARDING" })
        }
      />
      <div className={styles.body}>
        <p className={`${styles.intro} type-secondary`}>
          A live, public record of messages actually relayed by this server. Each entry shows a cryptographic
          commitment the server independently computed — proof the ciphertext wasn't altered in transit — and,
          only for messages whose sender explicitly opted in, the encrypted ciphertext itself. Never plaintext.
          Never a sender, recipient, or conversation identity.
        </p>
        <div className={styles.statusRow}>
          <span className={`${styles.liveDot} ${live ? styles.liveDotOn : ""}`} aria-hidden="true" />
          <span className="type-caption">{live ? "LIVE" : "connecting…"}</span>
        </div>

        {loading && <p className="type-secondary">Loading…</p>}
        {error && <p className={styles.error}>{error}</p>}
        {!loading && !error && events.length === 0 && (
          <p className="type-secondary">No public transactions yet. Nobody has opted in, and the demo feed is idle.</p>
        )}

        <div className={styles.list}>
          {events.map((event) => (
            <div key={event.publicTransactionId} className={styles.card}>
              <div className={styles.cardHeader}>
                <span className={`${styles.txLabel} type-mono`}>{shortLabel(event.publicTransactionId)}</span>
                <span className="type-caption">{event.relayStatus === "relayed" ? "Relayed ✓" : event.relayStatus}</span>
              </div>
              <div className={`${styles.row} type-caption`}>
                <span>Public tx</span>
                <span className={styles.mono}>{event.publicTransactionId}</span>
              </div>
              <div className={`${styles.row} type-caption`}>
                <span>Protocol</span>
                <span>E2EE-v{event.protocolVersion}</span>
              </div>
              <div className={`${styles.row} type-caption`}>
                <span>Commitment</span>
                <span className={styles.mono}>{event.commitment}</span>
              </div>
              {event.expired ? (
                <div className={`${styles.ciphertextBlock} type-caption`}>Message expired — ciphertext no longer stored.</div>
              ) : event.ciphertext ? (
                <div className={styles.ciphertextBlock}>
                  <div className="type-caption">Encrypted content (ciphertext — not readable without the recipient's key)</div>
                  <div className={`${styles.ciphertextValue} type-mono`}>{event.ciphertext}</div>
                </div>
              ) : null}
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
