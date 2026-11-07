import { generateHandle } from "../lib/identity.js";
import { initialSettings } from "./mockData.js";
import type {
  AppSettings,
  Conversation,
  DMConversation,
  DurationKey,
  FriendRequestView,
  Message,
  MessageSecurity,
  ProfileOrigin,
  SelfIdentity,
  SubscriptionTier,
  GroupConversation,
} from "../types.js";

export type Screen =
  | { key: "onboarding" }
  | { key: "chats" }
  | { key: "thread"; conversationId: string }
  | { key: "new" }
  | { key: "new-contact" }
  | { key: "new-group" }
  | { key: "group-info"; conversationId: string }
  | { key: "verify"; contactId: string; origin: "new-contact" | "thread" }
  | { key: "message-security"; conversationId: string; messageId: string }
  /** `returnTo` is where the back button goes — "onboarding" when reached pre-login, "settings" when reached from a signed-in session. */
  | { key: "transparency-feed"; returnTo: "onboarding" | "settings" }
  | { key: "settings"; section?: SettingsSection }
  /** Call history (and, with `userId`, the history with one person) plus your call links. */
  | { key: "calls"; userId?: string }
  | { key: "contacts"; userId?: string }
  /** Someone opened a call link: `secret` is the base64url secret from the URL fragment. */
  | { key: "join-call"; linkId: string; secret: string }
  | { key: "group-invite"; linkId: string; secret: string }
  | { key: "friend-requests" }
  | { key: "profile"; userId: string; origin: ProfileOrigin }
  | { key: "change-password" }
  | { key: "change-username" }
  | { key: "mfa-setup" }
  | { key: "mfa-disable" }
  | { key: "delete-account" }
  /** `issuedKey`: a key just created (shown once, memory only); absent when opened from Settings. */
  | { key: "recovery-key"; issuedKey?: string }
  | { key: "admin" }
  | { key: "announcements" }
  /** The channel list, or one channel (`secret`: a private channel's link secret, from the link just opened). */
  | { key: "channels"; channelId?: string; secret?: string }
  | { key: "analytics"; page?: AnalyticsPage }
  | { key: "passwords" }
  /** Paid and free communities: discovery, a community's page, memberships, creating one, the creator dashboard. */
  | { key: "communities"; page?: CommunitiesPage; communityId?: string; inviteToken?: string; checkoutId?: string }
  /** Files: your encrypted storage, the public links you made, and files uploaded to you through them. */
  | { key: "files"; page?: FilesPage };

/** The pages in Files' sidebar. */
export type FilesPage = "files" | "links" | "incoming";

/** The pages of the Communities screen. */
export type CommunitiesPage = "discover" | "mine" | "memberships" | "create" | "creator" | "community" | "manage" | "library";

/** The pages in Analytics' sidebar. */
export type AnalyticsPage = "overview" | "storage" | "uploads" | "cleanup" | "backup" | "links" | "activity";

export type SettingsSection =
  | "account"
  | "privacy"
  | "stories"
  | "notifications"
  | "calls-audio"
  | "devices"
  | "transparency-log"
  | "relays"
  | "network"
  | "about";

export interface NewGroupDraft {
  name: string;
  memberIds: string[];
}

export type AuthStatus = "signed-out" | "authenticating" | "signed-in" | "mfa-pending";

export interface AppState {
  screen: Screen;
  locked: boolean;
  identity: SelfIdentity | null;
  authStatus: AuthStatus;
  authError: string | null;
  onboardingCandidateHandle: string;
  conversations: Conversation[];
  /** Pending friend requests someone else sent me — deliberately kept separate from `conversations`: no conversation/key exists until one is accepted. */
  friendRequestsIncoming: FriendRequestView[];
  /** Pending friend requests I sent, awaiting their response. */
  friendRequestsOutgoing: FriendRequestView[];
  settings: AppSettings;
  newGroupDraft: NewGroupDraft;
  contactError: string | null;
  /**
   * Messages waiting in the outbox (lib/outbox.ts) and whether the last try found no connection
   * (in Tor Browser: Tor or the onion service unreachable).
   */
  outbox: { queued: number; held: number; offline: boolean };
}

