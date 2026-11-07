import { useEffect, useRef, useState } from "react";
import { TorGate } from "./components/TorGate.js";
import styles from "./App.module.css";
import { CallOverlay } from "./components/CallOverlay.js";
import { GroupCallOverlay } from "./components/GroupCallOverlay.js";
import { LockOverlay } from "./components/LockOverlay.js";
import { isTopLevel, NavRail, sectionOf } from "./components/NavRail.js";
import { Sidebar } from "./components/Sidebar.js";
import { useGroupCallDeepLink } from "./hooks/useGroupCallDeepLink.js";
import { useIsDesktopLayout } from "./hooks/useMediaQuery.js";
import { AdminPanel } from "./screens/AdminPanel.js";
import { Announcements } from "./screens/Announcements.js";
import { Communities } from "./screens/Communities.js";
import { useCommunityDeepLink } from "./hooks/useCommunityDeepLink.js";
import { Analytics } from "./screens/Analytics.js";
import { Passwords } from "./screens/Passwords.js";
import { Files } from "./screens/Files.js";
import { checkIncoming } from "./lib/shareWatch.js";
import { driveKey } from "./lib/driveMeta.js";
import { useLive } from "./state/LiveContext.js";
import { lockVault } from "./lib/vaultStore.js";
import { CallsDetail, CallsList } from "./screens/CallsScreen.js";
import { ChangePassword } from "./screens/ChangePassword.js";
import { ChangeUsername } from "./screens/ChangeUsername.js";
import { ContactDetail, ContactsList } from "./screens/ContactsScreen.js";
import { DeleteAccount } from "./screens/DeleteAccount.js";
import { FriendRequests } from "./screens/FriendRequests.js";
import { GroupInfo } from "./screens/GroupInfo.js";
import { JoinCall } from "./screens/JoinCall.js";
import { JoinGroup } from "./screens/JoinGroup.js";
import { Channels } from "./screens/Channels.js";
import { ChannelView } from "./screens/ChannelView.js";
import { channelSecretFromFragment } from "./lib/channels.js";
import { CHECK_EVERY_MS, checkFollowedChannels } from "./lib/channelWatch.js";
import type { Action } from "./state/reducer.js";
import { MfaDisable } from "./screens/MfaDisable.js";
import { MfaSetup } from "./screens/MfaSetup.js";
import { NewChooser } from "./screens/NewChooser.js";
import { NewContact } from "./screens/NewContact.js";
import { NewGroup } from "./screens/NewGroup.js";
import { Onboarding } from "./screens/Onboarding.js";
import { Profile } from "./screens/Profile.js";
import { RecoveryKey } from "./screens/RecoveryKey.js";
import { Settings, SettingsNav } from "./screens/Settings.js";
import { Thread } from "./screens/Thread.js";
import { TransparencyFeed } from "./screens/TransparencyFeed.js";
import { VerificationDetails } from "./screens/VerificationDetails.js";
import { Verify } from "./screens/Verify.js";
import { AppStateProvider, useAppState } from "./state/AppStateContext.js";
import { CallProvider, useCall } from "./state/CallContext.js";
import { GroupCallProvider, useGroupCall } from "./state/GroupCallContext.js";
import { LiveProvider } from "./state/LiveContext.js";
import { V3Receiver } from "./state/V3Chat.js";
import { StoriesProvider } from "./state/StoriesContext.js";
import { StoryOverlays } from "./components/stories/StoryOverlays.js";
import { EarningsBackground, usePurchaseReturnLink } from "./components/earnings/EarningsBackground.js";

