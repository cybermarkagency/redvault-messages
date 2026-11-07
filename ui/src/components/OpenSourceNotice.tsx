import { legalFileUrl, SOURCE_OFFER, type SourceOffer } from "../lib/sourceOffer.js";
import styles from "../screens/settings/SettingsPages.module.css";

export const OPEN_SOURCE_TITLE = "Open-Source Licenses and Source Code";

export interface OpenSourceNoticeProps {
  offer?: SourceOffer;
  /** The heading's id, for aria-labelledby (must be unique on the page). */
  headingId?: string;
  /** Leave out the heading when a surrounding dialog already shows the title (headingId then names that dialog's title). */
  hideHeading?: boolean;
}

/**
 * The AGPL-3.0-only notice and source offer (AGPL section 13). Shown in
 * Settings > About, and from the sign-in screens so it's reachable without
 * an account. The source link appears only when the build configures one.
 */
export function OpenSourceNotice({ offer = SOURCE_OFFER, headingId = "open-source-title", hideHeading = false }: OpenSourceNoticeProps) {
  const contact = offer.contact;
  return (
    <section aria-labelledby={headingId} style={{ display: "flex", flexDirection: "column", gap: 12 }}>
      {!hideHeading && (
        <h3 id={headingId} className={`${styles.subhead} type-caption`}>
          {OPEN_SOURCE_TITLE}
        </h3>
      )}
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Licence</span>
            <span className={styles.rowDetail}>
              RedVault Messages is free software, licensed under the GNU Affero General Public License, version 3 only (AGPL-3.0-only). It comes
              with no warranty.
            </span>
          </span>
        </div>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Version</span>
            <span className={`${styles.rowDetail} type-mono`}>RedVault Messages web {offer.version}</span>
          </span>
        </div>
        {offer.sourceUrl ? (
          <a className={styles.row} href={offer.sourceUrl} target="_blank" rel="noopener noreferrer">
            <span className={styles.rowTitle}>Source code for this version</span>
          </a>
        ) : (
          <div className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>Source code</span>
              <span className={styles.rowDetail}>The public source code link will appear here once it is published.</span>
            </span>
          </div>
        )}
        <a className={styles.row} href={legalFileUrl("LICENSE.txt")} target="_blank" rel="noopener noreferrer">
          <span className={styles.rowTitle}>Full licence text (GNU AGPL v3)</span>
        </a>
        <a className={styles.row} href={legalFileUrl("THIRD_PARTY_NOTICES.txt")} target="_blank" rel="noopener noreferrer">
          <span className={styles.rowTitle}>Third-party notices</span>
        </a>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Problems getting the source code</span>
            <span className={styles.rowDetail}>
              {contact === null ? (
                <>
                  Tell us using the contact address in the{" "}
                  <a className={styles.inlineLink} href="terms.html" target="_blank" rel="noopener noreferrer">
                    Terms of Service
                  </a>
                  .
                </>
              ) : contact.kind === "email" ? (
                <>
                  Tell us at <a className={styles.inlineLink} href={`mailto:${contact.address}`}>{contact.address}</a>.
                </>
              ) : contact.kind === "url" ? (
                <>
                  Tell us at{" "}
                  <a className={styles.inlineLink} href={contact.url} target="_blank" rel="noopener noreferrer">
                    {contact.url}
                  </a>
                  .
                </>
              ) : (
                <>Tell us: {contact.text}</>
              )}
            </span>
          </span>
        </div>
        <div className={styles.row}>
          <span className={styles.rowDetail}>RedVault Messages is not affiliated with or endorsed by Signal Messenger LLC.</span>
        </div>
      </div>
    </section>
  );
}
