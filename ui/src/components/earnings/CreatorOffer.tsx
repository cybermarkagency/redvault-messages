import { useEffect, useState } from "react";
import { ApiError } from "../../lib/api.js";
import * as eapi from "../../lib/earningsApi.js";
import { formatUsd, type OfferDto, type PostDto } from "../../lib/earningsApi.js";
import { PostCard } from "./PostCard.js";
import { TipDialog } from "./TipDialog.js";
import { startMessageRequest } from "../../state/actions.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./Earnings.module.css";

/** On a creator's profile: a tip button, what messaging them costs, and their exclusive posts. Nothing for blocked pairs. */
export function CreatorOffer({ userId, name }: { userId: string; name: string }) {
  const [offer, setOffer] = useState<OfferDto | null>(null);
  const [posts, setPosts] = useState<PostDto[]>([]);
  const [tipping, setTipping] = useState(false);
  const { state, dispatch } = useAppState();
  const [starting, setStarting] = useState(false);
  const [startError, setStartError] = useState<string | null>(null);
  // Someone who charges for messages can be messaged without being friends; an existing chat just opens.
  const existingChat = state.conversations.find((c) => c.kind === "dm" && c.userId === userId);

  useEffect(() => {
    let alive = true;
    eapi
      .getOffer(userId)
      .then((o) => {
        if (!alive) return;
        setOffer(o);
        if (o.available && o.posts > 0) eapi.listCreatorPosts(userId).then((p) => alive && setPosts(p), () => undefined);
      })
      .catch(() => alive && setOffer(null));
    return () => {
      alive = false;
    };
  }, [userId]);

  if (!offer?.available) return null;
  return (
    <section className={styles.offer} aria-label="Creator">
      <div className={styles.offerHead}>
        <span className={styles.label}>CREATOR</span>
        <span className={styles.muted}>@{offer.creator?.handle}</span>
      </div>
      {offer.paidMessages && (
        <p className={styles.muted}>
          {offer.paidMessages.mode === "first_contact"
            ? `Starting a chat costs ${formatUsd(offer.paidMessages.priceMinor)} unless you're friends.`
            : `Each message costs ${formatUsd(offer.paidMessages.priceMinor)} unless you're friends.`}
        </p>
      )}
      {offer.paidMessages && (
        <button
          type="button"
          className={styles.secondary}
          disabled={starting}
          onClick={async () => {
            if (existingChat) {
              dispatch({ type: "OPEN_THREAD", conversationId: existingChat.id });
              return;
            }
            if (state.authStatus !== "signed-in" || !state.identity) return;
            setStarting(true);
            setStartError(null);
            try {
              await startMessageRequest(dispatch, state.identity, userId, state.settings.defaultDisappearing);
            } catch (err) {
              setStartError(err instanceof ApiError ? err.message : "Couldn't start a chat. Try again.");
            } finally {
              setStarting(false);
            }
          }}
        >
          Message {name}
        </button>
      )}
      {startError && <span className={styles.error}>{startError}</span>}
      {offer.tips && (
        <button type="button" className={styles.primary} onClick={() => setTipping(true)}>
          Tip {name}
        </button>
      )}
      {posts.length > 0 && (
        <>
          <span className={styles.label}>EXCLUSIVE POSTS</span>
          <div className={styles.posts}>
            {posts.map((p) => (
              <PostCard key={p.id} post={p} />
            ))}
          </div>
        </>
      )}
      {tipping && <TipDialog creatorUserId={userId} name={name} onClose={() => setTipping(false)} />}
    </section>
  );
}