/** The chats section's main area (and, on a phone, every screen below the chat list). */
function MainPane() {
  const { state } = useAppState();
  const { screen } = state;

  switch (screen.key) {
    case "thread":
      return <Thread conversationId={screen.conversationId} />;
    case "new":
      return <NewChooser />;
    case "new-contact":
      return <NewContact />;
    case "new-group":
      return <NewGroup />;
    case "group-info":
      return <GroupInfo conversationId={screen.conversationId} />;
    case "verify":
      // A dialog over the chat it's about (or over the empty chat area for a new contact).
      return (
        <>
          {screen.origin === "thread" ? <Thread conversationId={screen.contactId} /> : <EmptyMain />}
          <Verify contactId={screen.contactId} origin={screen.origin} />
        </>
      );
    case "message-security":
      return <VerificationDetails conversationId={screen.conversationId} messageId={screen.messageId} />;
    case "transparency-feed":
      return <TransparencyFeed returnTo={screen.returnTo} />;
    case "settings":
      return <Settings />;
    case "friend-requests":
      return <FriendRequests />;
    case "profile":
      return <Profile userId={screen.userId} origin={screen.origin} />;
    case "change-password":
      return <ChangePassword />;
    case "change-username":
      return <ChangeUsername />;
    case "mfa-setup":
      return <MfaSetup />;
    case "mfa-disable":
      return <MfaDisable />;
    case "delete-account":
      return <DeleteAccount />;
    case "recovery-key":
      return <RecoveryKey issuedKey={screen.issuedKey} />;
    case "admin":
      return <AdminPanel />;
    case "announcements":
      return <Announcements />;
    case "communities":
      return <Communities />;
    case "channels":
      return screen.channelId ? <ChannelView channelId={screen.channelId} secret={screen.secret} /> : <Channels />;
    case "analytics":
      return <Analytics />;
    case "passwords":
      return <Passwords />;
    case "files":
      return <Files page={screen.page ?? "files"} />;
    case "calls":
      return <CallsDetail />;
    case "join-call":
      return <JoinCall linkId={screen.linkId} secret={screen.secret} />;
    case "group-invite":
      return <JoinGroup linkId={screen.linkId} secret={screen.secret} />;
    case "contacts":
      return <ContactDetail />;
    case "chats":
      return <EmptyMain />;
    default:
      return null;
  }
}

function EmptyMain() {
  return (
    <div className={styles.emptyMain}>
      <p className="type-secondary">Select a conversation</p>
    </div>
  );
}

/** Desktop: nav rail, then the section's list column and main area. */
function DesktopShell() {
  const { state } = useAppState();
  const section = sectionOf(state.screen);
  const settingsLike = section === "settings" || section === "privacy";

  return (
    <>
      <NavRail variant="rail" />
      {section === "chats" && (
        <div className={styles.listPane}>
          <Sidebar />
        </div>
      )}
      {section === "calls" && (
        <div className={styles.listPane}>
          <CallsList />
        </div>
      )}
      {section === "contacts" && (
        <div className={styles.listPane}>
          <ContactsList />
        </div>
      )}
      {settingsLike && (
        <div className={styles.settingsNavPane}>
          <SettingsNav />
        </div>
      )}
      <main className={styles.mainPane}>
        <MainPane />
      </main>
    </>
  );
}

/** Phone: one screen at a time; the top-level lists get the tab bar. */
function MobileShell() {
  const { state } = useAppState();
  const { screen } = state;
  const topLevel = isTopLevel(screen);
  let content;
  if (screen.key === "chats") content = <Sidebar showLock />;
  else if (screen.key === "calls" && !screen.userId) content = <CallsList />;
  else if (screen.key === "contacts" && !screen.userId) content = <ContactsList />;
  else content = <MainPane />;
  return (
    <div className={styles.mobileColumn}>
      <main className={styles.mobileMain}>{content}</main>
      {topLevel && <NavRail variant="tabs" />}
    </div>
  );
}

const PENDING_JOIN = "redvaultsystem:pending-join";
const PENDING_INVITE = "redvaultsystem:pending-group-invite";

/**
 * Someone opened a link with a secret in its fragment: a call link
 * (…?join=<id>#<secret>) or a group invite link (…?invite=<id>#<secret>).
 * The secret is taken out of the address bar and history right away, kept
 * only for this tab, and the link's screen opens once signed in.
 */
