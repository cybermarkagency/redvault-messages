// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type ChangeEvent, type ReactNode, useEffect, useRef, useState } from "react";
import { BellIcon, GlobeIcon, HeadphonesIcon, InfoIcon, LogIcon, MonitorIcon, ShieldIcon, StoriesIcon, UserIcon } from "../components/icons24.js";
import { DurationSelect } from "../components/DurationSelect.js";
import { Avatar } from "../components/Avatar.js";
import { ToggleSwitch } from "../components/ToggleSwitch.js";
import { TopBar } from "../components/TopBar.js";
import { useIsDesktopLayout } from "../hooks/useMediaQuery.js";
import { resolveApiUrl } from "../lib/apiBase.js";
import { readImageAsAvatar } from "../lib/avatar.js";
import { writePrefs } from "../lib/localPrefs.js";
import { getFileStorageUsage, type FileStorageUsageDto } from "../lib/api.js";
import { formatBytes } from "../lib/fileMessage.js";
import {
  logout,
  removeAvatar,
  updateAcceptMessageRequests,
  updateShowOnlineStatus,
  updateTransparencyMode,
  uploadAvatar,
} from "../state/actions.js";
import type { SettingsSection } from "../state/reducer.js";
import { AboutSettings, CallsAudioSettings, NotificationsSettings } from "./settings/GeneralSettings.js";
import { StorySettings } from "./settings/StorySettings.js";
import { LinkedDevices } from "./settings/LinkedDevices.js";
import { PlanPicker } from "./settings/PlanPicker.js";
import { RelayServers } from "./settings/RelayServers.js";
import { NetworkRouting } from "./settings/NetworkRouting.js";
import { TransparencyLog } from "./settings/TransparencyLog.js";
import { useAppState } from "../state/AppStateContext.js";
import { useStoriesOptional } from "../state/StoriesContext.js";
import { tierAtLeast } from "../types.js";
import styles from "./Settings.module.css";

