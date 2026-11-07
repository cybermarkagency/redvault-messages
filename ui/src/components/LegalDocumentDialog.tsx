import { useEffect, useMemo, useRef } from "react";
import { LEGAL_DOCUMENTS, parseLegalDocument, type LegalDocumentId } from "../lib/legalDocuments.js";
import styles from "./LegalDocumentDialog.module.css";

export interface LegalDocumentDialogProps {
  document: LegalDocumentId;
  onClose: () => void;
}

/** The Terms of Service or Privacy Policy, read in place: closing returns to where you were. */
export function LegalDocumentDialog({ document: id, onClose }: LegalDocumentDialogProps) {
  const blocks = useMemo(() => parseLegalDocument(LEGAL_DOCUMENTS[id]), [id]);
  const title = blocks.find((b) => b.kind === "title");
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
      <div className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="legal-title" onClick={(e) => e.stopPropagation()}>
        <div className={styles.header}>
          <h2 id="legal-title">{title && title.kind === "title" ? title.text : ""}</h2>
          <button ref={closeButton} type="button" className={styles.close} aria-label="Close" onClick={onClose}>
            ×
          </button>
        </div>
        <div className={styles.body} tabIndex={0}>
          {blocks.map((block, i) => {
            switch (block.kind) {
              case "title":
                return null;
              case "heading":
                return <h3 key={i}>{block.text}</h3>;
              case "subheading":
                return <h4 key={i}>{block.text}</h4>;
              case "bullets":
                return (
                  <ul key={i}>
                    {block.items.map((item, j) => (
                      <li key={j}>{item}</li>
                    ))}
                  </ul>
                );
              case "numbered":
                return (
                  <ol key={i}>
                    {block.items.map((item, j) => (
                      <li key={j}>{item}</li>
                    ))}
                  </ol>
                );
              default:
                return <p key={i}>{block.text}</p>;
            }
          })}
        </div>
        <button type="button" className={styles.done} onClick={onClose}>
          Done
        </button>
      </div>
    </div>
  );
}
