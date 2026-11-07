// Shared in-memory stand-in for ../lib/api.js, used by test files that need
// the app to get past onboarding (register/login) without a live server.
// Real crypto (e2eCrypto.js) still runs for real — only the network layer
// is faked. Each test file wires this in with:
//
//   vi.mock("../lib/api.js", async () => {
//     const backend = await import("./fakeApiBackend.js");
//     return backend.fakeApiModule;
//   });
//
// (the dynamic import avoids vitest's vi.mock hoisting TDZ, since a
// directly-closed-over top-level import isn't safe inside a mock factory).
import { commitmentToHex, computeCommitment } from "../lib/commitment.js";
import * as crypto from "../lib/e2eCrypto.js";

interface FakeUser {
  userId: string;
  username: string;
  publicKey: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfSalt: string;
  acceptMessageRequests: boolean;
  transparencyMode: boolean;
  avatarUrl: string | null;
  role: "user" | "super_admin";
  subscriptionTier: "free" | "pro" | "premium";
  mfaEnabled: boolean;
  createdAt: string;
  deleted: boolean;
}
// "pending"/"accepted"/"ignored" conversation_participants statuses no longer
// gate 1:1 messaging — friendship (below) does. Every fake conversation this
// module creates is already mutually "accepted" by construction.
type FakeParticipantStatus = "accepted";
interface FakeParticipant {
  userId: string;
  wrappedKey: string;
  status: FakeParticipantStatus;
  /** This participant's own disappearing-messages preference — independent per participant, mirroring conversation_participants.disappearing_seconds. */
  disappearingSeconds: number | null;
}
interface FakeConversation {
  id: string;
  isGroup: boolean;
  createdBy: string;
  createdAt: string;
  participants: FakeParticipant[];
}
interface FakeMessage {
  id: string;
  conversationId: string;
  senderId: string;
  ciphertext: string;
  nonce: string;
  createdAt: string;
  /** Captured from the sender's own disappearing preference at send time — mirrors messages.disappear_after_seconds. */
  disappearAfterSeconds: number | null;
}
interface FakeReceipt {
  messageId: string;
  userId: string;
  status: "delivered" | "read";
  statusAt: string;
}
/** Mirrors transparency_events — see server/src/services/transparencyService.ts. */
interface FakeVerification {
  publicTransactionId: string;
  commitment: string;
  protocolVersion: number;
  relayStatus: "relayed";
  createdAt: string;
}
type FakeFriendRequestStatus = "pending" | "accepted" | "denied" | "canceled";
interface FakeFriendRequest {
  id: string;
  requesterId: string;
  recipientId: string;
  status: FakeFriendRequestStatus;
  createdAt: string;
  respondedAt: string | null;
}
interface FakeReport {
  id: string;
  reporterId: string;
  reportedId: string;
  reason: string;
  status: "open" | "reviewed" | "dismissed";
  createdAt: string;
  reviewedAt: string | null;
  reviewedBy: string | null;
}

export const usersByName = new Map<string, FakeUser>();
export const usersById = new Map<string, FakeUser>();
export const conversations: FakeConversation[] = [];
export const messages: FakeMessage[] = [];
export const receipts: FakeReceipt[] = [];
/** By message id — set once, right after a fake sendMessage, exactly like the real server's send-time recordForMessage. */
export const verifications = new Map<string, FakeVerification>();
export const friendRequests: FakeFriendRequest[] = [];
export const reports: FakeReport[] = [];
let seq = 0;
/** Whoever last called register/login through the fake — mirrors the real server's session cookie, since listConversations() etc. take no userId argument. */
let currentUserId: string | null = null;
/** Tier given to accounts created through register(). Sending files is Pro-only, so file tests set this to "pro". */
let newUserTier: "free" | "pro" | "premium" = "free";

export function setNewUserTier(tier: "free" | "pro" | "premium"): void {
  newUserTier = tier;
}