export function createInitialState(): AppState {
  return {
    screen: { key: "onboarding" },
    locked: false,
    identity: null,
    authStatus: "signed-out",
    authError: null,
    onboardingCandidateHandle: generateHandle(),
    conversations: [],
    friendRequestsIncoming: [],
    friendRequestsOutgoing: [],
    settings: initialSettings,
    newGroupDraft: { name: "", memberIds: [] },
    contactError: null,
    outbox: { queued: 0, held: 0, offline: false },
  };
}

export type Action =
  | { type: "REGENERATE_CANDIDATE" }
  | { type: "AUTH_STARTED" }
  | { type: "AUTH_SUCCEEDED"; identity: SelfIdentity; acceptMessageRequests: boolean; transparencyMode: boolean; showOnlineStatus?: boolean }
  | { type: "AUTH_FAILED"; message: string }
  /** Password verified; the account has MFA enabled — no key material yet, awaiting a code via submitMfaCode. */
  | { type: "AUTH_MFA_REQUIRED" }
  /** A wrong code — deliberately does NOT reset authStatus to "signed-out"; stays on the MFA challenge so the password isn't lost. */
  | { type: "AUTH_MFA_FAILED"; message: string }
  | { type: "AVATAR_UPDATED"; avatarUrl: string | null }
  | { type: "USERNAME_CHANGED"; handle: string }
  | { type: "SUBSCRIPTION_TIER_SET"; tier: SubscriptionTier }
  | { type: "LOGOUT" }
  | { type: "GO_CHATS" }
  /** Pre-auth only — e.g. backing out of the live transparency feed reached from Onboarding, where GO_CHATS would wrongly show the (identity-less) authenticated shell. */
  | { type: "GO_ONBOARDING" }
  | { type: "OPEN_FRIEND_REQUESTS" }
  | { type: "FRIEND_REQUESTS_LOADED"; incoming: FriendRequestView[]; outgoing: FriendRequestView[] }
  | { type: "FRIEND_REQUEST_SENT"; request: FriendRequestView }
  | { type: "FRIEND_REQUEST_ACCEPTED"; requestId: string }
  | { type: "FRIEND_REQUEST_DENIED"; requestId: string }
  | { type: "FRIEND_REQUEST_CANCELED"; requestId: string }
  | { type: "OPEN_PROFILE"; userId: string; origin: ProfileOrigin }
  | { type: "OPEN_CHANGE_PASSWORD" }
  | { type: "OPEN_CHANGE_USERNAME" }
  | { type: "OPEN_MFA_SETUP" }
  | { type: "OPEN_MFA_DISABLE" }
  | { type: "OPEN_DELETE_ACCOUNT" }
  | { type: "OPEN_RECOVERY_KEY"; issuedKey?: string }
  | { type: "MFA_ENABLED" }
  | { type: "MFA_DISABLED" }
  | { type: "OPEN_ADMIN" }
  | { type: "OPEN_ANNOUNCEMENTS" }
  | { type: "OPEN_COMMUNITIES"; page?: CommunitiesPage; communityId?: string; inviteToken?: string; checkoutId?: string }
  | { type: "OPEN_CHANNELS"; channelId?: string; secret?: string }
  | { type: "OPEN_ANALYTICS"; page?: AnalyticsPage }
  | { type: "OPEN_PASSWORDS" }
  | { type: "OPEN_FILES"; page?: FilesPage }
  | { type: "OPEN_THREAD"; conversationId: string }
  | { type: "OPEN_NEW" }
  | { type: "OPEN_NEW_CONTACT" }
  | { type: "OPEN_NEW_GROUP" }
  | { type: "CONTACT_LOOKUP_FAILED"; message: string | null }
  | { type: "CONTACT_READY"; contact: DMConversation }
  | { type: "CONVERSATIONS_LOADED"; conversations: Conversation[] }
  | { type: "MESSAGES_LOADED"; conversationId: string; messages: Message[] }
  /** `replacesQueueId`: the queued copy this message replaces (sent from the outbox). */
  | { type: "MESSAGE_SENT"; conversationId: string; message: Message; replacesQueueId?: string }
  /** The outbox as it is now: its messages shown in their chats, after the server's. */
  | { type: "OUTBOX_LOADED"; queued: { conversationId: string; message: Message }[]; held: number }
  | { type: "NETWORK_STATE"; offline: boolean }
  | { type: "MESSAGE_DELETED"; conversationId: string; messageId: string } // real DMs — confirmed deleted server-side
  | { type: "OPEN_VERIFY_FROM_THREAD"; contactId: string }
  | { type: "MARK_VERIFIED" }
  | { type: "DECLINE_VERIFY" }
  | { type: "OPEN_MESSAGE_SECURITY"; conversationId: string; messageId: string }
  | { type: "MESSAGE_SECURITY_UPDATED"; conversationId: string; messageId: string; security: MessageSecurity }
  | { type: "UPDATE_NEW_GROUP_NAME"; name: string }
  | { type: "TOGGLE_NEW_GROUP_MEMBER"; contactId: string }
  /** A group was created on the server and hydrated (see actions.createGroup). */
  | { type: "GROUP_CREATED"; group: GroupConversation }
  | { type: "OPEN_GROUP_INFO"; conversationId: string }
  | { type: "LEAVE_GROUP"; conversationId: string }
  | { type: "OPEN_SETTINGS"; section?: SettingsSection }
  | { type: "OPEN_CALLS"; userId?: string }
  | { type: "OPEN_CONTACTS"; userId?: string }
  | { type: "OPEN_JOIN_CALL"; linkId: string; secret: string }
  | { type: "OPEN_GROUP_INVITE"; linkId: string; secret: string }
  | { type: "OPEN_TRANSPARENCY_FEED"; returnTo: "onboarding" | "settings" }
  | { type: "UPDATE_SETTINGS"; settings: Partial<AppSettings> }
  | { type: "SET_DISAPPEARING"; conversationId: string; duration: DurationKey }
  | { type: "LOCK" }
  | { type: "UNLOCK" };

