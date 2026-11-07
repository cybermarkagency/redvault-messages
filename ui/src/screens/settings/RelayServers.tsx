import { useEffect, useState } from "react";
import { GlobeIcon, PhoneIcon, RefreshIcon } from "../../components/icons24.js";
import * as api from "../../lib/api.js";
import { API_BASE } from "../../lib/apiBase.js";
import styles from "./SettingsPages.module.css";

type Check = { state: "idle" } | { state: "checking" } | { state: "ok"; ms: number } | { state: "failed" };

function describe(check: Check): { text: string; tone: string } {
  if (check.state === "checking") return { text: "Checking…", tone: styles.rowDetail };
  if (check.state === "ok") return { text: `Reachable · ${check.ms} ms`, tone: styles.ok };
  if (check.state === "failed") return { text: "Unreachable from here", tone: styles.warn };
  return { text: "Not checked", tone: styles.rowDetail };
}

/** Times how long this browser takes to get a relay address from one TURN URL. Sends no media and no identity. */
async function checkTurn(url: string, server: api.IceServerDto): Promise<Check> {
  if (typeof RTCPeerConnection === "undefined") return { state: "failed" };
  const started = performance.now();
  const pc = new RTCPeerConnection({ iceServers: [{ urls: [url], username: server.username, credential: server.credential }], iceTransportPolicy: "relay" });
  try {
    return await new Promise<Check>((resolve) => {
      const timer = setTimeout(() => resolve({ state: "failed" }), 8000);
      pc.onicecandidate = (e) => {
        if (e.candidate && / typ relay /.test(e.candidate.candidate)) {
          clearTimeout(timer);
          resolve({ state: "ok", ms: Math.round(performance.now() - started) });
        }
      };
      pc.createDataChannel("probe");
      pc.createOffer()
        .then((offer) => pc.setLocalDescription(offer))
        .catch(() => resolve({ state: "failed" }));
    });
  } finally {
    pc.close();
  }
}

function hostOf(url: string): string {
  return url.replace(/^turns?:/, "").replace(/\?.*$/, "");
}

function transportOf(url: string): string {
  const secure = url.startsWith("turns:");
  const transport = /transport=(\w+)/.exec(url)?.[1]?.toUpperCase() ?? "UDP";
  return `${secure ? "TLS" : transport}${secure ? " over TCP" : ""}`;
}

/**
 * The servers this app talks to: the message relay (this server's API),
 * and the TURN relays every call goes through. Neither can read anything:
 * messages and calls are encrypted end to end.
 */
export function RelayServers() {
  const [turn, setTurn] = useState<api.IceServerDto | null>(null);
  const [turnError, setTurnError] = useState<string | null>(null);
  const [apiCheck, setApiCheck] = useState<Check>({ state: "idle" });
  const [turnChecks, setTurnChecks] = useState<Record<string, Check>>({});

  useEffect(() => {
    Promise.resolve()
      .then(() => api.getRelays())
      .then((res) => setTurn(res?.iceServers?.[0] ?? null))
      .catch((err) => setTurnError(err instanceof api.ApiError && err.code === "calls_disabled" ? "No call relay is set up on this server, so calls are off." : "Couldn't load the call relays."));
  }, []);

  async function checkApi() {
    setApiCheck({ state: "checking" });
    const started = performance.now();
    try {
      await api.me();
      setApiCheck({ state: "ok", ms: Math.round(performance.now() - started) });
    } catch {
      setApiCheck({ state: "failed" });
    }
  }

  async function checkAll() {
    void checkApi();
    if (!turn) return;
    for (const url of turn.urls) {
      setTurnChecks((c) => ({ ...c, [url]: { state: "checking" } }));
      const result = await checkTurn(url, turn);
      setTurnChecks((c) => ({ ...c, [url]: result }));
    }
  }

  const apiStatus = describe(apiCheck);
  return (
    <>
      <p className={styles.lead}>
        Messages go through the message relay, and calls always go through a call relay, so nobody you talk to ever learns your IP
        address. Group calls go through this site's group call server the same way. Relays only ever carry ciphertext: they can't
        read messages or listen to calls.
      </p>
      <div>
        <button type="button" className={styles.smallButton} onClick={checkAll}>
          <RefreshIcon size={16} style={{ verticalAlign: "-3px", marginRight: 6 }} />
          Check connections
        </button>
      </div>
      <h3 className={`${styles.subhead} type-caption`}>MESSAGE RELAY</h3>
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.person}>
            <GlobeIcon size={20} />
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>{window.location.host}</span>
              <span className={styles.mono}>{API_BASE}</span>
            </span>
          </span>
          <span className={apiStatus.tone}>{apiStatus.text}</span>
        </div>
      </div>
      <h3 className={`${styles.subhead} type-caption`}>CALL RELAYS (TURN)</h3>
      <div className={styles.group}>
        {turnError && (
          <div className={styles.row}>
            <span className={styles.rowDetail}>{turnError}</span>
          </div>
        )}
        {turn?.urls.map((url) => {
          const status = describe(turnChecks[url] ?? { state: "idle" });
          return (
            <div key={url} className={styles.row}>
              <span className={styles.person}>
                <PhoneIcon size={20} />
                <span className={styles.rowText}>
                  <span className={styles.rowTitle}>{hostOf(url)}</span>
                  <span className={styles.rowDetail}>{transportOf(url)} · relay only</span>
                </span>
              </span>
              <span className={status.tone}>{status.text}</span>
            </div>
          );
        })}
      </div>
    </>
  );
}