/** Calls: off unless a test turns them on (the real server answers 503 without a TURN relay). */
let callsEnabled = false;
export const startedCalls: { conversationId: string; kind: string }[] = [];
export function setCallsEnabled(enabled: boolean): void {
  callsEnabled = enabled;
}
/** A Pro account's fake card subscription: renews until cancelled, then ends on FAKE_PLAN_ENDS_ON. */
let planCancelled = false;
export const FAKE_PLAN_ENDS_ON = "2026-10-26";
function fakePlanStatus() {
  if (newUserTier === "free") return { tier: "free", source: null, renews: false, endsOn: null, canCancel: false, canResume: false, canSwitchTo: null };
  return {
    tier: newUserTier,
    source: "stripe",
    renews: !planCancelled,
    endsOn: planCancelled ? FAKE_PLAN_ENDS_ON : null,
    canCancel: !planCancelled,
    canResume: planCancelled,
    canSwitchTo: newUserTier === "premium" ? "pro" : "premium",
  };
}
/** Set when login() returns {mfaRequired: true} — mirrors the real server's mfa_pending session flag; verifyMfaCode() consumes it. */
let pendingMfaUserId: string | null = null;
/** The fake never actually runs TOTP — any confirm/verify call must use this fixed code, mirroring how the fake never checks the real password either. */
export const FAKE_MFA_CODE = "123456";

export class FakeApiError extends Error {
  constructor(
    public status: number,
    public code: string,
    message: string
  ) {
    super(message);
  }
}

export function resetFakeBackend(): void {
  planCancelled = false;
  callsEnabled = false;
  startedCalls.length = 0;
  usersByName.clear();
  usersById.clear();
  conversations.length = 0;
  messages.length = 0;
  receipts.length = 0;
  verifications.clear();
  friendRequests.length = 0;
  reports.length = 0;
  seq = 0;
  currentUserId = null;
  pendingMfaUserId = null;
  newUserTier = "free";
}

/** Registers a fixture "other side" user directly into the fake backend, bypassing the UI and the current session. */
export async function seedOtherUser(username: string): Promise<FakeUser> {
  await crypto.ready();
  const keyPair = crypto.generateKeyPair();
  const wrapped = crypto.wrapPrivateKey(keyPair.privateKey, "unused-fixture-password");
  const user: FakeUser = {
    userId: `user-${++seq}`,
    username,
    publicKey: crypto.toBase64(keyPair.publicKey),
    encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
    privateKeyNonce: crypto.toBase64(wrapped.nonce),
    privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
    acceptMessageRequests: true,
    transparencyMode: false,
    avatarUrl: null,
    role: "user",
    subscriptionTier: "free",
    mfaEnabled: false,
    createdAt: new Date().toISOString(),
    deleted: false,
  };
  usersByName.set(username.toLowerCase(), user);
  usersById.set(user.userId, user);
  return user;
}

/** Test-only: runs a fake-api call as if a different (already-seeded) user were the active session — simulates "the other side" without disturbing the App-under-test's own session. */
async function asUser<T>(userId: string, fn: () => Promise<T>): Promise<T> {
  const saved = currentUserId;
  currentUserId = userId;
  try {
    return await fn();
  } finally {
    currentUserId = saved;
  }
}

export function createConversationAsOther(
  creatorUserId: string,
  participants: { userId: string; wrappedKey: string }[]
): Promise<{ id: string; isGroup: boolean; createdAt: string }> {
  return asUser(creatorUserId, () => fakeApiModule.createConversation({ isGroup: false, participants }));
}

export function sendMessageAsOther(
  senderId: string,
  conversationId: string,
  ciphertext: string,
  nonce: string
): Promise<FakeMessage> {
  return asUser(senderId, () => fakeApiModule.sendMessage(conversationId, ciphertext, nonce));
}

/** Simulates the recipient's own client fetching + acknowledging a conversation — the real delivery/read ACK, from their session, not the sender's. */
export function markConversationReadAsOther(
  recipientUserId: string,
  conversationId: string,
  status: "delivered" | "read"
): Promise<void> {
  return asUser(recipientUserId, () => fakeApiModule.markConversationRead(conversationId, status));
}

