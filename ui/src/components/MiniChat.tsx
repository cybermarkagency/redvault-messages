// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { refreshDmMessages, sendDmMessage } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { DURATION_LABELS, type DMConversation } from "../types.js";
import { FileBubble } from "./FileBubble.js";
import { CloseIcon, LockIcon, SendIcon } from "./icons24.js";
import styles from "./MiniChat.module.css";

/** The in-call chat panel: the same encrypted 1:1 chat, in a narrow column beside the call. */
export function MiniChat({ conversation, onClose }: { conversation: DMConversation; onClose: () => void }) {
  const { state, dispatch } = useAppState();
  const [draft, setDraft] = useState("");
  const [sending, setSending] = useState(false);
  const end = useRef<HTMLDivElement>(null);
  const firstName = conversation.handle.split(/[\s.·]/)[0];

  // The thread screen isn't open during a call, so poll here instead.
  useEffect(() => {
    if (!conversation.conversationKey || !conversation.userId || !state.identity) return;
    const key = conversation.conversationKey;
    const tick = () =>
      refreshDmMessages(dispatch, conversation.id, key, state.identity!.userId, [conversation.userId!], state.settings.readReceipts).catch(() => {});
    tick();
    const timer = setInterval(tick, 3000);
    return () => clearInterval(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [conversation.id]);

  useEffect(() => {
    end.current?.scrollIntoView?.({ block: "end" });
  }, [conversation.messages.length]);

  async function send() {
    if (!draft.trim() || sending || !conversation.conversationKey) return;
    setSending(true);
    try {
      await sendDmMessage(dispatch, conversation.id, conversation.conversationKey, draft);
      setDraft("");
    } finally {
      setSending(false);
    }
  }

  const timer =
    conversation.disappearing === "off" ? "" : ` and follow its ${DURATION_LABELS[conversation.disappearing].toLowerCase()} disappearing timer`;

  return (
    <aside className={styles.panel} aria-label="In-call chat">
      <header className={styles.header}>
        <div>
          <div className={styles.title}>In-call chat</div>
          <div className={styles.sub}>
            <LockIcon size={12} /> End-to-end encrypted
          </div>
        </div>
        <button type="button" className={styles.close} aria-label="Close chat" onClick={onClose}>
          <CloseIcon size={18} />
        </button>
      </header>
      <div className={styles.messages}>
        <p className={styles.note}>
          Messages here are saved to your chat with {firstName}
          {timer}.
        </p>
        {conversation.messages.slice(-60).map((m, i) => (
          <div key={m.id ?? i} className={`${styles.bubble} ${m.from === "me" ? styles.mine : styles.theirs}`}>
            {m.file ? <FileBubble file={m.file} sent={m.from === "me"} /> : <span className={styles.text}>{m.text}</span>}
            <span className={styles.time}>{m.time}</span>
          </div>
        ))}
        <div ref={end} />
      </div>
      <div className={styles.composer}>
        <input
          placeholder={`Message ${firstName}`}
          value={draft}
          aria-label={`Message ${firstName}`}
          onChange={(e) => setDraft(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") void send();
          }}
        />
        <button type="button" className={styles.send} aria-label="Send in-call message" onClick={send} disabled={!draft.trim() || sending}>
          <SendIcon size={18} />
        </button>
      </div>
    </aside>
  );
}
