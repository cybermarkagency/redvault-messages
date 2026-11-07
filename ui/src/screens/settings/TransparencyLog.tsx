import { useCallback, useEffect, useMemo, useState } from "react";
import { CheckIcon, ClockIcon, CopyIcon, DownloadIcon, MonitorIcon, SearchIcon, ShieldCheckIcon, ShieldIcon } from "../../components/icons24.js";
import { useIsDesktopLayout } from "../../hooks/useMediaQuery.js";
import * as api from "../../lib/api.js";
import { commitmentToHex, computeCommitment, COMMITMENT_PROTOCOL_VERSION } from "../../lib/commitment.js";
import { fromBase64, ready } from "../../lib/e2eCrypto.js";
import { saveBlobAs } from "../../lib/fileTransfer.js";
import { acceptLogKey, fetchVerifiedCheckpoint, keyFingerprint, LogVerificationError, type VerifiedCheckpoint, verifyEntryInclusion } from "../../lib/transparencyLog.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./TransparencyLog.module.css";

type MineStatus = "match" | "pending" | "mismatch";

/** One of your own messages, as this device checked it against the log. */
interface MyEntry {
  messageId: string;
  commitment: string; // computed here
  logIndex: number | null;
  status: MineStatus;
}

const LIVE_POLL_MS = 15_000;
const MAX_OWN_CHECKS = 40;

function short(hex: string): string {
  return hex.length > 30 ? `${hex.slice(0, 16)}…${hex.slice(-10)}` : hex;
}

/**
 * Settings › Transparency log. Everything here is checked on this device:
 * the checkpoint's signature (under the log key pinned the first time),
 * that the log only ever grew since this browser last looked, and that each
 * of your own messages is in it, using commitments computed here.
 */