/** Simulates `strangerUserId` sending `recipientUsername` a friend request — the modern equivalent of the old "a stranger messages you" fixture, since a non-friend can no longer create a conversation directly. */
export function sendFriendRequestAsOther(
  strangerUserId: string,
  recipientUsername: string
): Promise<FakeFriendRequest> {
  return asUser(strangerUserId, () => fakeApiModule.sendFriendRequest(recipientUsername));
}

function requireCurrentUser(): FakeUser {
  const user = currentUserId ? usersById.get(currentUserId) : undefined;
  if (!user) throw new FakeApiError(401, "unauthorized", "Authentication required");
  return user;
}

function conversationDto(conversation: FakeConversation, forUserId: string) {
  const mine = conversation.participants.find((p) => p.userId === forUserId)!;
  return {
    id: conversation.id,
    isGroup: conversation.isGroup,
    role: conversation.createdBy === forUserId ? ("owner" as const) : ("member" as const),
    nameCiphertext: null,
    nameNonce: null,
    wrappedKey: mine.wrappedKey,
    disappearingSeconds: mine.disappearingSeconds,
    createdBy: conversation.createdBy,
    createdAt: conversation.createdAt,
  };
}

/** Mirrors the real server's lazy sweep-on-list: soft-"deletes" (drops) anything past its own captured TTL — per-message, not per-conversation, so two participants' independent settings never fight over one message's fate. */
function expireDisappearingMessages(conversationId: string): void {
  const now = Date.now();
  for (let i = messages.length - 1; i >= 0; i--) {
    const m = messages[i];
    if (m.conversationId !== conversationId || !m.disappearAfterSeconds) continue;
    const cutoff = new Date(m.createdAt).getTime() + m.disappearAfterSeconds * 1000;
    if (now >= cutoff) messages.splice(i, 1);
  }
}

export const FAKE_PAYMENT_LINK = "https://buy.stripe.com/test_fake";
export const FAKE_PREMIUM_PAYMENT_LINK = "https://buy.stripe.com/test_fake_premium";
const GiB = 1024 * 1024 * 1024;
export const FAKE_FILES_STORAGE = { free: 5 * GiB, pro: 50 * GiB, premium: 100 * GiB };