/** Account and plan (part "account"), privacy and security (part "privacy"), or both stacked (a phone). */
function AccountAndPrivacy({ part }: { part: "account" | "privacy" | "both" }) {
  const { state, dispatch } = useAppState();
  const { settings, identity } = state;
  const showAccount = part !== "privacy";
  const showPrivacy = part !== "account";
  const [onlineError, setOnlineError] = useState<string | null>(null);
  const [avatarBusy, setAvatarBusy] = useState(false);
  const [avatarError, setAvatarError] = useState<string | null>(null);
  const [requestsSettingError, setRequestsSettingError] = useState<string | null>(null);
  const [transparencyError, setTransparencyError] = useState<string | null>(null);
  // Null while loading or when file sharing isn't configured on this
  // server at all — either way, the row below just doesn't render, rather
  // than showing a scary error for something that isn't the user's fault.
  const [storageUsage, setStorageUsage] = useState<FileStorageUsageDto | null>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    let cancelled = false;
    getFileStorageUsage()
      .then((usage) => {
        if (!cancelled) setStorageUsage(usage);
      })
      .catch(() => {
        /* file sharing not configured, or a transient error — just omit the row */
      });
    return () => {
      cancelled = true;
    };
  }, []);

  if (!identity) return null;
  const paidTier = tierAtLeast(identity.subscriptionTier, "pro");

  async function handleFileChange(e: ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    e.target.value = ""; // allow re-selecting the same file later
    if (!file) return;
    setAvatarBusy(true);
    try {
      const imageBase64 = await readImageAsAvatar(file);
      await uploadAvatar(dispatch, imageBase64);
      setAvatarError(null);
    } catch (err) {
      setAvatarError(err instanceof Error ? err.message : "Couldn't use that image.");
    } finally {
      setAvatarBusy(false);
    }
  }

  async function handleRemoveAvatar() {
    setAvatarBusy(true);
    try {
      await removeAvatar(dispatch);
      setAvatarError(null);
    } catch {
      setAvatarError("Couldn't remove that photo — try again.");
    } finally {
      setAvatarBusy(false);
    }
  }

  async function handleToggleAcceptMessageRequests(value: boolean) {
    setRequestsSettingError(null);
    try {
      await updateAcceptMessageRequests(dispatch, value);
    } catch {
      setRequestsSettingError("Couldn't update that setting — try again.");
    }
  }

  async function handleToggleOnline(value: boolean) {
    setOnlineError(null);
    try {
      await updateShowOnlineStatus(dispatch, value);
    } catch {
      setOnlineError("Couldn't update that setting — try again.");
    }
  }

  async function handleToggleTransparencyMode(value: boolean) {
    setTransparencyError(null);
    try {
      await updateTransparencyMode(dispatch, value);
    } catch {
      setTransparencyError("Couldn't update that setting — try again.");
    }
  }

  return (
    <>
      {showAccount && (
        <>
        <div className={styles.identityCard}>
          {identity.avatarUrl ? (
            <img src={resolveApiUrl(identity.avatarUrl)} alt="" referrerPolicy="no-referrer" className={styles.avatarImg} width={56} height={56} />
          ) : (
            <Avatar seed={identity.handle} size={56} />
          )}
          <span className={`${styles.identityHandle} type-mono`}>{identity.handle}</span>
        </div>

        <div className={styles.avatarActions}>
          <input
            ref={fileInputRef}
            type="file"
            accept="image/*"
            onChange={handleFileChange}
            disabled={avatarBusy}
            className={styles.avatarFileInput}
          />
          <button
            type="button"
            className={styles.avatarButton}
            disabled={avatarBusy}
            onClick={() => fileInputRef.current?.click()}
          >
            {avatarBusy ? "Uploading…" : identity.avatarUrl ? "Change photo" : "Add photo"}
          </button>
          {identity.avatarUrl && (
            <button type="button" className={styles.avatarButtonSecondary} disabled={avatarBusy} onClick={handleRemoveAvatar}>
              Remove
            </button>
          )}
        </div>
        {avatarError && <p className={styles.avatarError}>{avatarError}</p>}
        <p className={`${styles.avatarHint} type-secondary`}>Visible to every user — not encrypted, not private.</p>
        </>
      )}

      {showPrivacy && (
        <>
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>PRIVACY DEFAULTS</span>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Read receipts</div>
              <div className={`${styles.rowDesc} type-secondary`}>Let contacts see when you've read a message</div>
            </span>
            <ToggleSwitch
              label="Read receipts"
              checked={settings.readReceipts}
              onChange={(v) => dispatch({ type: "UPDATE_SETTINGS", settings: { readReceipts: v } })}
            />
          </div>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Typing indicators</div>
              <div className={`${styles.rowDesc} type-secondary`}>Show contacts when you're typing</div>
            </span>
            <ToggleSwitch
              label="Typing indicators"
              checked={settings.typingIndicators}
              onChange={(v) => {
                if (state.identity) writePrefs(state.identity.userId, { typingIndicators: v });
                dispatch({ type: "UPDATE_SETTINGS", settings: { typingIndicators: v } });
              }}
            />
          </div>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Show when I'm online</div>
              <div className={`${styles.rowDesc} type-secondary`}>
                Contacts who share theirs see when you're online, and you see theirs. Never stored; off hides both.
              </div>
            </span>
            <ToggleSwitch label="Show when I'm online" checked={settings.showOnlineStatus} onChange={handleToggleOnline} />
          </div>
          {onlineError && <p className={styles.avatarError}>{onlineError}</p>}
          <div className={styles.row} style={{ flexDirection: "column", alignItems: "stretch", gap: 8 }}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Default disappearing messages</div>
              <div className={`${styles.rowDesc} type-secondary`}>Applies only to conversations created from now on</div>
            </span>
            <DurationSelect
              value={settings.defaultDisappearing}
              onChange={(v) => dispatch({ type: "UPDATE_SETTINGS", settings: { defaultDisappearing: v } })}
            />
          </div>
        </div>

        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>FRIEND REQUESTS</span>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Accept messages from anyone</div>
              <div className={`${styles.rowDesc} type-secondary`}>
                Off rejects new friend requests from people you haven't talked to before
              </div>
            </span>
            <ToggleSwitch
              label="Accept messages from anyone"
              checked={settings.acceptMessageRequests}
              onChange={handleToggleAcceptMessageRequests}
            />
          </div>
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: "OPEN_FRIEND_REQUESTS" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Friend requests</div>
              <div className={`${styles.rowDesc} type-secondary`}>Review pending requests, accept or deny them</div>
            </span>
            {state.friendRequestsIncoming.length > 0 && (
              <span className={styles.requestsBadge}>{state.friendRequestsIncoming.length}</span>
            )}
          </button>
        </div>
        {requestsSettingError && <p className={styles.avatarError}>{requestsSettingError}</p>}

        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>TRANSPARENCY</span>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Transparency mode</div>
              <div className={`${styles.rowDesc} type-secondary`}>
                When on, your outgoing messages' encrypted ciphertext (never the readable content) appears in the
                public live transparency feed. Off by default — this is entirely your choice.
              </div>
            </span>
            <ToggleSwitch
              label="Transparency mode"
              checked={settings.transparencyMode}
              onChange={handleToggleTransparencyMode}
            />
          </div>
          {transparencyError && <p className={styles.avatarError}>{transparencyError}</p>}
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: "OPEN_TRANSPARENCY_FEED", returnTo: "settings" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>View live transparency feed</div>
              <div className={`${styles.rowDesc} type-secondary`}>See the public record of relayed messages</div>
            </span>
          </button>
        </div>

        </>
      )}

      {showAccount && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>ACCOUNT</span>
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: "OPEN_CHANGE_USERNAME" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Handle</div>
              <div className={`${styles.rowDesc} type-secondary`}>{identity.handle}</div>
            </span>
          </button>
          {storageUsage && paidTier && (
            <div className={styles.row} style={{ flexDirection: "column", alignItems: "stretch", gap: 4 }}>
              <span>
                <div className={`${styles.rowTitle} type-row-primary`}>File storage</div>
                <div className={`${styles.rowDesc} type-secondary`}>
                  {formatBytes(storageUsage.used)} of {formatBytes(storageUsage.limit)} used ·{" "}
                  {formatBytes(Math.max(0, storageUsage.limit - storageUsage.used))} left
                </div>
              </span>
              <div className={styles.storageBar}>
                <div
                  className={`${styles.storageBarFill} ${
                    storageUsage.used >= storageUsage.limit ? styles.storageBarFillFull : ""
                  }`}
                  style={{
                    width: `${Math.min(100, storageUsage.limit > 0 ? (storageUsage.used / storageUsage.limit) * 100 : 0)}%`,
                  }}
                />
              </div>
            </div>
          )}
        </div>
      )}

      {showAccount && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>PLAN</span>
          <PlanPicker />
        </div>
      )}

      {showPrivacy && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>SECURITY</span>
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: "OPEN_RECOVERY_KEY" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Recovery key</div>
              <div className={`${styles.rowDesc} type-secondary`}>The only way back in if you forget your password</div>
            </span>
          </button>
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: "OPEN_CHANGE_PASSWORD" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Change password</div>
              <div className={`${styles.rowDesc} type-secondary`}>Re-wraps your private key with the new password</div>
            </span>
          </button>
          <button
            type="button"
            className={styles.row}
            style={{ width: "100%", textAlign: "left", border: "none" }}
            onClick={() => dispatch({ type: identity.mfaEnabled ? "OPEN_MFA_DISABLE" : "OPEN_MFA_SETUP" })}
          >
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Two-factor authentication</div>
              <div className={`${styles.rowDesc} type-secondary`}>
                {identity.mfaEnabled ? "Enabled — turn off" : "Off — set up an authenticator app"}
              </div>
            </span>
          </button>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>App lock</div>
              <div className={`${styles.rowDesc} type-secondary`}>Require unlock after backgrounding or timeout</div>
            </span>
            <ToggleSwitch
              label="App lock"
              checked={settings.appLock}
              onChange={(v) => dispatch({ type: "UPDATE_SETTINGS", settings: { appLock: v } })}
            />
          </div>
          <div className={styles.row}>
            <span>
              <div className={`${styles.rowTitle} type-row-primary`}>Blur in app switcher</div>
              <div className={`${styles.rowDesc} type-secondary`}>Hide previews in recents / the app switcher</div>
            </span>
            <ToggleSwitch
              label="Blur in app switcher"
              checked={settings.switcherBlur}
              onChange={(v) => dispatch({ type: "UPDATE_SETTINGS", settings: { switcherBlur: v } })}
            />
          </div>
        </div>
      )}

      {showAccount && (
        <div className={styles.buttonRow}>
          <button type="button" className={styles.lockNowButton} onClick={() => dispatch({ type: "LOCK" })}>
            Lock now
          </button>
          <button type="button" className={styles.lockNowButton} onClick={() => logout(dispatch)}>
            Log out
          </button>
          <button type="button" className={styles.lockNowButton} onClick={() => dispatch({ type: "OPEN_DELETE_ACCOUNT" })}>
            Delete account
          </button>
        </div>
      )}
    </>
  );
}

