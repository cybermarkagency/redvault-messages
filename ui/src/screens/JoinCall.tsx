import { useEffect, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { ChevronLeftIcon, LockIcon, PhoneIcon, VideoIcon } from "../components/icons24.js";
import * as api from "../lib/api.js";
import { openLinkName, secretFromFragment } from "../lib/callLinkCrypto.js";
import { ready } from "../lib/e2eCrypto.js";
import { useAppState } from "../state/AppStateContext.js";
import { useCall } from "../state/CallContext.js";
import styles from "./JoinCall.module.css";

interface LinkPreview {
  name: string;
  kind: api.CallKind;
  ownerId: string;
  ownerHandle: string;
  ownerAvatarUrl: string | null;
  secret: Uint8Array;
}

/** Someone opened a call link: who's hosting, what it's called, and "Ask to join". */
export function JoinCall({ linkId, secret }: { linkId: string; secret: string }) {
  const { state, dispatch } = useAppState();
  const calls = useCall();
  const [preview, setPreview] = useState<LinkPreview | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      await ready();
      const key = secretFromFragment(secret);
      if (!key) {
        setError("This link is incomplete. Ask for it again, and open the whole link.");
        return;
      }
      try {
        const info = await api.getCallLinkInfo(linkId);
        let name: string;
        try {
          name = openLinkName(info.nameCiphertext, info.nameNonce, key);
        } catch {
          if (!cancelled) setError("This link's secret doesn't match. It may have been changed: ask for it again.");
          return;
        }
        const owner = await api.getUser(info.ownerId);
        if (!cancelled) {
          setPreview({ name, kind: info.kind, ownerId: info.ownerId, ownerHandle: owner.username, ownerAvatarUrl: owner.avatarUrl, secret: key });
        }
      } catch (err) {
        if (!cancelled) setError(err instanceof api.ApiError && err.status === 404 ? "This call link doesn't exist or was deleted." : "Couldn't open this link. Try again.");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [linkId, secret]);

  const own = preview?.ownerId === state.identity?.userId;
  const busy = calls.call !== null && calls.call.phase !== "ended";

  return (
    <div className={styles.screen}>
      <button type="button" className={styles.back} aria-label="Back" onClick={() => dispatch({ type: "OPEN_CALLS" })}>
        <ChevronLeftIcon />
      </button>
      <div className={styles.card}>
        {error ? (
          <p className={styles.error} role="alert">
            {error}
          </p>
        ) : !preview ? (
          <p className={styles.muted}>Opening the link…</p>
        ) : (
          <>
            <Avatar avatarUrl={preview.ownerAvatarUrl} seed={preview.ownerHandle} size={88} />
            <h1 className={`${styles.title} type-display`}>{preview.name}</h1>
            <p className={styles.muted}>
              {preview.kind === "video" ? "Video call" : "Voice call"} hosted by <strong>@{preview.ownerHandle}</strong>
            </p>
            <p className={styles.note}>
              <LockIcon size={14} /> {preview.ownerHandle} will be asked to let you in. The call is end-to-end encrypted with this link's
              secret, which never reaches the server.
            </p>
            {own ? (
              <p className={styles.muted}>This is your own link: share it with the person you want to call.</p>
            ) : !calls.callsAvailable ? (
              <p className={styles.muted}>Calling isn't available on this server right now.</p>
            ) : (
              <button
                type="button"
                className={styles.join}
                disabled={busy}
                onClick={() =>
                  calls.joinLink({
                    id: linkId,
                    secret: preview.secret,
                    name: preview.name,
                    kind: preview.kind,
                    ownerId: preview.ownerId,
                    ownerHandle: preview.ownerHandle,
                    ownerAvatarUrl: preview.ownerAvatarUrl,
                  })
                }
              >
                {preview.kind === "video" ? <VideoIcon size={18} /> : <PhoneIcon size={18} />} Ask to join
              </button>
            )}
          </>
        )}
      </div>
    </div>
  );
}