export function nowLabel(): string {
  return new Date().toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
}

function updateConversation(
  conversations: Conversation[],
  id: string,
  update: (c: Conversation) => Conversation
): Conversation[] {
  return conversations.map((c) => (c.id === id ? update(c) : c));
}

/**
 * fetchAndDecryptMessages (state/actions.ts) never computes `security` —
 * it's either set immediately at send time or fetched lazily on demand —
 * so any reducer case that replaces a DM's `messages` array with a freshly
 * re-hydrated one (MESSAGES_LOADED from Thread's own poll, and
 * CONVERSATIONS_LOADED from the sidebar's separate poll) must carry it
 * forward for messages that already had it, or it flickers back to
 * "unknown" every few seconds.
 */
function withPreservedSecurity(freshMessages: Message[], previousMessages: Message[]): Message[] {
  const existingById = new Map(previousMessages.filter((m) => m.id).map((m) => [m.id, m]));
  const fresh = freshMessages.map((m) => {
    if (m.id && !m.security) {
      const existing = existingById.get(m.id);
      if (existing?.security) return { ...m, security: existing.security };
    }
    return m;
  });
  // Queued messages aren't on the server: a refresh keeps them (OUTBOX_LOADED and MESSAGE_SENT replace them).
  return [...fresh, ...previousMessages.filter((m) => m.queueId)];
}

