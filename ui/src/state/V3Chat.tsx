// Protocol-3 chat in the app (docs/multi-device-threat-model.md section 9):
// V3Receiver keeps this account's device mailbox polled while signed in, and
// useV3Chat gives one conversation its messages and its send. Both do nothing
// unless lib/e2eV3/session.ts finds protocol 3 usable in this browser.
import { useCallback, useEffect, useRef, useState } from "react";
import type { Conversation, Message, MessageReaction } from "../types.js";
import { chatMode, sendChatFile, sendChatOp, sendChatText, PinnedChatError, type ChatMessage, type ChatOp } from "../lib/e2eV3/chat.js";
import { deleteFile } from "../lib/api.js";
import type { FileDescriptor } from "../lib/fileMessage.js";
import { EngineError, keyProblemMessage } from "../lib/e2eV3/device.js";
import { announceChat, chatSession, closeChatSession, latestChats, onChatMessages, pollChat } from "../lib/e2eV3/session.js";
import { useAppState } from "./AppStateContext.js";
import { useLive } from "./LiveContext.js";

const POLL_MS = 5000;

/** Renders nothing: checks the device mailbox on start, on a live "v3-mailbox" event, and every 5 s. */
export function V3Receiver() {
  const { state } = useAppState();
  const live = useLive();
  const userId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;

  useEffect(() => {
    if (!userId) return;
    void pollChat(userId);
    const timer = setInterval(() => void pollChat(userId), POLL_MS);
    const unsubscribe = live.subscribe((event) => {
      if ((event as { type: string }).type === "v3-mailbox") void pollChat(userId);
    });
    return () => {
      clearInterval(timer);
      unsubscribe();
      closeChatSession();
    };
  }, [userId, live.subscribe]);
  return null;
}

export interface V3ChatView {
  /** Protocol 3 is usable here: messages in this chat travel by device keys. */
  active: boolean;
  /** "blocked": the chat used device keys and the other side has none now (nothing is sent). */
  mode: "v3" | "v1" | "blocked";
  messages: Message[];
  send(text: string): Promise<void>;
  sendFile(file: FileDescriptor): Promise<void>;
  /** Adds the emoji, or takes it away if it's already your reaction. */
  react(message: Message, emoji: string): Promise<void>;
  edit(messageId: string, text: string): Promise<void>;
  /** Deletes for everyone in the chat, and removes the file from the server if there is one. */
  remove(message: Message): Promise<void>;
  /** Sends a failed message again (as a new message) or drops it. */
  retry(id: string): Promise<void>;
  discard(id: string): Promise<void>;
  error: string | null;
  /** The person confirmed using the older protocol for this chat. */
  allowOlderProtocol(): Promise<void>;
}

export function toMessage(m: ChatMessage, selfUserId: string, group: boolean, dmUserId: string | null): Message {
  const mine = m.direction === "out";
  const reactions = new Map<string, { count: number; mine: boolean }>();
  for (const r of m.reactions ?? []) {
    const held = reactions.get(r.emoji) ?? { count: 0, mine: false };
    reactions.set(r.emoji, { count: held.count + 1, mine: held.mine || r.userId === selfUserId });
  }
  return {
    // Failed sends never reached anyone, so nothing can react to them or edit them.
    id: m.status === "failed" ? undefined : m.id,
    from: mine ? "me" : group ? m.fromUserId : (dmUserId ?? m.fromUserId),
    text: m.deleted ? "This message was deleted." : m.text,
    deleted: m.deleted ? true : undefined,
    editedAt: m.edited ? m.ts : undefined,
    file: m.file ? { descriptor: m.file, status: "available" } : undefined,
    reactions: [...reactions].map(([emoji, r]) => ({ emoji: emoji as MessageReaction["emoji"], count: r.count, mine: r.mine })),
    time: new Date(m.ts).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" }),
    createdAt: m.ts,
    v3: true,
    delivered: mine && m.status === "sent" ? true : undefined,
    // A failed send shows as "Not sent" with Retry and Discard, like an outbox message.
    ...(m.status === "failed" ? { queueId: m.id, queueHeld: true, queueError: "Not sent." } : {}),
  };
}

