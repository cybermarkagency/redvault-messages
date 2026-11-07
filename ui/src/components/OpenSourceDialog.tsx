import { useEffect, useRef, useState } from "react";
import { SOURCE_OFFER, type SourceOffer } from "../lib/sourceOffer.js";
import styles from "./LegalDocumentDialog.module.css";
import { OPEN_SOURCE_TITLE, OpenSourceNotice } from "./OpenSourceNotice.js";

/** The open-source notice, read in place from the sign-in screens (no account needed). */
export function OpenSourceDialog({ onClose, offer = SOURCE_OFFER }: { onClose: () => void; offer?: SourceOffer }) {
  const closeButton = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null;
    closeButton.current?.focus();
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("keydown", onKey);
      previous?.focus?.();
    };
  }, [onClose]);

  return (
    <div className={styles.backdrop} onClick={onClose}>
      <div className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="open-source-dialog-title" onClick={(e) => e.stopPropagation()}>
        <div className={styles.header}>
          <h2 id="open-source-dialog-title">{OPEN_SOURCE_TITLE}</h2>
          <button ref={closeButton} type="button" className={styles.close} aria-label="Close" onClick={onClose}>
            ×
          </button>
        </div>
        <div className={styles.body} tabIndex={0}>
          <OpenSourceNotice offer={offer} headingId="open-source-dialog-title" hideHeading />
        </div>
        <button type="button" className={styles.done} onClick={onClose}>
          Done
        </button>
      </div>
    </div>
  );
}

/**
 * "Source code" in the sign-in screens' footer: straight to the published
 * source when the build has its URL, otherwise the notice above (which says
 * the link isn't published yet, and links the licence and notices).
 */
export function SourceCodeLink({ className, offer = SOURCE_OFFER }: { className?: string; offer?: SourceOffer }) {
  const [open, setOpen] = useState(false);
  if (offer.sourceUrl) {
    return (
      <a className={className} href={offer.sourceUrl} target="_blank" rel="noopener noreferrer">
        Source code
      </a>
    );
  }
  return (
    <>
      <button type="button" className={className} onClick={() => setOpen(true)}>
        Source code
      </button>
      {open && <OpenSourceDialog offer={offer} onClose={() => setOpen(false)} />}
    </>
  );
}
