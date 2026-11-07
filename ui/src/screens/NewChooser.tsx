import { useState } from "react";
import { PlusIcon, ScanIcon } from "../components/icons.js";
import { LinkIcon } from "../components/icons24.js";
import { TopBar } from "../components/TopBar.js";
import { parseGroupInvite } from "../lib/groupInviteCrypto.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./NewChooser.module.css";

export function NewChooser() {
  const { dispatch } = useAppState();
  const [pasting, setPasting] = useState(false);
  const [text, setText] = useState("");
  const [error, setError] = useState<string | null>(null);

  function joinWithLink(e: React.FormEvent) {
    e.preventDefault();
    const invite = parseGroupInvite(text);
    if (!invite) {
      setError("That doesn't look like a group invite link. Paste the whole link, including the part after #.");
      return;
    }
    dispatch({ type: "OPEN_GROUP_INVITE", linkId: invite.linkId, secret: invite.secret });
  }

  return (
    <div className={styles.screen}>
      <TopBar title="New" onBack={() => dispatch({ type: "GO_CHATS" })} />
      <div className={styles.options}>
        <button type="button" className={styles.option} onClick={() => dispatch({ type: "OPEN_NEW_CONTACT" })}>
          <span className={styles.optionIcon}>
            <ScanIcon />
          </span>
          <span>
            <div className={`${styles.optionTitle} type-row-primary`}>Add friend</div>
            <div className={`${styles.optionDesc} type-secondary`}>Look up their handle</div>
          </span>
        </button>
        <button type="button" className={styles.option} onClick={() => dispatch({ type: "OPEN_NEW_GROUP" })}>
          <span className={styles.optionIcon}>
            <PlusIcon />
          </span>
          <span>
            <div className={`${styles.optionTitle} type-row-primary`}>New group</div>
            <div className={`${styles.optionDesc} type-secondary`}>Pick from your existing contacts</div>
          </span>
        </button>
        <button type="button" className={styles.option} aria-expanded={pasting} onClick={() => setPasting((p) => !p)}>
          <span className={styles.optionIcon}>
            <LinkIcon />
          </span>
          <span>
            <div className={`${styles.optionTitle} type-row-primary`}>Join a group with a link</div>
            <div className={`${styles.optionDesc} type-secondary`}>Paste an invite link someone shared with you</div>
          </span>
        </button>
        {pasting && (
          <form className={styles.pasteForm} onSubmit={joinWithLink}>
            <input
              className={styles.pasteInput}
              value={text}
              onChange={(e) => {
                setText(e.target.value);
                setError(null);
              }}
              placeholder="Paste the invite link"
              aria-label="Group invite link"
              autoFocus
            />
            <button type="submit" className={styles.pasteButton} disabled={!text.trim()}>
              Open
            </button>
            {error && (
              <p className={styles.pasteError} role="alert">
                {error}
              </p>
            )}
          </form>
        )}
      </div>
    </div>
  );
}
