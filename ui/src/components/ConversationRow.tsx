import { toPlain } from "../lib/textFormat.js";
import type { Conversation, DMConversation, Message } from "../types.js";
import { Avatar } from "./Avatar.js";
import { GroupGlyph } from "./GroupGlyph.js";
import { BellOffIcon, TimerIcon } from "./icons24.js";
import styles from "./ConversationRow.module.css";

export interface ConversationRowProps {
  conversation: Conversation;
  active?: boolean;
  contactsById: Map<string, DMConversation>;
  online?: boolean;
  typing?: boolean;
  muted?: boolean;
  onOpen: (id: string) => void;
}

/** The last message as one line: formatting markers dropped and spoilers hidden, like a notification would. */
function previewOf(last: Message | undefined): string {
  if (!last) return "No messages yet";
  const you = last.from === "me";
  if (last.file) return last.file.descriptor.mimeType.startsWith("audio/") ? "Voice message" : last.file.descriptor.filename;
  if (last.notice) {
    const what = last.notice.kind === "pin" ? `pinned “${toPlain(last.text)}”` : "unpinned a message";
    return you ? `You ${what}` : what.charAt(0).toUpperCase() + what.slice(1);
  }
  const text = last.poll ? `📊 ${last.text}` : toPlain(last.text);
  return you && text ? `You: ${text}` : text || "No messages yet";
}

export function ConversationRow({ conversation, active, online, typing, muted, onOpen }: ConversationRowProps) {
  const isGroup = conversation.kind === "group";
  const name = isGroup ? conversation.label : conversation.handle;
  const lastMessage = conversation.messages[conversation.messages.length - 1];
  const hidePreview = conversation.disappearing !== "off";
  const unread = muted ? 0 : conversation.unread;

  const preview = typing ? (
    <span className={styles.typing}>typing…</span>
  ) : hidePreview ? (
    <>
      <TimerIcon size={14} />
      New message
    </>
  ) : (
    previewOf(lastMessage)
  );

  return (
    <button
      type="button"
      className={`${styles.row} ${active ? styles.rowActive : ""}`}
      aria-current={active ? "true" : undefined}
      onClick={() => onOpen(conversation.id)}
    >
      {isGroup ? (
        <GroupGlyph label={conversation.label} memberSeeds={conversation.members.filter((m) => !m.isSelf).map((m) => m.handle)} size={46} />
      ) : (
        <Avatar avatarUrl={conversation.avatarUrl} seed={conversation.handle} size={46} online={online} />
      )}
      <div className={styles.body}>
        <div className={styles.topLine}>
          <span className={styles.name}>{name}</span>
          {muted && <BellOffIcon size={14} className={styles.mutedIcon} aria-label="Muted" />}
          <span className={`${styles.time} ${unread > 0 ? styles.timeUnread : ""}`}>{conversation.time}</span>
        </div>
        <div className={styles.bottomLine}>
          <span className={styles.preview}>{preview}</span>
          {unread > 0 && <span className={styles.unread}>{unread}</span>}
        </div>
      </div>
    </button>
  );
}