export function reducer(state: AppState, action: Action): AppState {
  switch (action.type) {
    case "REGENERATE_CANDIDATE":
      return { ...state, onboardingCandidateHandle: generateHandle() };

    case "AUTH_STARTED":
      return { ...state, authStatus: "authenticating", authError: null };

    case "AUTH_SUCCEEDED":
      return {
        ...state,
        authStatus: "signed-in",
        authError: null,
        identity: action.identity,
        settings: {
          ...state.settings,
          acceptMessageRequests: action.acceptMessageRequests,
          transparencyMode: action.transparencyMode,
          showOnlineStatus: action.showOnlineStatus ?? false,
        },
        screen: { key: "chats" },
      };

    case "AUTH_FAILED":
      return { ...state, authStatus: "signed-out", authError: action.message };

    case "AUTH_MFA_REQUIRED":
      return { ...state, authStatus: "mfa-pending", authError: null };

    case "AUTH_MFA_FAILED":
      // Deliberately stays on "mfa-pending", not "signed-out" — the password
      // already entered shouldn't be lost over a mistyped code.
      return { ...state, authStatus: "mfa-pending", authError: action.message };

    case "AVATAR_UPDATED":
      return state.identity ? { ...state, identity: { ...state.identity, avatarUrl: action.avatarUrl } } : state;

    case "USERNAME_CHANGED":
      return state.identity ? { ...state, identity: { ...state.identity, handle: action.handle } } : state;

    case "SUBSCRIPTION_TIER_SET":
      return state.identity ? { ...state, identity: { ...state.identity, subscriptionTier: action.tier } } : state;

    case "MFA_ENABLED":
      return state.identity ? { ...state, identity: { ...state.identity, mfaEnabled: true } } : state;

    case "MFA_DISABLED":
      return state.identity ? { ...state, identity: { ...state.identity, mfaEnabled: false } } : state;

    case "LOGOUT":
      return createInitialState();

    case "GO_CHATS":
      return { ...state, screen: { key: "chats" } };

    case "GO_ONBOARDING":
      return { ...state, screen: { key: "onboarding" } };

    case "OPEN_FRIEND_REQUESTS":
      return { ...state, screen: { key: "friend-requests" } };

    case "FRIEND_REQUESTS_LOADED":
      return { ...state, friendRequestsIncoming: action.incoming, friendRequestsOutgoing: action.outgoing };

    case "FRIEND_REQUEST_SENT":
      return { ...state, friendRequestsOutgoing: [...state.friendRequestsOutgoing, action.request] };

    case "FRIEND_REQUEST_ACCEPTED":
      return {
        ...state,
        friendRequestsIncoming: state.friendRequestsIncoming.filter((r) => r.id !== action.requestId),
      };

    case "FRIEND_REQUEST_DENIED":
      return {
        ...state,
        friendRequestsIncoming: state.friendRequestsIncoming.filter((r) => r.id !== action.requestId),
      };

    case "FRIEND_REQUEST_CANCELED":
      return {
        ...state,
        friendRequestsOutgoing: state.friendRequestsOutgoing.filter((r) => r.id !== action.requestId),
      };

    case "OPEN_PROFILE":
      return { ...state, screen: { key: "profile", userId: action.userId, origin: action.origin } };

    case "OPEN_CHANGE_PASSWORD":
      return { ...state, screen: { key: "change-password" } };

    case "OPEN_CHANGE_USERNAME":
      return { ...state, screen: { key: "change-username" } };

    case "OPEN_MFA_SETUP":
      return { ...state, screen: { key: "mfa-setup" } };

    case "OPEN_MFA_DISABLE":
      return { ...state, screen: { key: "mfa-disable" } };
    case "OPEN_DELETE_ACCOUNT":
      return { ...state, screen: { key: "delete-account" } };

    case "OPEN_RECOVERY_KEY":
      return { ...state, screen: { key: "recovery-key", issuedKey: action.issuedKey } };

    case "OPEN_ADMIN":
      return { ...state, screen: { key: "admin" } };
    case "OPEN_ANNOUNCEMENTS":
      return { ...state, screen: { key: "announcements" } };
    case "OPEN_COMMUNITIES":
      return {
        ...state,
        screen: { key: "communities", page: action.page, communityId: action.communityId, inviteToken: action.inviteToken, checkoutId: action.checkoutId },
      };
    case "OPEN_CHANNELS":
      return { ...state, screen: { key: "channels", channelId: action.channelId, ...(action.secret ? { secret: action.secret } : {}) } };
    case "OPEN_ANALYTICS":
      return { ...state, screen: { key: "analytics", page: action.page } };
    case "OPEN_PASSWORDS":
      return { ...state, screen: { key: "passwords" } };
    case "OPEN_FILES":
      return { ...state, screen: { key: "files", page: action.page } };

    case "OPEN_THREAD":
      return {
        ...state,
        screen: { key: "thread", conversationId: action.conversationId },
        conversations: updateConversation(state.conversations, action.conversationId, (c) => ({
          ...c,
          unread: 0,
        })),
      };

    case "OPEN_NEW":
      return { ...state, screen: { key: "new" } };

    case "OPEN_NEW_CONTACT":
      return { ...state, screen: { key: "new-contact" }, contactError: null };

    case "OPEN_NEW_GROUP":
      return { ...state, screen: { key: "new-group" }, newGroupDraft: { name: "", memberIds: [] } };

    case "CONTACT_LOOKUP_FAILED":
      return { ...state, contactError: action.message };

    case "CONTACT_READY": {
      const exists = state.conversations.some((c) => c.id === action.contact.id);
      const conversations = exists
        ? updateConversation(state.conversations, action.contact.id, () => action.contact)
        : [action.contact, ...state.conversations];
      return {
        ...state,
        conversations,
        contactError: null,
        screen: { key: "verify", contactId: action.contact.id, origin: "new-contact" },
      };
    }

    case "CONVERSATIONS_LOADED": {
      // Fires repeatedly (initial load + periodic refresh). `verified` is a
      // purely local judgment, so it must survive a reload instead of being
      // clobbered back to the freshly-hydrated default. `disappearing` and
      // `unread` are both real and server-backed now (disappearing —
      // per-participant, see db/schema.sql conversation_participants; unread
      // — computed from message_receipts in fetchAndDecryptMessages), so the
      // fresh incoming value is always authoritative, e.g. picking up a
      // change the *other* participant just made, or the true unread count
      // surviving a reload instead of being recomputed as a wrong guess.
      const existingById = new Map(state.conversations.map((c) => [c.id, c]));
      const conversations = action.conversations.map((incoming): Conversation => {
        const existing = existingById.get(incoming.id);
        if (!existing || existing.kind !== incoming.kind) return incoming;
        const messages = withPreservedSecurity(incoming.messages, existing.messages);
        if (incoming.kind !== "dm" || existing.kind !== "dm") return { ...incoming, messages };
        // Verification is about one key: a contact whose key just changed is unverified again.
        const sameKey = sameBytes(existing.publicKey, incoming.publicKey);
        return { ...incoming, verified: sameKey ? existing.verified : incoming.verified, messages };
      });
      return { ...state, conversations };
    }

    case "MESSAGES_LOADED":
      // Replaces wholesale rather than merging by id — the fetched list is
      // the server's current truth, so a message deleted (by either side)
      // since the last poll needs to actually disappear, not linger because
      // an id-based merge only ever adds. `security` is the one exception:
      // fetchAndDecryptMessages never computes it (it's either set
      // immediately at send time or fetched lazily on demand — see
      // state/actions.ts), so without this it would flicker back to
      // "unknown" on every 3s poll after the user just looked it up.
      return {
        ...state,
        conversations: updateConversation(state.conversations, action.conversationId, (c) => {
          const merged = withPreservedSecurity(action.messages, c.messages).sort(
            (a, b) => (a.createdAt ?? 0) - (b.createdAt ?? 0)
          );
          return { ...c, messages: merged, time: merged.at(-1)?.time ?? c.time };
        }),
      };

    case "MESSAGE_SENT":
      return {
        ...state,
        conversations: updateConversation(state.conversations, action.conversationId, (c) => ({
          ...c,
          time: action.message.time,
          messages: [...c.messages.filter((m) => !action.replacesQueueId || m.queueId !== action.replacesQueueId), action.message],
        })),
      };

    case "OUTBOX_LOADED":
      return {
        ...state,
        conversations: state.conversations.map((c) => {
          const queued = action.queued.filter((q) => q.conversationId === c.id).map((q) => q.message);
          const kept = c.messages.filter((m) => !m.queueId);
          if (queued.length === 0 && kept.length === c.messages.length) return c;
          return { ...c, messages: [...kept, ...queued] };
        }),
        outbox: { ...state.outbox, queued: action.queued.length, held: action.held },
      };

    case "NETWORK_STATE":
      return state.outbox.offline === action.offline ? state : { ...state, outbox: { ...state.outbox, offline: action.offline } };

    case "MESSAGE_DELETED":
      return {
        ...state,
        conversations: updateConversation(state.conversations, action.conversationId, (c) => ({
          ...c,
          messages: c.messages.filter((m) => m.id !== action.messageId),
        })),
      };

    case "OPEN_VERIFY_FROM_THREAD":
      return { ...state, screen: { key: "verify", contactId: action.contactId, origin: "thread" } };

    case "MARK_VERIFIED":
    case "DECLINE_VERIFY": {
      if (state.screen.key !== "verify") return state;
      const { contactId, origin } = state.screen;
      const conversations =
        action.type === "MARK_VERIFIED"
          ? updateConversation(state.conversations, contactId, (c) =>
              c.kind === "dm" ? { ...c, verified: true, keyChanged: false } : c
            )
          : state.conversations;
      return {
        ...state,
        conversations,
        screen: origin === "new-contact" ? { key: "chats" } : { key: "thread", conversationId: contactId },
      };
    }

    case "OPEN_MESSAGE_SECURITY":
      return { ...state, screen: { key: "message-security", conversationId: action.conversationId, messageId: action.messageId } };

    case "MESSAGE_SECURITY_UPDATED":
      return {
        ...state,
        conversations: updateConversation(state.conversations, action.conversationId, (c) => {
          if (c.kind !== "dm") return c;
          return {
            ...c,
            messages: c.messages.map((m) => (m.id === action.messageId ? { ...m, security: action.security } : m)),
          };
        }),
      };

    case "UPDATE_NEW_GROUP_NAME":
      return { ...state, newGroupDraft: { ...state.newGroupDraft, name: action.name } };

    case "TOGGLE_NEW_GROUP_MEMBER": {
      const { memberIds } = state.newGroupDraft;
      const next = memberIds.includes(action.contactId)
        ? memberIds.filter((id) => id !== action.contactId)
        : [...memberIds, action.contactId];
      return { ...state, newGroupDraft: { ...state.newGroupDraft, memberIds: next } };
    }

    case "GROUP_CREATED":
      return {
        ...state,
        conversations: [action.group, ...state.conversations.filter((c) => c.id !== action.group.id)],
        newGroupDraft: { name: "", memberIds: [] },
        screen: { key: "thread", conversationId: action.group.id },
      };

    case "OPEN_GROUP_INFO":
      return { ...state, screen: { key: "group-info", conversationId: action.conversationId } };

    case "LEAVE_GROUP":
      return {
        ...state,
        conversations: state.conversations.filter((c) => c.id !== action.conversationId),
        screen: { key: "chats" },
      };

    case "OPEN_SETTINGS":
      return { ...state, screen: { key: "settings", section: action.section } };

    case "OPEN_CALLS":
      return { ...state, screen: { key: "calls", userId: action.userId } };

    case "OPEN_CONTACTS":
      return { ...state, screen: { key: "contacts", userId: action.userId } };

    case "OPEN_JOIN_CALL":
      return { ...state, screen: { key: "join-call", linkId: action.linkId, secret: action.secret } };

    case "OPEN_GROUP_INVITE":
      return { ...state, screen: { key: "group-invite", linkId: action.linkId, secret: action.secret } };

    case "OPEN_TRANSPARENCY_FEED":
      return { ...state, screen: { key: "transparency-feed", returnTo: action.returnTo } };

    case "UPDATE_SETTINGS":
      return { ...state, settings: { ...state.settings, ...action.settings } };


    case "SET_DISAPPEARING":
      return {
        ...state,
        conversations: updateConversation(state.conversations, action.conversationId, (c) => ({
          ...c,
          disappearing: action.duration,
        })),
      };

    case "LOCK":
      return { ...state, locked: true };

    case "UNLOCK":
      return { ...state, locked: false };

    default:
      return state;
  }
}

function sameBytes(a: Uint8Array | undefined, b: Uint8Array | undefined): boolean {
  if (!a || !b) return a === b;
  return a.length === b.length && a.every((byte, i) => byte === b[i]);
}
