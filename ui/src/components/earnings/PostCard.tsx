// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { openPost, PaymentNotCompleted, postMediaUrl, startCheckout, waitForPurchase } from "../../lib/earnings.js";
import * as eapi from "../../lib/earningsApi.js";
import { formatUsd, type PostDto } from "../../lib/earningsApi.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./Earnings.module.css";

/**
 * An exclusive post: its public teaser and price for everyone; its
 * encrypted body for its author and for buyers once the creator's app has
 * sealed them the key.
 */
export function PostCard({ post: initial, onChanged }: { post: PostDto; onChanged?: () => void }) {
  const { state } = useAppState();
  const identity = state.identity;
  const [post, setPost] = useState(initial);
  const [text, setText] = useState<string | null>(null);
  const [media, setMedia] = useState<{ url: string; mime: string } | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => setPost(initial), [initial]);

  useEffect(() => {
    let url: string | null = null;
    let alive = true;
    if (!identity || post.locked || !post.body) return;
    const known = new Map<string, Uint8Array>();
    for (const c of state.conversations) if (c.kind === "dm" && c.userId && c.publicKey) known.set(c.userId, c.publicKey);
    openPost(identity, post, known)
      .then(async (opened) => {
        if (!alive) return;
        setText(opened.body.text);
        if (opened.body.media) {
          url = await postMediaUrl(post.id, opened.body);
          if (alive && url) setMedia({ url, mime: opened.body.media.mime });
        }
      })
      .catch(() => alive && setError("This post couldn't be opened."));
    return () => {
      alive = false;
      if (url) URL.revokeObjectURL(url);
    };
  }, [identity, post, state.conversations]);

  async function buy() {
    setError(null);
    setStatus("Opening the payment page…");
    try {
      const started = await startCheckout({ product: "exclusive_post", postId: post.id });
      setStatus("Waiting for the payment provider to confirm…");
      await waitForPurchase(started.purchaseId);
      setStatus("Paid. Your key arrives from the creator's app shortly.");
      setPost(await eapi.getPost(post.id));
      onChanged?.();
    } catch (err) {
      setStatus(null);
      setError(err instanceof PaymentNotCompleted || err instanceof Error ? err.message : "Couldn't buy this post.");
    }
  }

  async function unpublish() {
    if (!window.confirm("Stop selling this post? People who bought it keep it.")) return;
    await eapi.unpublishPost(post.id).catch(() => undefined);
    onChanged?.();
  }

  return (
    <article className={styles.post} aria-label="Exclusive post">
      <div className={styles.postHead}>
        <span className={styles.price}>{formatUsd(post.priceMinor)}</span>
        {post.mine && <span className={styles.badge}>{post.status === "published" ? "On sale" : post.status === "unpublished" ? "Off sale" : post.status}</span>}
        {post.owned && <span className={styles.badgeAccent}>Bought</span>}
      </div>
      {post.teaser && (
        <p className={styles.teaser}>
          <span className={styles.label}>Public teaser</span> {post.teaser}
        </p>
      )}
      {!post.locked && text !== null && <p className={styles.postText}>{text}</p>}
      {media &&
        (media.mime.startsWith("video/") ? (
          <video className={styles.postMedia} src={media.url} controls playsInline controlsList="nodownload" />
        ) : (
          <img className={styles.postMedia} src={media.url} alt="" />
        ))}
      {post.locked && !post.mine && (
        <div className={styles.locked}>
          {post.awaitingKey ? (
            <span>Bought. Your key arrives from the creator's app shortly (or you're refunded after 72 hours).</span>
          ) : (
            <>
              <span>Encrypted. Buy it to read it.</span>
              <button type="button" className={styles.primary} onClick={buy} disabled={status !== null && !error}>
                Buy for {formatUsd(post.priceMinor)}
              </button>
            </>
          )}
        </div>
      )}
      {post.mine && post.status === "published" && (
        <button type="button" className={styles.link} onClick={unpublish}>
          Stop selling
        </button>
      )}
      {status && <p className={styles.muted}>{status}</p>}
      {error && <p className={styles.error}>{error}</p>}
    </article>
  );
}
