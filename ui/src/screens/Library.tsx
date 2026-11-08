// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { PostCard } from "../components/earnings/PostCard.js";
import * as eapi from "../lib/earningsApi.js";
import styles from "./Communities.module.css";

/** Exclusive posts you bought (Communities > Library): readable once each creator's app has sent your key. */
export function Library() {
  const [posts, setPosts] = useState<eapi.PostDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    eapi.getLibrary().then(setPosts, () => setError("Your library isn't reachable right now."));
  }, []);
  if (error) return <p className={styles.error}>{error}</p>;
  if (!posts) return <p className={styles.muted}>Loading…</p>;
  return (
    <>
      <p className={styles.text}>
        Exclusive posts you bought. Each is encrypted with its own key, which only the creator's app can send you; if it hasn't arrived within 72 hours you're refunded
        automatically.
      </p>
      {posts.length === 0 && <div className={styles.empty}>Nothing here yet. Exclusive posts are on creators' profiles.</div>}
      <div className={styles.list}>
        {posts.map((p) => (
          <div key={p.id}>
            {p.creator && <span className={styles.muted}>{p.creator.displayName} · @{p.creator.handle}</span>}
            <PostCard post={p} />
          </div>
        ))}
      </div>
    </>
  );
}