function usePendingSecretLink(param: "join" | "invite", storageKey: string, open: (linkId: string, secret: string) => Action) {
  const { state, dispatch } = useAppState();
  useEffect(() => {
    const params = new URLSearchParams(window.location.search);
    const linkId = params.get(param);
    const secret = window.location.hash.replace(/^#/, "");
    if (!linkId || !secret) return;
    try {
      sessionStorage.setItem(storageKey, JSON.stringify({ linkId, secret }));
    } catch {
      // Without session storage the link is only honored if already signed in (below).
    }
    params.delete(param);
    const rest = params.toString();
    window.history.replaceState(null, "", `${window.location.pathname}${rest ? `?${rest}` : ""}`);
    if (state.authStatus === "signed-in") dispatch(open(linkId, secret));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (state.authStatus !== "signed-in") return;
    let pending: { linkId: string; secret: string } | null = null;
    try {
      const raw = sessionStorage.getItem(storageKey);
      sessionStorage.removeItem(storageKey);
      pending = raw ? (JSON.parse(raw) as { linkId: string; secret: string }) : null;
    } catch {
      pending = null;
    }
    if (pending) dispatch(open(pending.linkId, pending.secret));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.authStatus, dispatch]);
}

function usePendingCallLink() {
  usePendingSecretLink("join", PENDING_JOIN, (linkId, secret) => ({ type: "OPEN_JOIN_CALL", linkId, secret }));
}

function usePendingGroupInvite() {
  usePendingSecretLink("invite", PENDING_INVITE, (linkId, secret) => ({ type: "OPEN_GROUP_INVITE", linkId, secret }));
}

/**
 * Followed channels: checks them for new posts every couple of minutes and
 * when the tab comes back, for the Channels badge and notifications
 * (lib/channelWatch.ts: all on this device; the server keeps no follower list).
 */
function useChannelWatch() {
  const { state, dispatch } = useAppState();
  const userId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  const current = useRef<string | null>(null);
  current.current = state.screen.key === "channels" ? (state.screen.channelId ?? null) : null;
  useEffect(() => {
    if (!userId) return;
    let running = false;
    const run = () => {
      if (running) return;
      running = true;
      void checkFollowedChannels(userId, {
        currentChannelId: current.current,
        openChannel: (channelId) => dispatch({ type: "OPEN_CHANNELS", channelId }),
      }).finally(() => {
        running = false;
      });
    };
    run();
    const timer = setInterval(run, CHECK_EVERY_MS);
    const onVisible = () => {
      if (!document.hidden) run();
    };
    document.addEventListener("visibilitychange", onVisible);
    window.addEventListener("redvaultsystem:prefs-changed", run);
    return () => {
      clearInterval(timer);
      document.removeEventListener("visibilitychange", onVisible);
      window.removeEventListener("redvaultsystem:prefs-changed", run);
    };
  }, [userId, dispatch]);
}

/**
 * Files uploaded to you through your share links: checks Incoming when you
 * sign in, every couple of minutes, when the tab comes back, and right away
 * when the server says something arrived (lib/shareWatch.ts).
 */
function useShareWatch() {
  const { state, dispatch } = useAppState();
  const live = useLive();
  const userId = state.authStatus === "signed-in" ? state.identity?.userId : undefined;
  const privateKey = state.identity?.privateKey;
  const onFiles = useRef(false);
  onFiles.current = state.screen.key === "files" && state.screen.page === "incoming";
  useEffect(() => {
    if (!userId || !privateKey) return;
    const key = driveKey(privateKey);
    let running = false;
    const run = () => {
      if (running) return;
      running = true;
      void checkIncoming(userId, key, { onOpen: () => dispatch({ type: "OPEN_FILES", page: "incoming" }), quiet: onFiles.current && !document.hidden })
        .catch(() => undefined)
        .finally(() => {
          running = false;
        });
    };
    run();
    const timer = setInterval(run, CHECK_EVERY_MS);
    const onVisible = () => {
      if (!document.hidden) run();
    };
    document.addEventListener("visibilitychange", onVisible);
    const unsubscribe = live.subscribe((event) => {
      if (event.type === "share_upload") run();
    });
    return () => {
      clearInterval(timer);
      document.removeEventListener("visibilitychange", onVisible);
      unsubscribe();
    };
  }, [userId, privateKey, dispatch, live]);
}

const PENDING_CHANNEL = "redvaultsystem:pending-channel";

/**
 * Someone opened a channel's link (…?channel=<id>, with #<secret> for a
 * private channel): it opens once signed in. Like other links with a secret,
 * the fragment leaves the address bar and history right away.
 */
function usePendingChannelLink() {
  const { state, dispatch } = useAppState();
  useEffect(() => {
    const params = new URLSearchParams(window.location.search);
    const channelId = params.get("channel");
    if (!channelId || !/^[0-9a-f-]{36}$/i.test(channelId)) return;
    const secret = channelSecretFromFragment(window.location.hash) ?? undefined;
    try {
      sessionStorage.setItem(PENDING_CHANNEL, JSON.stringify({ channelId, secret }));
    } catch {
      // Without session storage the link only opens if already signed in (below).
    }
    params.delete("channel");
    const rest = params.toString();
    window.history.replaceState(null, "", `${window.location.pathname}${rest ? `?${rest}` : ""}`);
    if (state.authStatus === "signed-in") dispatch({ type: "OPEN_CHANNELS", channelId, secret });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (state.authStatus !== "signed-in") return;
    let pending: { channelId: string; secret?: string } | null = null;
    try {
      const raw = sessionStorage.getItem(PENDING_CHANNEL);
      sessionStorage.removeItem(PENDING_CHANNEL);
      pending = raw ? (JSON.parse(raw) as { channelId: string; secret?: string }) : null;
    } catch {
      pending = null;
    }
    if (pending) dispatch({ type: "OPEN_CHANNELS", channelId: pending.channelId, secret: pending.secret });
  }, [state.authStatus, dispatch]);
}

export const IDLE_LOCK_MS = 2 * 60 * 1000; // 2 minutes — "policy is each platform's OS convention to define"
const IDLE_RESET_EVENTS = ["pointerdown", "keydown", "wheel", "touchstart"] as const;

/**
 * Lock is a route guard, not a screen: any state can be interrupted by
 * locked=true and resumes exactly where it left off on unlock.
 *
 * "App lock" (require unlock after backgrounding or timeout) and "Blur in
 * app switcher" (hide previews in recents) are independent settings, not
 * an AND: App lock alone must still force a real unlock on backgrounding or
 * idle timeout, regardless of the blur setting.
 */
function useAutoLock() {
  const { state, dispatch } = useAppState();
  const { appLock } = state.settings;
  // Nobody touches the keyboard during a call; that isn't "idle".
  const { call } = useCall();
  const { call: groupCall } = useGroupCall();
  const inCall = (call !== null && call.phase !== "ended") || (groupCall !== null && groupCall.phase !== "ended");

  // Backgrounding: switching tabs/windows is the closest web analog to the
  // OS app-switcher trigger.
  useEffect(() => {
    if (!appLock) return;
    function onVisibilityChange() {
      if (document.hidden) dispatch({ type: "LOCK" });
    }
    document.addEventListener("visibilitychange", onVisibilityChange);
    return () => document.removeEventListener("visibilitychange", onVisibilityChange);
  }, [appLock, dispatch]);

  // Timeout: no pointer/keyboard activity for IDLE_LOCK_MS.
  useEffect(() => {
    if (!appLock || inCall) return;
    let timer: ReturnType<typeof setTimeout>;
    function resetTimer() {
      clearTimeout(timer);
      timer = setTimeout(() => dispatch({ type: "LOCK" }), IDLE_LOCK_MS);
    }
    for (const event of IDLE_RESET_EVENTS) window.addEventListener(event, resetTimer);
    resetTimer();
    return () => {
      clearTimeout(timer);
      for (const event of IDLE_RESET_EVENTS) window.removeEventListener(event, resetTimer);
    };
  }, [appLock, inCall, dispatch]);
}

/**
 * "Blur in app switcher" is a distinct, weaker setting from App lock: a web
 * page can't reach into the OS's actual app-switcher thumbnail, so this
 * approximates it by blurring on tab backgrounding — with no unlock
 * required, auto-clearing the instant the tab is visible again. When App
 * lock is also on, the real lock already replaces the screen entirely, so
 * this only matters when App lock is off but the user still wants the
 * on-screen content hidden while backgrounded.
 */
function useSwitcherBlur(): boolean {
  const { state } = useAppState();
  const { switcherBlur, appLock } = state.settings;
  const [blurred, setBlurred] = useState(false);

  useEffect(() => {
    if (!switcherBlur || appLock) {
      setBlurred(false);
      return;
    }
    function onVisibilityChange() {
      setBlurred(document.hidden);
    }
    document.addEventListener("visibilitychange", onVisibilityChange);
    return () => document.removeEventListener("visibilitychange", onVisibilityChange);
  }, [switcherBlur, appLock]);

  return blurred;
}

function AppShell() {
  const { state, dispatch } = useAppState();
  const isDesktop = useIsDesktopLayout();
  useAutoLock();
  usePendingCallLink();
  usePendingGroupInvite();
  usePendingChannelLink();
  useChannelWatch();
  usePurchaseReturnLink();
  useShareWatch();
  const groupCallLink = useGroupCallDeepLink();
  useCommunityDeepLink();
  const switcherBlurred = useSwitcherBlur();

  // The password vault never stays open behind the app lock or after signing out.
  useEffect(() => {
    if (state.locked || !state.identity) lockVault();
  }, [state.locked, state.identity]);

  // Reachable with no account at all (see Onboarding's link to it) — render
  // it standalone, before anything below that assumes a signed-in identity
  // (Sidebar, the auto-lock shell). Opened from a signed-in Settings screen
  // instead renders through the normal MainPane switch further down, so it
  // still gets Sidebar/desktop layout like every other authenticated screen.
  if (state.screen.key === "transparency-feed" && state.screen.returnTo === "onboarding") {
    return (
      <div className={styles.surface}>
        <TransparencyFeed returnTo="onboarding" />
      </div>
    );
  }

  if (state.screen.key === "onboarding") {
    return (
      <div className={styles.surface}>
        <Onboarding />
      </div>
    );
  }

  // Locked renders ONLY the lock state: the conversation list, thread
  // content, and everything else must be absent from the DOM/accessibility
  // tree while locked, not merely covered by an overlay — otherwise a
  // screen reader or DOM inspection could still expose it.
  if (state.locked) {
    return (
      <div className={styles.surface}>
        <LockOverlay onUnlock={() => dispatch({ type: "UNLOCK" })} />
        <CallOverlay locked />
        <GroupCallOverlay locked />
      </div>
    );
  }

  return (
    <div className={styles.surface}>
      <div className={`${styles.shell} ${switcherBlurred ? styles.blurred : ""}`}>{isDesktop ? <DesktopShell /> : <MobileShell />}</div>
      <StoryOverlays />
      <EarningsBackground />
      <CallOverlay locked={false} />
      <GroupCallOverlay locked={false} />
      {groupCallLink.notice && (
        <div className={styles.notice} role="alert">
          <span>{groupCallLink.notice}</span>
          <button type="button" onClick={groupCallLink.dismiss}>
            Close
          </button>
        </div>
      )}
    </div>
  );
}

export default function App() {
  return (
    // Tor routing: on the clearnet address, says where the app is moving (or, once enforced, runs only there).
    <TorGate>
      <AppStateProvider>
        <LiveProvider>
          <V3Receiver />
          <CallProvider>
            <GroupCallProvider>
              <StoriesProvider>
                <AppShell />
              </StoriesProvider>
            </GroupCallProvider>
          </CallProvider>
        </LiveProvider>
      </AppStateProvider>
    </TorGate>
  );
}