export function useV3Chat(conversation: Conversation | undefined): V3ChatView {
  const { state } = useAppState();
  const selfUserId = state.identity?.userId;
  const conversationId = conversation?.id;
  const isGroup = conversation?.kind === "group";
  const contactUserId = conversation?.kind === "dm" ? (conversation.userId ?? null) : null;
  const [active, setActive] = useState(false);
  const [mode, setMode] = useState<V3ChatView["mode"]>("v1");
  const [stored, setStored] = useState<ChatMessage[]>([]);
  const [error, setError] = useState<string | null>(null);
  const target = useRef({ conversationId, contactUserId, isGroup });
  target.current = { conversationId, contactUserId, isGroup };

  const reload = useCallback(async () => {
    const { conversationId: id } = target.current;
    if (!selfUserId || !id) return;
    const session = await chatSession(selfUserId);
    if (!session) {
      setActive(false);
      return;
    }
    setActive(true);
    setStored(await session.history.list(id));
    const { contactUserId: contact, isGroup: group } = target.current;
    if (group || contact) setMode(await chatMode(session.device, session.history, { conversationId: id, contactUserId: group ? null : contact }));
  }, [selfUserId]);

  useEffect(() => {
    setStored([]);
    setError(null);
    void reload().catch(() => setActive(false));
    return onChatMessages((fresh) => {
      if (fresh.some((m) => m.conversationId === target.current.conversationId)) void reload().catch(() => {});
    });
  }, [conversationId, reload]);

  const send = useCallback(
    async (text: string) => {
      const { conversationId: id, contactUserId: contact, isGroup: group } = target.current;
      if (!selfUserId || !id) return;
      const session = await chatSession(selfUserId);
      if (!session) throw new Error("Device keys aren't available in this browser.");
      setError(null);
      try {
        const sent = await sendChatText(session.device, session.history, { conversationId: id, contactUserId: group ? null : contact }, text);
        announceChat([sent]);
      } catch (err) {
        setError(
          err instanceof PinnedChatError
            ? err.message
            : (err instanceof EngineError ? keyProblemMessage(err.code) : null) ?? "That message wasn't sent. Check your connection and try again."
        );
        await reload().catch(() => {});
        throw err;
      }
      await reload();
    },
    [selfUserId, reload]
  );

  const withSession = useCallback(
    async (work: (session: NonNullable<Awaited<ReturnType<typeof chatSession>>>, conv: { conversationId: string; contactUserId: string | null }) => Promise<void>) => {
      const { conversationId: id, contactUserId: contact, isGroup: group } = target.current;
      if (!selfUserId || !id) return;
      const session = await chatSession(selfUserId);
      if (!session) throw new Error("Device keys aren't available in this browser.");
      setError(null);
      try {
        await work(session, { conversationId: id, contactUserId: group ? null : contact });
      } catch (err) {
        setError(
          err instanceof PinnedChatError
            ? err.message
            : (err instanceof EngineError ? keyProblemMessage(err.code) : null) ?? "That didn't go through. Check your connection and try again."
        );
        throw err;
      } finally {
        await reload().catch(() => {});
      }
    },
    [selfUserId, reload]
  );

  const sendOp = useCallback(
    (op: ChatOp) => withSession((session, conv) => sendChatOp(session.device, session.history, conv, op)),
    [withSession]
  );

  const sendFile = useCallback(
    async (file: FileDescriptor) => {
      await withSession(async (session, conv) => {
        await sendChatFile(session.device, session.history, conv, file);
      });
    },
    [withSession]
  );

  const react = useCallback(
    (message: Message, emoji: string) => {
      const mine = message.reactions?.some((r) => r.emoji === emoji && r.mine);
      return sendOp({ t: "reaction", target: message.id!, emoji: mine ? "" : emoji });
    },
    [sendOp]
  );

  const edit = useCallback((messageId: string, text: string) => sendOp({ t: "edit", target: messageId, body: text }), [sendOp]);

  const remove = useCallback(
    async (message: Message) => {
      await sendOp({ t: "delete", target: message.id! });
      if (message.file) await deleteFile(message.file.descriptor.objectId).catch(() => {});
    },
    [sendOp]
  );

  const discard = useCallback(
    async (id: string) => {
      const { conversationId: conv } = target.current;
      if (!selfUserId || !conv) return;
      const session = await chatSession(selfUserId);
      await session?.history.remove(conv, id);
      await reload();
    },
    [selfUserId, reload]
  );

  const retry = useCallback(
    async (id: string) => {
      const { conversationId: conv } = target.current;
      if (!selfUserId || !conv) return;
      const session = await chatSession(selfUserId);
      const failed = (await session?.history.list(conv))?.find((m) => m.id === id && m.status === "failed");
      if (!session || !failed) return;
      await session.history.remove(conv, id);
      if (failed.file) await sendFile(failed.file).catch(() => {});
      else await send(failed.text).catch(() => {});
    },
    [selfUserId, send, sendFile]
  );

  const allowOlderProtocol = useCallback(async () => {
    const { conversationId: id } = target.current;
    if (!selfUserId || !id) return;
    const session = await chatSession(selfUserId);
    await session?.history.unpin(id);
    setError(null);
    await reload();
  }, [selfUserId, reload]);

  const messages = selfUserId ? stored.map((m) => toMessage(m, selfUserId, isGroup, contactUserId)) : [];
  return { active, mode, messages, send, sendFile, react, edit, remove, retry, discard, error, allowOlderProtocol };
}

/** The newest protocol-3 message of each conversation, kept current, so the chat list can show it. */
export function useV3Latest(): Map<string, ChatMessage> {
  const { state } = useAppState();
  const userId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  const [latest, setLatest] = useState<Map<string, ChatMessage>>(new Map());
  useEffect(() => {
    if (!userId) return;
    let cancelled = false;
    const load = () =>
      latestChats(userId)
        .then((list) => {
          if (!cancelled) setLatest(new Map(list.map((m) => [m.conversationId, m])));
        })
        .catch(() => {});
    void load();
    const unsubscribe = onChatMessages(() => void load());
    return () => {
      cancelled = true;
      unsubscribe();
    };
  }, [userId]);
  return latest;
}