export const fakeApiModule = {
  ApiError: FakeApiError,
  callEvents: async (after: number | null) => {
    if (!callsEnabled) throw new FakeApiError(503, "calls_disabled", "Calling isn't available on this server.");
    // A long poll with nothing to deliver.
    if (after !== null) await new Promise((r) => setTimeout(r, 1000));
    return { cursor: 0, events: [] };
  },
  startCall: async (conversationId: string, kind: string) => {
    startedCalls.push({ conversationId, kind });
    throw new FakeApiError(503, "calls_disabled", "Calling isn't available on this server.");
  },
  hangUpCall: async () => {},
  sendTyping: async () => {},
  getPresence: async () => ({ online: [] as string[] }),
  getRelays: async () => {
    throw new FakeApiError(503, "calls_disabled", "Calling isn't available on this server.");
  },
  listCallLinks: async () => [],
  getCallLinkInfo: async () => {
    throw new FakeApiError(404, "not_found", "This call link doesn't exist or was deleted.");
  },
  listSessions: async () => [{ id: "session-1", current: true, expiresOn: "2026-10-28", label: null }],
  setSessionLabel: async () => {},
  signOutSession: async () => {},
  getLogCheckpoint: async () => {
    throw new FakeApiError(503, "log_disabled", "The transparency log isn't set up on this server.");
  },
  declineCall: async () => {},
  hangUpCallOnUnload: () => {},
  getBillingConfig: async () => ({ paymentLink: FAKE_PAYMENT_LINK, premiumPaymentLink: FAKE_PREMIUM_PAYMENT_LINK, filesStorage: FAKE_FILES_STORAGE }),
  getPlanStatus: async () => fakePlanStatus(),
  cancelPlan: async () => {
    planCancelled = true;
    return fakePlanStatus();
  },
  resumePlan: async () => {
    planCancelled = false;
    return fakePlanStatus();
  },
  // Like the server: switching keeps a subscription that was set to end.
  switchPlan: async (tier: "pro" | "premium") => {
    planCancelled = false;
    newUserTier = tier;
    const user = currentUserId ? usersById.get(currentUserId) : undefined;
    if (user) user.subscriptionTier = tier;
    return fakePlanStatus();
  },
  paymentLinkFor: (link: string, userId: string) => {
    const url = new URL(link);
    url.searchParams.set("client_reference_id", userId);
    return url.toString();
  },
  register: async (input: Record<string, string | boolean>) => {
    // Like the server: no account unless all three sign-up boxes were ticked.
    if (input.terms_accepted !== true || input.privacy_acknowledged !== true || input.age_18_confirmed !== true) {
      throw new FakeApiError(400, "consent_required", "To create an account, agree to the Terms of Service, acknowledge the Privacy Policy and confirm you are 18 or older.");
    }
    const fields = input as Record<string, string>;
    if (usersByName.has(fields.username.toLowerCase())) {
      throw new FakeApiError(409, "username_taken", "Username is already taken");
    }
    const user: FakeUser = {
      userId: `user-${++seq}`,
      username: fields.username,
      publicKey: fields.publicKey,
      encryptedPrivateKey: fields.encryptedPrivateKey,
      privateKeyNonce: fields.privateKeyNonce,
      privateKeyKdfSalt: fields.privateKeyKdfSalt,
      acceptMessageRequests: true,
      transparencyMode: false,
      avatarUrl: null,
      role: "user",
      subscriptionTier: newUserTier,
      mfaEnabled: false,
      createdAt: new Date().toISOString(),
      deleted: false,
    };
    usersByName.set(fields.username.toLowerCase(), user);
    usersById.set(user.userId, user);
    currentUserId = user.userId;
    return {
      userId: user.userId,
      acceptMessageRequests: user.acceptMessageRequests,
      transparencyMode: user.transparencyMode,
      avatarUrl: user.avatarUrl,
      expiresAt: new Date().toISOString(),
      role: user.role,
      subscriptionTier: user.subscriptionTier,
    };
  },
  publishKeyNotice: async () => {},
  preLogin: async () => ({ kdfSalt: "AAAAAAAAAAAAAAAAAAAAAA==", authVersion: 1 as const, upgrade: false }),
  login: async (username: string) => {
    const user = usersByName.get(username.toLowerCase());
    if (!user) throw new FakeApiError(401, "invalid_credentials", "Invalid username or password");
    if (user.mfaEnabled) {
      // Password verification is only simulated (the fake never checks the
      // password itself — see the note on loginIdentity's real counterpart);
      // reaching here means "the password would have been accepted." No key
      // material yet — same as the real MFA-gated login response.
      currentUserId = null;
      pendingMfaUserId = user.userId;
      return { mfaRequired: true as const };
    }
    currentUserId = user.userId;
    return {
      userId: user.userId,
      publicKey: user.publicKey,
      encryptedPrivateKey: user.encryptedPrivateKey,
      privateKeyNonce: user.privateKeyNonce,
      privateKeyKdfSalt: user.privateKeyKdfSalt,
      acceptMessageRequests: user.acceptMessageRequests,
      transparencyMode: user.transparencyMode,
      avatarUrl: user.avatarUrl,
      expiresAt: new Date().toISOString(),
      role: user.role,
      subscriptionTier: user.subscriptionTier,
    };
  },
  verifyMfaCode: async (code: string) => {
    const user = pendingMfaUserId ? usersById.get(pendingMfaUserId) : undefined;
    if (!user) throw new FakeApiError(401, "unauthorized", "No pending MFA challenge");
    if (code !== FAKE_MFA_CODE) throw new FakeApiError(401, "invalid_code", "Wrong code");
    pendingMfaUserId = null;
    currentUserId = user.userId;
    return {
      userId: user.userId,
      publicKey: user.publicKey,
      encryptedPrivateKey: user.encryptedPrivateKey,
      privateKeyNonce: user.privateKeyNonce,
      privateKeyKdfSalt: user.privateKeyKdfSalt,
      acceptMessageRequests: user.acceptMessageRequests,
      transparencyMode: user.transparencyMode,
      avatarUrl: user.avatarUrl,
      expiresAt: new Date().toISOString(),
      role: user.role,
      subscriptionTier: user.subscriptionTier,
    };
  },
  logout: async () => {
    currentUserId = null;
  },
  me: async () => {
    const user = requireCurrentUser();
    return {
      userId: user.userId,
      username: user.username,
      publicKey: user.publicKey,
      acceptMessageRequests: user.acceptMessageRequests,
      transparencyMode: user.transparencyMode,
      avatarUrl: user.avatarUrl,
      role: user.role,
      subscriptionTier: user.subscriptionTier,
      mfaEnabled: user.mfaEnabled,
    };
  },
  updateSettings: async (input: { acceptMessageRequests?: boolean; transparencyMode?: boolean }) => {
    const user = requireCurrentUser();
    if (input.acceptMessageRequests !== undefined) user.acceptMessageRequests = input.acceptMessageRequests;
    if (input.transparencyMode !== undefined) user.transparencyMode = input.transparencyMode;
  },
  changePassword: async (input: { currentPassword: string; newPassword: string; encryptedPrivateKey: string; privateKeyNonce: string; privateKeyKdfSalt: string }) => {
    const user = requireCurrentUser();
    user.encryptedPrivateKey = input.encryptedPrivateKey;
    user.privateKeyNonce = input.privateKeyNonce;
    user.privateKeyKdfSalt = input.privateKeyKdfSalt;
  },
  changeUsername: async (username: string) => {
    const user = requireCurrentUser();
    if (usersByName.has(username.toLowerCase()) && usersByName.get(username.toLowerCase()) !== user) {
      throw new FakeApiError(409, "username_taken", "Username is already taken");
    }
    usersByName.delete(user.username.toLowerCase());
    user.username = username;
    usersByName.set(username.toLowerCase(), user);
  },
  setupMfa: async () => {
    requireCurrentUser();
    return { secretBase32: "FAKESECRETBASE32", otpauthUri: "otpauth://totp/Redvaultsystem:fake?secret=FAKESECRETBASE32" };
  },
  confirmMfa: async (code: string) => {
    const user = requireCurrentUser();
    if (code !== FAKE_MFA_CODE) throw new FakeApiError(401, "invalid_code", "Wrong code");
    user.mfaEnabled = true;
  },
  /** Like the rest of the fake, the password isn't really checked — except "wrong password", to exercise the error path. */
  deleteAccount: async (password: string) => {
    const user = requireCurrentUser();
    if (password === "wrong password") throw new FakeApiError(401, "invalid_credentials", "Invalid username or password");
    usersById.delete(user.userId);
    usersByName.delete(user.username.toLowerCase());
    currentUserId = null;
  },
  disableMfa: async (_password: string) => {
    const user = requireCurrentUser();
    user.mfaEnabled = false;
  },
  lookupUserByUsername: async (username: string) => {
    const user = usersByName.get(username.toLowerCase());
    if (!user) throw new FakeApiError(404, "not_found", "No user with that handle");
    return { userId: user.userId, publicKey: user.publicKey, avatarUrl: user.avatarUrl };
  },
  getUser: async (userId: string) => {
    const user = usersById.get(userId);
    if (!user) throw new FakeApiError(404, "not_found", "User not found");
    return {
      userId: user.userId,
      username: user.username,
      publicKey: user.publicKey,
      avatarUrl: user.avatarUrl,
      createdAt: user.createdAt,
      deleted: user.deleted,
      signingPublicKey: null,
      signingKeySince: null,
    };
  },
  // Message signing and group key rotation (db/migrations/030): accepted, not modelled here.
  registerSigningKey: async () => {
    requireCurrentUser();
  },
  rotateGroupKey: async () => ({ keyEpoch: 0 }),
  uploadAvatar: async (imageBase64: string) => {
    const user = requireCurrentUser();
    user.avatarUrl = `https://fake-avatar-bucket.test/avatars/${user.userId}.jpg?v=${Date.now()}&len=${imageBase64.length}`;
    return { avatarUrl: user.avatarUrl };
  },
  removeAvatar: async () => {
    const user = requireCurrentUser();
    user.avatarUrl = null;
  },
  listConversations: async () => {
    const user = requireCurrentUser();
    return conversations
      .filter((c) => c.participants.some((p) => p.userId === user.userId && p.status === "accepted"))
      .map((c) => conversationDto(c, user.userId));
  },
  createConversation: async (input: { isGroup: boolean; participants: { userId: string; wrappedKey: string }[] }) => {
    const creator = requireCurrentUser();
    const conversation: FakeConversation = {
      id: `conv-${++seq}`,
      isGroup: input.isGroup,
      createdBy: creator.userId,
      createdAt: new Date().toISOString(),
      participants: input.participants.map((p) => ({
        userId: p.userId,
        wrappedKey: p.wrappedKey,
        status: "accepted",
        disappearingSeconds: null,
      })),
    };
    conversations.push(conversation);
    return { id: conversation.id, isGroup: conversation.isGroup, createdAt: conversation.createdAt };
  },
  setDisappearing: async (conversationId: string, seconds: number | null) => {
    const user = requireCurrentUser();
    const conversation = conversations.find((c) => c.id === conversationId);
    const participant = conversation?.participants.find((p) => p.userId === user.userId);
    if (participant) participant.disappearingSeconds = seconds;
  },
  // --- Friends ---
  sendFriendRequest: async (username: string) => {
    const requester = requireCurrentUser();
    const recipient = usersByName.get(username.toLowerCase());
    if (!recipient) throw new FakeApiError(404, "not_found", "No user with that handle");
    if (recipient.userId === requester.userId) {
      throw new FakeApiError(400, "bad_request", "Can't friend yourself");
    }
    if (!recipient.acceptMessageRequests) {
      throw new FakeApiError(403, "forbidden", "This person isn't accepting message requests right now.");
    }
    const existingPending = friendRequests.find(
      (r) =>
        r.status === "pending" &&
        ((r.requesterId === requester.userId && r.recipientId === recipient.userId) ||
          (r.requesterId === recipient.userId && r.recipientId === requester.userId))
    );
    if (existingPending) throw new FakeApiError(409, "already_pending", "A friend request already exists");
    const request: FakeFriendRequest = {
      id: `freq-${++seq}`,
      requesterId: requester.userId,
      recipientId: recipient.userId,
      status: "pending",
      createdAt: new Date().toISOString(),
      respondedAt: null,
    };
    friendRequests.push(request);
    return request;
  },
  listIncomingFriendRequests: async () => {
    const user = requireCurrentUser();
    return friendRequests.filter((r) => r.recipientId === user.userId && r.status === "pending");
  },
  listOutgoingFriendRequests: async () => {
    const user = requireCurrentUser();
    return friendRequests.filter((r) => r.requesterId === user.userId && r.status === "pending");
  },
  acceptFriendRequest: async (
    requestId: string,
    input: { participants: { userId: string; wrappedKey: string }[] }
  ) => {
    const user = requireCurrentUser();
    const request = friendRequests.find((r) => r.id === requestId);
    if (!request || request.recipientId !== user.userId || request.status !== "pending") {
      throw new FakeApiError(404, "not_found", "Friend request not found");
    }
    request.status = "accepted";
    request.respondedAt = new Date().toISOString();
    const conversation: FakeConversation = {
      id: `conv-${++seq}`,
      isGroup: false,
      createdBy: user.userId,
      createdAt: new Date().toISOString(),
      participants: input.participants.map((p) => ({
        userId: p.userId,
        wrappedKey: p.wrappedKey,
        status: "accepted",
        disappearingSeconds: null,
      })),
    };
    conversations.push(conversation);
    return { id: conversation.id, isGroup: conversation.isGroup, createdAt: conversation.createdAt };
  },
  denyFriendRequest: async (requestId: string) => {
    const user = requireCurrentUser();
    const request = friendRequests.find((r) => r.id === requestId);
    if (request && request.recipientId === user.userId && request.status === "pending") {
      request.status = "denied";
      request.respondedAt = new Date().toISOString();
    }
  },
  cancelFriendRequest: async (requestId: string) => {
    const user = requireCurrentUser();
    const request = friendRequests.find((r) => r.id === requestId);
    if (request && request.requesterId === user.userId && request.status === "pending") {
      request.status = "canceled";
      request.respondedAt = new Date().toISOString();
    }
  },
  listFriends: async () => {
    requireCurrentUser();
    return [];
  },
  // --- Reports ---
  submitReport: async (reportedUserId: string, reason: string) => {
    const reporter = requireCurrentUser();
    reports.push({
      id: `report-${++seq}`,
      reporterId: reporter.userId,
      reportedId: reportedUserId,
      reason,
      status: "open",
      createdAt: new Date().toISOString(),
      reviewedAt: null,
      reviewedBy: null,
    });
  },
  // --- Admin ---
  adminListUsers: async () => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    return [...usersById.values()]
      .filter((u) => !u.deleted)
      .map((u) => ({
        userId: u.userId,
        username: u.username,
        role: u.role,
        subscriptionTier: u.subscriptionTier,
        createdAt: u.createdAt,
        deletedAt: null,
      }));
  },
  adminCreateUser: async (consented: Record<string, string | boolean>) => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    if (consented.terms_accepted !== true || consented.privacy_acknowledged !== true || consented.age_18_confirmed !== true) {
      throw new FakeApiError(400, "consent_required", "To create an account, agree to the Terms of Service, acknowledge the Privacy Policy and confirm you are 18 or older.");
    }
    const input = consented as Record<string, string>;
    if (usersByName.has(input.username.toLowerCase())) {
      throw new FakeApiError(409, "username_taken", "Username is already taken");
    }
    const user: FakeUser = {
      userId: `user-${++seq}`,
      username: input.username,
      publicKey: input.publicKey,
      encryptedPrivateKey: input.encryptedPrivateKey,
      privateKeyNonce: input.privateKeyNonce,
      privateKeyKdfSalt: input.privateKeyKdfSalt,
      acceptMessageRequests: true,
      transparencyMode: false,
      avatarUrl: null,
      role: "user",
      subscriptionTier: "free",
      mfaEnabled: false,
      createdAt: new Date().toISOString(),
      deleted: false,
    };
    usersByName.set(input.username.toLowerCase(), user);
    usersById.set(user.userId, user);
    return { userId: user.userId };
  },
  adminDeleteUser: async (userId: string) => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    const user = usersById.get(userId);
    if (user) user.deleted = true;
  },
  adminSetTier: async (userId: string, tier: "free" | "pro") => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    const user = usersById.get(userId);
    if (user) user.subscriptionTier = tier;
  },
  adminListReports: async (status?: "open" | "reviewed" | "dismissed") => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    return reports
      .filter((r) => !status || r.status === status)
      .map((r) => ({
        ...r,
        reporterUsername: usersById.get(r.reporterId)?.username,
        reportedUsername: usersById.get(r.reportedId)?.username,
      }));
  },
  adminResolveReport: async (reportId: string, status: "reviewed" | "dismissed") => {
    const admin = requireCurrentUser();
    if (admin.role !== "super_admin") throw new FakeApiError(403, "forbidden", "Super admin only");
    const report = reports.find((r) => r.id === reportId);
    if (report) {
      report.status = status;
      report.reviewedAt = new Date().toISOString();
      report.reviewedBy = admin.userId;
    }
  },
  // Forward-secret messaging stays off against this fake: these tests cover
  // the conversation-key path (e2eV2Messaging.test.tsx covers the other).
  getE2eConfig: async () => ({ enabled: false }),
  listParticipants: async (conversationId: string) => {
    const conversation = conversations.find((c) => c.id === conversationId);
    return (conversation?.participants ?? []).map((p) => ({
      userId: p.userId,
      role: "member" as const,
      joinedAt: new Date().toISOString(),
    }));
  },
  sendMessage: async (conversationId: string, ciphertext: string, nonce: string) => {
    const user = requireCurrentUser();
    const conversation = conversations.find((c) => c.id === conversationId);
    const sender = conversation?.participants.find((p) => p.userId === user.userId);
    const message: FakeMessage = {
      id: `msg-${++seq}`,
      conversationId,
      senderId: user.userId,
      ciphertext,
      nonce,
      createdAt: new Date().toISOString(),
      disappearAfterSeconds: sender?.disappearingSeconds ?? null,
    };
    messages.push(message);
    // Mirrors MessageService.send: independently computed here, from the
    // exact bytes just "persisted" — see commitment.ts. This is the same
    // function the real client uses to recompute its own local commitment,
    // so a test that never deliberately corrupts things will always see a
    // match, exactly like talking to the real server would.
    await crypto.ready();
    const commitment = commitmentToHex(
      computeCommitment({ messageId: message.id, ciphertext: crypto.fromBase64(ciphertext), nonce: crypto.fromBase64(nonce) })
    );
    const verification: FakeVerification = {
      publicTransactionId: `tx_fake-${message.id}`,
      commitment,
      protocolVersion: 1,
      relayStatus: "relayed",
      createdAt: message.createdAt,
    };
    verifications.set(message.id, verification);
    return { ...message, verification };
  },
  getMessageVerification: async (messageId: string) => {
    requireCurrentUser();
    const verification = verifications.get(messageId);
    if (!verification) throw new FakeApiError(404, "not_found", "No verification record for this message");
    return verification;
  },
  // Mirrors TransparencyRepository.listPublicVisible's live opt-in check —
  // unauthenticated, and only ever includes a message whose sender
  // currently has transparencyMode on.
  getTransparencyFeed: async (options?: { limit?: number }) => {
    const visible = messages
      .filter((m) => usersById.get(m.senderId)?.transparencyMode)
      .map((m) => {
        const verification = verifications.get(m.id);
        if (!verification) return null;
        return {
          publicTransactionId: verification.publicTransactionId,
          commitment: verification.commitment,
          protocolVersion: verification.protocolVersion,
          relayStatus: verification.relayStatus,
          timestamp: verification.createdAt,
          ciphertext: m.ciphertext,
          expired: false,
        };
      })
      .filter((e): e is NonNullable<typeof e> => e !== null)
      .reverse();
    return visible.slice(0, options?.limit ?? 20);
  },
  listMessages: async (conversationId: string) => {
    expireDisappearingMessages(conversationId);
    return messages.filter((m) => m.conversationId === conversationId);
  },
  deleteMessage: async (messageId: string) => {
    const index = messages.findIndex((m) => m.id === messageId);
    if (index !== -1) messages.splice(index, 1);
  },
  markConversationRead: async (conversationId: string, status: "delivered" | "read") => {
    const user = requireCurrentUser();
    const now = new Date().toISOString();
    const upsert = (messageId: string, s: "delivered" | "read") => {
      const existing = receipts.find((r) => r.messageId === messageId && r.userId === user.userId && r.status === s);
      if (existing) existing.statusAt = now;
      else receipts.push({ messageId, userId: user.userId, status: s, statusAt: now });
    };
    for (const m of messages) {
      if (m.conversationId !== conversationId || m.senderId === user.userId) continue;
      // Mirrors MessageService.markRead: 'delivered' is always recorded,
      // even when the caller only asked for 'read' — otherwise an account
      // with read receipts on would never get a real "Delivered" event.
      upsert(m.id, "delivered");
      if (status === "read") upsert(m.id, "read");
    }
  },
  listReceipts: async (conversationId: string) => {
    const messageIds = new Set(messages.filter((m) => m.conversationId === conversationId).map((m) => m.id));
    return receipts.filter((r) => messageIds.has(r.messageId));
  },
  // Mirrors the real server: 503 file_sharing_disabled when nothing
  // configures file sharing — which, in this base fake (no file-upload
  // simulation), is always. Tests that layer on the file-sharing fake
  // (fakeFileBackend.ts's withFakeFileApi) override this with real numbers.
  getFileStorageUsage: async () => {
    requireCurrentUser();
    throw new FakeApiError(503, "file_sharing_disabled", "File sharing isn't enabled on this server.");
  },
};