const SECTION_TITLES: Record<SettingsSection, string> = {
  account: "Account",
  privacy: "Privacy and security",
  stories: "Stories",
  notifications: "Notifications",
  "calls-audio": "Calls and audio",
  devices: "Linked devices",
  "transparency-log": "Transparency log",
  relays: "Relay servers",
  network: "Network routing",
  about: "About",
};

const SECTION_ICONS: Record<SettingsSection, ReactNode> = {
  account: <UserIcon size={19} />,
  privacy: <ShieldIcon size={19} />,
  stories: <StoriesIcon size={19} />,
  notifications: <BellIcon size={19} />,
  "calls-audio": <HeadphonesIcon size={19} />,
  devices: <MonitorIcon size={19} />,
  "transparency-log": <LogIcon size={19} />,
  relays: <GlobeIcon size={19} />,
  network: <ShieldIcon size={19} />,
  about: <InfoIcon size={19} />,
};

/** Desktop: the settings column beside the nav rail. */
export function SettingsNav() {
  const { state, dispatch } = useAppState();
  // Stories switched off on the server: no Stories entry (a missing provider, as in tests, counts as on).
  const storiesOn = useStoriesOptional()?.enabled !== false;
  const current: SettingsSection | null = state.screen.key === "settings" ? (state.screen.section ?? "account") : null;
  const item = (section: SettingsSection) => (
    <button
      key={section}
      type="button"
      className={`${styles.navItem} ${current === section ? styles.navItemOn : ""}`}
      aria-current={current === section ? "page" : undefined}
      onClick={() => dispatch({ type: "OPEN_SETTINGS", section })}
    >
      {SECTION_ICONS[section]}
      {SECTION_TITLES[section]}
    </button>
  );
  return (
    <nav className={styles.nav} aria-label="Settings">
      <h1 className={`${styles.navTitle} type-display`}>Settings</h1>
      {(storiesOn ? (["account", "privacy", "stories", "notifications", "calls-audio", "devices"] as const) : (["account", "privacy", "notifications", "calls-audio", "devices"] as const)).map(item)}
      <div className={styles.navDivider} />
      <span className={`${styles.navLabel} type-caption`}>NETWORK</span>
      {(["network", "transparency-log", "relays"] as const).map(item)}
      <div className={styles.navSpacer} />
      {item("about")}
    </nav>
  );
}