export function TransparencyLog() {
  const { state } = useAppState();
  const isDesktop = useIsDesktopLayout();
  const [checkpoint, setCheckpoint] = useState<VerifiedCheckpoint | null>(null);
  const [checkError, setCheckError] = useState<string | null>(null);
  const [disabled, setDisabled] = useState(false);
  const [entries, setEntries] = useState<api.LogEntryDto[]>([]);
  const [search, setSearch] = useState("");
  const [onlyMine, setOnlyMine] = useState(false);
  const [live, setLive] = useState(true);
  const [mine, setMine] = useState<MyEntry[]>([]);
  const [checkingMine, setCheckingMine] = useState(false);
  const [selected, setSelected] = useState<number | null>(null);

  const refresh = useCallback(async () => {
    try {
      await ready();
      const cp = await fetchVerifiedCheckpoint();
      setCheckpoint(cp);
      setCheckError(null);
      setEntries(await api.getLogEntries({ limit: 50, q: search.trim() || undefined }));
    } catch (err) {
      if (err instanceof api.ApiError && (err.code === "log_disabled" || err.status === 404)) setDisabled(true);
      else setCheckError(err instanceof LogVerificationError ? err.message : "Couldn't reach the log. Try again.");
    }
  }, [search]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  useEffect(() => {
    if (!live || disabled) return;
    const timer = setInterval(() => void refresh(), LIVE_POLL_MS);
    return () => clearInterval(timer);
  }, [live, disabled, refresh]);

  /** Your own sent messages: commitment computed here, entry number from the server, inclusion proof checked here. */
  const checkMine = useCallback(async () => {
    if (!checkpoint) return;
    setCheckingMine(true);
    const own = state.conversations
      .flatMap((c) => c.messages)
      .filter((m) => m.from === "me" && m.id && m.ciphertextB64 && m.nonceB64)
      .sort((a, b) => (b.createdAt ?? 0) - (a.createdAt ?? 0))
      .slice(0, MAX_OWN_CHECKS);
    const results: MyEntry[] = [];
    for (const m of own) {
      const local = commitmentToHex(computeCommitment({ messageId: m.id!, ciphertext: fromBase64(m.ciphertextB64!), nonce: fromBase64(m.nonceB64!) }));
      try {
        const v = await api.getMessageVerification(m.id!);
        const logIndex = v.logIndex ?? null;
        let status: MineStatus;
        if (v.commitment.toLowerCase() !== local) status = "mismatch";
        else if (logIndex === null || logIndex >= checkpoint.treeSize) status = "pending";
        else status = (await verifyEntryInclusion(local, logIndex, checkpoint)) ? "match" : "mismatch";
        results.push({ messageId: m.id!, commitment: local, logIndex, status });
      } catch {
        // A message from before the log existed, or a transient error: not counted either way.
      }
    }
    setMine(results);
    setCheckingMine(false);
  }, [checkpoint, state.conversations]);

  useEffect(() => {
    void checkMine();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [checkpoint?.treeSize]);

  const mineByIndex = useMemo(() => new Map(mine.filter((m) => m.logIndex !== null).map((m) => [m.logIndex!, m])), [mine]);
  const mismatches = mine.filter((m) => m.status === "mismatch").length;
  const matched = mine.filter((m) => m.status === "match").length;
  const pendingMine = mine.filter((m) => m.status === "pending");

  // "Only mine": your logged entries, fetched by number.
  const [mineRows, setMineRows] = useState<api.LogEntryDto[]>([]);
  useEffect(() => {
    if (!onlyMine) return;
    let cancelled = false;
    (async () => {
      const rows: api.LogEntryDto[] = [];
      for (const m of mine.filter((x) => x.logIndex !== null).slice(0, 25)) {
        try {
          rows.push(...(await api.getLogEntries({ q: `#${m.logIndex}`, limit: 1 })));
        } catch {
          // skip
        }
      }
      if (!cancelled) setMineRows(rows.sort((a, b) => b.index - a.index));
    })();
    return () => {
      cancelled = true;
    };
  }, [onlyMine, mine]);

  const rows = onlyMine ? mineRows : entries;
  const selectedEntry = rows.find((e) => e.index === selected) ?? null;

  // Each listed row is checked here, not taken on the server's word: its
  // inclusion proof must lead to the root of the checkpoint this device
  // verified. Results are kept per checkpoint, so each row is checked once.
  const [rowProofs, setRowProofs] = useState<Map<string, "ok" | "bad">>(new Map());
  useEffect(() => {
    if (!checkpoint) return;
    let cancelled = false;
    const cp = checkpoint;
    (async () => {
      for (const entry of rows) {
        if (cancelled) return;
        const key = `${entry.index}@${cp.treeSize}:${cp.rootHash}`;
        if (rowProofs.has(key)) continue;
        let ok = false;
        try {
          ok = entry.index < cp.treeSize && (await verifyEntryInclusion(entry.commitment, entry.index, cp));
        } catch {
          ok = false;
        }
        if (!cancelled) setRowProofs((m) => new Map(m).set(key, ok ? "ok" : "bad"));
      }
    })();
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [rows, checkpoint?.treeSize, checkpoint?.rootHash]);
  const rowProof = (index: number) => (checkpoint ? rowProofs.get(`${index}@${checkpoint.treeSize}:${checkpoint.rootHash}`) : undefined);
  const failedRows = rows.filter((e) => rowProof(e.index) === "bad").length;

  function exportJson() {
    const body = JSON.stringify({ checkpoint, entries: rows }, null, 2);
    saveBlobAs(new Blob([body], { type: "application/json" }), `transparency-log-${checkpoint?.treeSize ?? 0}.json`);
  }

  if (disabled) {
    return (
      <div className={styles.page}>
        <div className={styles.main}>
          <h2 className={`${styles.title} type-display`}>Transparency log</h2>
          <p className={styles.lead}>The verifiable transparency log isn't set up on this server yet.</p>
        </div>
      </div>
    );
  }

  const keyChanged = checkpoint?.keyStatus === "changed";
  const forked = checkpoint?.consistent === false;

  return (
    <div className={styles.page}>
      <div className={styles.main}>
        <div className={styles.titleRow}>
          <h2 className={`${styles.title} type-display`}>Transparency log</h2>
          <button type="button" className={styles.liveToggle} aria-pressed={live} onClick={() => setLive((v) => !v)}>
            <span className={live ? styles.dotLive : styles.dotPaused} /> {live ? "Live · pause" : "Paused · resume"}
          </button>
        </div>
        <p className={styles.lead}>
          A public, append-only record of every message this network relays. Each entry is a commitment to a sealed message, logged in
          signed batches, so anyone can check nothing was altered or removed on the way through.
        </p>
        <div className={styles.privacyNote}>
          <ShieldIcon size={16} /> The log never contains message content, senders or recipients. Only this device can tell which entries are
          yours.
        </div>

        {(checkError || keyChanged || forked) && (
          <div className={styles.alert} role="alert">
            {checkError ??
              (keyChanged
                ? "The log's signing key changed since this browser first saw it. Only accept the new key if the operator has announced it."
                : "This checkpoint doesn't extend the one this browser verified before: the log may have been rewritten. Treat it as untrusted.")}
            {keyChanged && checkpoint && (
              <button type="button" onClick={() => (acceptLogKey(checkpoint.publicKey), void refresh())}>
                Trust new key {keyFingerprint(checkpoint.publicKey)}
              </button>
            )}
          </div>
        )}

        <div className={styles.stats}>
          <div className={styles.stat}>
            <span className={styles.statLabel}>Entries logged</span>
            <span className={styles.statValue}>{checkpoint ? checkpoint.treeSize.toLocaleString() : "0"}</span>
          </div>
          <div className={styles.stat}>
            <span className={styles.statLabel}>Latest signed checkpoint</span>
            <span className={styles.statMono} title={checkpoint?.rootHash}>
              {checkpoint ? short(checkpoint.rootHash) : "none yet"}
            </span>
            {checkpoint && (
              <span className={styles.statDetail}>
                {checkpoint.consistent === null ? "Signature checked · first look" : checkpoint.consistent ? "Signature and history checked" : "History check failed"} ·
                key {keyFingerprint(checkpoint.publicKey)}
              </span>
            )}
          </div>
          <div className={`${styles.stat} ${mismatches ? styles.statBad : styles.statGood}`}>
            <span className={styles.statLabel}>Your messages checked on this device</span>
            <span className={styles.statResult}>
              {checkingMine ? (
                "Checking…"
              ) : mismatches ? (
                `${mismatches} mismatch${mismatches > 1 ? "es" : ""}`
              ) : (
                <>
                  <CheckIcon size={18} /> {matched ? `All match · 0 mismatches` : "Nothing to check yet"}
                </>
              )}
            </span>
            {pendingMine.length > 0 && <span className={styles.statDetail}>{pendingMine.length} waiting for the next checkpoint</span>}
          </div>
        </div>

        <div className={styles.toolbar}>
          <label className={styles.search}>
            <SearchIcon size={18} />
            <input
              type="search"
              placeholder="Search by entry number or commitment"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
              aria-label="Search the log"
            />
          </label>
          <div className={styles.segmented}>
            <button type="button" aria-pressed={!onlyMine} onClick={() => setOnlyMine(false)}>
              All entries
            </button>
            <button type="button" aria-pressed={onlyMine} onClick={() => setOnlyMine(true)}>
              Only mine
            </button>
          </div>
          <button type="button" className={styles.export} onClick={exportJson}>
            <DownloadIcon size={16} /> Export
          </button>
        </div>

        {failedRows > 0 && (
          <div className={styles.alert} role="alert">
            {failedRows} listed {failedRows === 1 ? "entry doesn't" : "entries don't"} prove to be in the signed checkpoint. The log is not
            behaving honestly for {failedRows === 1 ? "it" : "them"}.
          </div>
        )}

        <div className={styles.table} role="table" aria-label="Log entries">
          <div className={`${styles.tr} ${styles.th}`} role="row">
            <span role="columnheader">Entry</span>
            <span role="columnheader">Commitment</span>
            <span role="columnheader">Status</span>
          </div>
          {onlyMine &&
            pendingMine.map((m) => (
              <div key={m.messageId} className={styles.tr} role="row">
                <span className={styles.mono}>—</span>
                <span className={styles.mono}>
                  {short(m.commitment)} <span className={styles.yours}>Yours</span>
                </span>
                <span className={styles.pending}>
                  <ClockIcon size={15} /> Pending
                </span>
              </div>
            ))}
          {rows.length === 0 && (!onlyMine || pendingMine.length === 0) && (
            <div className={styles.emptyRow}>{onlyMine ? "None of your recent messages are in the log yet." : "No entries yet."}</div>
          )}
          {rows.map((entry) => {
            const own = mineByIndex.get(entry.index);
            return (
              <button
                key={entry.index}
                type="button"
                role="row"
                className={`${styles.tr} ${styles.rowButton} ${selected === entry.index ? styles.trOn : ""}`}
                onClick={() => setSelected(entry.index)}
              >
                <span className={styles.mono}>#{entry.index}</span>
                <span className={styles.mono}>
                  {short(entry.commitment)} {own && <span className={styles.yours}>Yours</span>}
                </span>
                {own?.status === "mismatch" || rowProof(entry.index) === "bad" ? (
                  <span className={styles.bad}>{own?.status === "mismatch" ? "Mismatch" : "Proof failed"}</span>
                ) : rowProof(entry.index) === "ok" ? (
                  <span className={styles.good}>
                    <CheckIcon size={15} /> Verified here
                  </span>
                ) : (
                  <span className={styles.pending}>
                    <ClockIcon size={15} /> Checking…
                  </span>
                )}
              </button>
            );
          })}
        </div>
      </div>

      {(isDesktop || selectedEntry) && (
        <EntryPanel entry={selectedEntry} checkpoint={checkpoint} mine={selectedEntry ? mineByIndex.get(selectedEntry.index) : undefined} />
      )}
    </div>
  );
}

function EntryPanel({ entry, checkpoint, mine }: { entry: api.LogEntryDto | null; checkpoint: VerifiedCheckpoint | null; mine: MyEntry | undefined }) {
  const [proof, setProof] = useState<{ path: string[]; ok: boolean } | null>(null);
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState(false);
  const [raw, setRaw] = useState(false);

  const check = useCallback(async () => {
    if (!entry || !checkpoint) return;
    setBusy(true);
    try {
      const p = await api.getLogProof(entry.index, checkpoint.treeSize);
      const ok = await verifyEntryInclusion(entry.commitment, entry.index, checkpoint);
      setProof({ path: p.path, ok });
    } catch {
      setProof({ path: [], ok: false });
    } finally {
      setBusy(false);
    }
  }, [entry, checkpoint]);

  useEffect(() => {
    setProof(null);
    setRaw(false);
    void check();
  }, [check]);

  if (!entry) {
    return (
      <aside className={styles.panel}>
        <p className={styles.lead}>Select an entry to check its inclusion proof on this device.</p>
      </aside>
    );
  }

  const rawJson = JSON.stringify({ entry, checkpoint: checkpoint && { treeSize: checkpoint.treeSize, rootHash: checkpoint.rootHash, signedAt: checkpoint.signedAt, signature: checkpoint.signature, publicKey: checkpoint.publicKey }, inclusionProof: proof?.path ?? [] }, null, 2);

  return (
    <aside className={styles.panel} aria-label={`Entry ${entry.index}`}>
      <span className="type-caption">ENTRY</span>
      <div className={styles.entryNumber}>#{entry.index}</div>
      <div className={proof && !proof.ok ? styles.calloutBad : styles.calloutGood}>
        {proof === null || busy ? (
          "Checking the inclusion proof…"
        ) : proof.ok ? (
          <>
            <CheckIcon size={18} /> Included in the latest signed checkpoint. The inclusion proof below checks out.
          </>
        ) : (
          "The inclusion proof doesn't check out against the signed checkpoint."
        )}
      </div>
      {mine && (
        <div className={styles.calloutYours}>
          <MonitorIcon size={18} /> Your message.{" "}
          {mine.status === "mismatch" ? "The commitment this device computed doesn't match the log." : "The commitment this device computed matches the log."}
        </div>
      )}
      <span className={styles.fieldLabel}>Commitment</span>
      <div className={styles.codeBox}>{entry.commitment}</div>
      <dl className={styles.facts}>
        <dt>In checkpoint</dt>
        <dd className={styles.mono}>{entry.checkpointSize ? `#${entry.checkpointSize.toLocaleString()} entries` : "—"}</dd>
        <dt>Scheme</dt>
        <dd className={styles.mono}>SHA-256 commitment v{COMMITMENT_PROTOCOL_VERSION} · RFC 9162 tree · Ed25519</dd>
      </dl>
      <span className={styles.fieldLabel}>Inclusion proof</span>
      <div className={styles.codeBox}>
        {proof?.path.length ? proof.path.map((h, i) => <div key={i}>{h}</div>) : <div>{busy ? "…" : "(single entry: no path)"}</div>}
      </div>
      {raw && <pre className={styles.raw}>{rawJson}</pre>}
      <div className={styles.panelSpacer} />
      <button type="button" className={styles.primary} onClick={check} disabled={busy}>
        <ShieldCheckIcon size={18} /> Re-check on this device
      </button>
      <div className={styles.panelButtons}>
        <button
          type="button"
          className={styles.secondary}
          onClick={async () => {
            try {
              await navigator.clipboard.writeText(JSON.stringify({ index: entry.index, treeSize: checkpoint?.treeSize, path: proof?.path ?? [] }));
              setCopied(true);
              setTimeout(() => setCopied(false), 1500);
            } catch {
              setCopied(false);
            }
          }}
        >
          {copied ? <CheckIcon size={16} /> : <CopyIcon size={16} />} {copied ? "Copied" : "Copy proof"}
        </button>
        <button type="button" className={styles.secondary} aria-pressed={raw} onClick={() => setRaw((v) => !v)}>
          {raw ? "Hide raw JSON" : "View raw JSON"}
        </button>
      </div>
    </aside>
  );
}