function SectionBody({ section }: { section: SettingsSection }) {
  switch (section) {
    case "account":
      return <AccountAndPrivacy part="account" />;
    case "privacy":
      return <AccountAndPrivacy part="privacy" />;
    case "stories":
      return <StorySettings />;
    case "notifications":
      return <NotificationsSettings />;
    case "calls-audio":
      return <CallsAudioSettings />;
    case "devices":
      return <LinkedDevices />;
    case "transparency-log":
      return <TransparencyLog />;
    case "relays":
      return <RelayServers />;
    case "network":
      return <NetworkRouting />;
    case "about":
      return <AboutSettings />;
  }
}

/** Pages a phone opens from the stacked Settings list. */
const SUB_PAGES: SettingsSection[] = ["stories", "notifications", "calls-audio", "devices", "network", "transparency-log", "relays", "about"];

export function Settings() {
  const { state, dispatch } = useAppState();
  const isDesktop = useIsDesktopLayout();
  const section = state.screen.key === "settings" ? state.screen.section : undefined;

  if (isDesktop) {
    const current = section ?? "account";
    const wide = current === "transparency-log";
    return (
      <div className={styles.screen}>
        <div className={`${styles.body} ${wide ? styles.bodyWide : styles.bodyDesktop}`}>
          {!wide && <h2 className={`${styles.pageTitle} type-display`}>{SECTION_TITLES[current]}</h2>}
          <SectionBody section={current} />
        </div>
      </div>
    );
  }

  if (section && SUB_PAGES.includes(section)) {
    return (
      <div className={styles.screen}>
        <TopBar title={SECTION_TITLES[section]} onBack={() => dispatch({ type: "OPEN_SETTINGS" })} />
        <div className={styles.body}>
          <SectionBody section={section} />
        </div>
      </div>
    );
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Settings" onBack={() => dispatch({ type: "GO_CHATS" })} />
      <div className={styles.body}>
        <AccountAndPrivacy part="both" />
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>MORE</span>
          {SUB_PAGES.map((page) => (
            <button
              key={page}
              type="button"
              className={styles.row}
              style={{ width: "100%", textAlign: "left", border: "none" }}
              onClick={() => dispatch({ type: "OPEN_SETTINGS", section: page })}
            >
              <span className={styles.navRowLabel}>
                {SECTION_ICONS[page]}
                <span className={`${styles.rowTitle} type-row-primary`}>{SECTION_TITLES[page]}</span>
              </span>
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}
