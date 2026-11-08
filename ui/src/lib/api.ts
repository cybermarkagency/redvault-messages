// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { RecoveryMaterial } from "./recoveryKey.js";
// Thin fetch wrapper for the real backend. Requests go to relative /api
// paths — the Vite dev server proxies those to the server (see
// vite.config.ts), so the session cookie stays same-origin without any
// server-side CORS setup. In production this app can be hosted under a
// path prefix (e.g. www.redvaultsystems.com/redvaultmessages) alongside
// other apps on the same domain, in which case nginx proxies
// <prefix>/api/* to the Node server — set VITE_API_BASE to that prefixed
// path at build time (see ui/README or the deploy notes) so requests still
// land on the right upstream. Defaults to root-relative /api for dev.
import { API_BASE, resolveApiUrl } from "./apiBase.js";

export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    message: string
  ) {
    super(message);
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    credentials: "include",
    headers: { "Content-Type": "application/json", ...init?.headers },
  });

  if (res.status === 204) return undefined as T;

  const body = await res.json().catch(() => null);
  if (!res.ok) {
    throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  }
  return body as T;
}

import type { SubscriptionTier, UserRole } from "../types.js";

export interface RegisterInput {
  username: string;
  password: string;
  publicKey: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfSalt: string;
  /** 2: `password` is the sign-in key (lib/authKey.ts). */
  authVersion?: 2;
  kdfOps?: number;
  kdfMem?: number;
  /** Set at sign-up so the account can be recovered with the recovery key (lib/recoveryKey.ts). */
  recovery?: RecoveryMaterial;
  /** The sign-up form's three boxes, as ticked; the server refuses the account unless all are true. */
  terms_accepted: boolean;
  privacy_acknowledged: boolean;
  age_18_confirmed: boolean;
}

export interface AuthResult {
  userId: string;
  acceptMessageRequests: boolean;
  transparencyMode: boolean;
  /** Absent on older servers. */
  showOnlineStatus?: boolean;
  avatarUrl: string | null;
  expiresAt: string;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
}

export interface LoginResult extends AuthResult {
  publicKey: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfOps?: number;
  privateKeyKdfMem?: number;
  privateKeyKdfSalt: string;
}

/** Returned instead of LoginResult when the account has MFA enabled — no key material is included until /auth/mfa/verify succeeds. */
export interface MfaRequiredResult {
  mfaRequired: true;
}

export function register(input: RegisterInput): Promise<AuthResult> {
  return request("/auth/register", { method: "POST", body: JSON.stringify(input) });
}

export interface PreLoginResult {
  kdfSalt: string;
  kdfOps?: number;
  kdfMem?: number;
  authVersion: 1 | 2;
  upgrade: boolean;
}

/** The salt and version needed to derive the sign-in key. Answers for any username. */
/** Makes an uploaded file downloadable without a server message: its key travels in a device-key envelope. */
export function releaseFile(objectId: string): Promise<{ objectId: string; status: string }> {
  return request(`/v1/files/${objectId}/release`, { method: "POST", body: "{}" });
}

/** A group's signed key notice (lib/keyTrust.ts). */
export interface ChatKeyNotice {
  epoch: number;
  seq: number;
  issuerId: string;
  members: string[];
  signature: string;
}

export function publishKeyNotice(conversationId: string, notice: ChatKeyNotice): Promise<void> {
  return request(`/conversations/${conversationId}/key-notice`, { method: "PUT", body: JSON.stringify(notice) });
}

export function preLogin(username: string): Promise<PreLoginResult> {
  return request("/auth/prelogin", { method: "POST", body: JSON.stringify({ username }) });
}

export function login(
  username: string,
  credential: { password: string; authVersion?: 2; upgradeAuthKey?: string }
): Promise<LoginResult | MfaRequiredResult> {
  return request("/auth/login", { method: "POST", body: JSON.stringify({ username, ...credential }) });
}

/** Completes login after a `login()` call returned `{mfaRequired: true}` — reads the pending session directly off the cookie, no requireAuth. Returns the same shape a normal login would, key material included. */
export function verifyMfaCode(code: string): Promise<LoginResult> {
  return request("/auth/mfa/verify", { method: "POST", body: JSON.stringify({ code }) });
}

export function logout(): Promise<void> {
  return request("/auth/logout", { method: "POST" });
}

export interface MeResult {
  userId: string;
  username: string;
  publicKey: string;
  acceptMessageRequests: boolean;
  transparencyMode: boolean;
  /** Opt-in, reciprocal online status. Absent on older servers. */
  showOnlineStatus?: boolean;
  avatarUrl: string | null;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
  mfaEnabled: boolean;
  hasRecoveryKey?: boolean;
}

export function me(): Promise<MeResult> {
  return request("/auth/me");
}

export function updateSettings(input: { acceptMessageRequests?: boolean; transparencyMode?: boolean; showOnlineStatus?: boolean }): Promise<void> {
  return request("/auth/settings", { method: "PATCH", body: JSON.stringify(input) });
}

/** Verifies current password, re-derives + re-wraps the private key client-side with the new password, and sends both together atomically. Revokes other sessions server-side. */
export function changePassword(input: {
  currentPassword: string;
  newPassword: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfSalt: string;
  authVersion?: 2;
  kdfOps?: number;
  kdfMem?: number;
}): Promise<void> {
  return request("/auth/password", { method: "PATCH", body: JSON.stringify(input) });
}

/** Sets or replaces the recovery key (needs the current password). Only derived values are sent, never the key. */
export function setRecoveryKey(currentPassword: string, material: RecoveryMaterial): Promise<void> {
  return request("/auth/recovery", { method: "PUT", body: JSON.stringify({ currentPassword, ...material }) });
}

export interface RecoveryStartResult {
  publicKey: string;
  recoveryEncryptedPrivateKey: string;
  recoveryPrivateKeyNonce: string;
  mfaRequired: boolean;
}

/** Step 1 of recovery (not signed in): proves the key and fetches the recovery-wrapped private key. */
export function recoveryStart(username: string, recoveryAuth: string): Promise<RecoveryStartResult> {
  return request("/auth/recovery/start", { method: "POST", body: JSON.stringify({ username, recoveryAuth }) });
}

/** Step 2: new password + the private key re-wrapped under it on this device. Signs in; other sessions end. */
export function recoveryComplete(input: {
  username: string;
  recoveryAuth: string;
  code?: string;
  newPassword: string;
  encryptedPrivateKey: string;
  privateKeyNonce: string;
  privateKeyKdfSalt: string;
  authVersion?: 2;
  kdfOps?: number;
  kdfMem?: number;
}): Promise<LoginResult> {
  return request("/auth/recovery/complete", { method: "POST", body: JSON.stringify(input) });
}

export function changeUsername(username: string): Promise<void> {
  return request("/auth/username", { method: "PATCH", body: JSON.stringify({ username }) });
}

export interface MfaSetupResult {
  secretBase32: string;
  otpauthUri: string;
}

export function setupMfa(): Promise<MfaSetupResult> {
  return request("/auth/mfa/setup", { method: "POST" });
}

export function confirmMfa(code: string): Promise<void> {
  return request("/auth/mfa/confirm", { method: "POST", body: JSON.stringify({ code }) });
}

/** Permanently deletes the signed-in account (password re-checked) and signs out every device. */
export function deleteAccount(password: string): Promise<void> {
  return request("/auth/delete-account", { method: "POST", body: JSON.stringify({ password }) });
}

export function disableMfa(password: string): Promise<void> {
  return request("/auth/mfa/disable", { method: "POST", body: JSON.stringify({ password }) });
}

export interface UserLookupResult {
  userId: string;
  publicKey: string;
  avatarUrl: string | null;
}

export function lookupUserByUsername(username: string): Promise<UserLookupResult> {
  return request(`/users/lookup?username=${encodeURIComponent(username)}`);
}

export interface UserResult {
  userId: string;
  username: string;
  publicKey: string;
  avatarUrl: string | null;
  createdAt: string;
  deleted: boolean;
  /** Their Ed25519 message-signing key (db/migrations/030); null until one of their updated apps registered it. Absent on older servers. */
  signingPublicKey?: string | null;
  /** "YYYY-MM-DD": group messages from them after this day should be signed. */
  signingKeySince?: string | null;
}

export function getUser(userId: string): Promise<UserResult> {
  return request(`/users/${userId}`);
}

/** imageBase64 should already be a small, resized JPEG — see lib/avatar.ts's readImageAsAvatar. */
export function uploadAvatar(imageBase64: string): Promise<{ avatarUrl: string }> {
  return request("/users/me/avatar", { method: "PUT", body: JSON.stringify({ image: imageBase64 }) });
}

export function removeAvatar(): Promise<void> {
  return request("/users/me/avatar", { method: "DELETE" });
}

export interface ConversationDto {
  id: string;
  isGroup: boolean;
  role: "owner" | "admin" | "member";
  nameCiphertext: string | null;
  nameNonce: string | null;
  /** This user's sealed copy of the current key. */
  wrappedKey: string;
  /** The current key's epoch (db/migrations/030); absent on older servers (= 0). */
  keyEpoch?: number;
  /** Every epoch's key this user holds; older messages need older keys. */
  keys?: { epoch: number; wrappedKey: string }[];
  keyNotice?: ChatKeyNotice | null;
  /** Someone left or was removed: a member's app must rotate the key before anything else is sent. */
  rekeyRequired?: boolean;
  disappearingSeconds: number | null;
  createdBy: string;
  createdAt: string;
  /** Present for a paid or free community (lib/communityApi.ts); the server lists them only because we ask. */
  community?: {
    id: string;
    name: string;
    kind: "channel" | "group";
    status: string;
    hasAvatar: boolean;
    role: "owner" | "admin" | "moderator" | "member";
    canPost: boolean;
    keyEpoch: number;
    rekeyRequired: boolean;
  };
}

/** Includes communities: this app reads their key epochs (lib/communityKeys.ts). */
export function listConversations(): Promise<ConversationDto[]> {
  return request("/conversations?include=communities");
}

export function setDisappearing(conversationId: string, seconds: number | null): Promise<void> {
  return request(`/conversations/${conversationId}/disappearing`, {
    method: "PATCH",
    body: JSON.stringify({ seconds }),
  });
}

export interface CreateConversationInput {
  isGroup: boolean;
  nameCiphertext?: string | null;
  nameNonce?: string | null;
  participants: { userId: string; wrappedKey: string }[];
}

export interface CreateConversationResult {
  id: string;
  isGroup: boolean;
  createdAt: string;
}

export function createConversation(input: CreateConversationInput): Promise<CreateConversationResult> {
  return request("/conversations", { method: "POST", body: JSON.stringify(input) });
}

export interface ParticipantDto {
  userId: string;
  role: "owner" | "admin" | "member";
  joinedAt: string;
}

export function listParticipants(conversationId: string): Promise<ParticipantDto[]> {
  return request(`/conversations/${conversationId}/participants`);
}

/** Stage-3 "Transaction verified" record — see server/src/services/transparencyService.ts. `commitment` is hex-encoded SHA-256. */
export interface VerificationDto {
  publicTransactionId: string;
  commitment: string;
  protocolVersion: number;
  relayStatus: "relayed";
  createdAt: string;
  /** Its entry in the verifiable transparency log; null until the next checkpoint logs it. Absent on older servers. */
  logIndex?: number | null;
  sizeKb?: number | null;
}

export interface MessageDto {
  id: string;
  conversationId: string;
  senderId: string;
  ciphertext: string;
  nonce: string;
  createdAt: string;
  editedAt: string | null;
  deletedAt: string | null;
  /** When the server's retention window ends, null if it has none or once deletedAt is set. Drives the client's retention warning banner. */
  deleteAt: string | null;
  /** Encrypted files this message's (E2EE) descriptor refers to — only ids + server-side status, never keys or names. */
  attachments?: AttachmentDto[];
  /** Only ever populated on the response to sendMessage() (null there only for the rare replay-of-an-older-message path) — GET /conversations/:id/messages omits it; fetch it on demand with getMessageVerification. */
  verification?: VerificationDto | null;
  /** 1 (absent on older servers): encrypted with the conversation key. 2: forward-secret, see lib/e2eV2. */
  protocol?: 1 | 2;
  senderDeviceId?: string | null;
  /** Protocol 2, listed for one device: that device's Double Ratchet payload, or null when it has none. */
  payload?: string | null;
  /** Which group key (epoch) the body is encrypted with; absent on older servers (= 0). */
  keyEpoch?: number;
  /** The sender's Ed25519 signature (lib/e2eCrypto.ts messageSignatureInput); null from apps that predate signing. */
  signature?: string | null;
}

/** On-demand re-fetch of a message's stage-3 verification record — used to re-verify after a reload/poll, or to populate the "View Verification Details" screen. Participant-checked server-side. */
export function getMessageVerification(messageId: string): Promise<VerificationDto> {
  return request(`/messages/${messageId}/verification`);
}

// --- Public live transparency feed ---
//
// Unauthenticated, no cookies needed — see server/src/routes/transparencyRoutes.ts.
// `ciphertext` is populated only for the fixed sandbox conversation, or for
// a message whose sender has Transparency Mode on (their own explicit
// opt-in) — never plaintext, never a key.

export interface PublicTransparencyEventDto {
  publicTransactionId: string;
  commitment: string;
  protocolVersion: number;
  relayStatus: "relayed";
  timestamp: string;
  ciphertext: string | null;
  expired: boolean;
}

export function getTransparencyFeed(options?: { limit?: number; before?: string }): Promise<PublicTransparencyEventDto[]> {
  const params = new URLSearchParams();
  if (options?.limit) params.set("limit", String(options.limit));
  if (options?.before) params.set("before", options.before);
  const qs = params.toString() ? `?${params.toString()}` : "";
  return request(`/public/transparency-feed${qs}`);
}

export type AttachmentStatus = "pending" | "uploaded" | "available" | "deleted" | "expired" | "failed";

export interface AttachmentDto {
  objectId: string;
  status: AttachmentStatus;
  encryptedSize: number | null;
}

/** Protocol 2: the sending device and one Double Ratchet payload per other device in the conversation. */
export interface ForwardSecretEnvelope {
  senderDeviceId: string;
  payloads: { deviceId: string; payload: string }[];
}

/** `attachments` = encrypted-file object ids whose descriptors are inside `ciphertext`. Safe to retry: re-sending the same attachments returns the original message rather than a duplicate. */
export function sendMessage(
  conversationId: string,
  ciphertext: string,
  nonce: string,
  attachments?: string[],
  forwardSecret?: ForwardSecretEnvelope,
  /** Protocol 1: the group key epoch the body is encrypted with, and the sender's signature over it. */
  signed?: { keyEpoch: number; signature?: string },
  /** The outbox's id for this send (db/migrations/033): a retry with it returns the message already stored. */
  clientMessageId?: string,
  /** To someone who charges for messages: the paid message this uses, or the free safety contact. */
  paid?: PaidSend
): Promise<MessageDto> {
  return request(`/conversations/${conversationId}/messages`, {
    method: "POST",
    body: JSON.stringify({
      ciphertext,
      nonce,
      ...(clientMessageId ? { clientMessageId } : {}),
      ...(attachments?.length ? { attachments } : {}),
      ...(forwardSecret ? { protocol: 2, ...forwardSecret } : {}),
      ...(signed && !forwardSecret ? { keyEpoch: signed.keyEpoch, ...(signed.signature ? { signature: signed.signature } : {}) } : {}),
      ...(paid?.entitlementId ? { entitlementId: paid.entitlementId } : {}),
      ...(paid?.safetyContact ? { safetyContact: true } : {}),
    }),
  });
}

/** Registers this account's message-signing public key (set once; the same key again is fine). */
export function registerSigningKey(signingPublicKey: string): Promise<void> {
  return request("/auth/signing-key", { method: "PUT", body: JSON.stringify({ signingPublicKey }) });
}

/** Moves a group to its next key: one sealed copy per current member, and the name re-encrypted with it. */
export function rotateGroupKey(
  conversationId: string,
  body: { fromEpoch: number; keys: { userId: string; wrappedKey: string }[]; nameCiphertext: string | null; nameNonce: string | null; keyNotice?: ChatKeyNotice }
): Promise<{ keyEpoch: number }> {
  return request(`/conversations/${conversationId}/keys`, { method: "POST", body: JSON.stringify(body) });
}

/** Paid messages (lib/earningsApi.ts): which purchase a send uses, or the free safety path. */
export interface PaidSend {
  entitlementId?: string;
  safetyContact?: boolean;
}

// --- Forward-secret messaging: devices and prekeys (lib/e2eV2, docs/e2e-v2-protocol.md) ---

export interface E2eDeviceDto {
  id: string;
  createdOn: string;
  signedPreKeyId: number;
  pqPreKeyId: number;
  /** Only on getE2eDevice. */
  oneTimePreKeyCount?: number;
}

export interface E2ePreKeysInput {
  signedPreKey: { id: number; publicKey: string; signature: string };
  pqPreKey: { id: number; publicKey: string; signature: string };
}

export interface E2eOneTimePreKeyInput {
  id: number;
  publicKey: string;
}

export interface PreKeyBundleDto {
  userId: string;
  deviceId: string;
  identityKey: string;
  signedPreKey: { id: number; publicKey: string; signature: string };
  pqPreKey: { id: number; publicKey: string; signature: string };
  oneTimePreKey: { id: number; publicKey: string } | null;
}

export function getE2eConfig(): Promise<{ enabled: boolean }> {
  return request("/e2e/config");
}

export function registerE2eDevice(input: E2ePreKeysInput & { oneTimePreKeys: E2eOneTimePreKeyInput[] }): Promise<E2eDeviceDto> {
  return request("/e2e/devices", { method: "POST", body: JSON.stringify(input) });
}

export function listE2eDevices(): Promise<E2eDeviceDto[]> {
  return request("/e2e/devices");
}

export function getE2eDevice(deviceId: string): Promise<E2eDeviceDto> {
  return request(`/e2e/devices/${deviceId}`);
}

export function rotateE2ePreKeys(deviceId: string, input: E2ePreKeysInput): Promise<void> {
  return request(`/e2e/devices/${deviceId}/prekeys`, { method: "PUT", body: JSON.stringify(input) });
}

export function addE2eOneTimePreKeys(deviceId: string, keys: E2eOneTimePreKeyInput[]): Promise<void> {
  return request(`/e2e/devices/${deviceId}/one-time-prekeys`, { method: "POST", body: JSON.stringify({ keys }) });
}

export function deleteE2eDevice(deviceId: string): Promise<void> {
  return request(`/e2e/devices/${deviceId}`, { method: "DELETE" });
}

/** This device has decrypted these messages: the server deletes its payloads for them. */
export function acknowledgeE2ePayloads(deviceId: string, messageIds: string[]): Promise<void> {
  return request(`/e2e/devices/${deviceId}/acks`, { method: "POST", body: JSON.stringify({ messageIds }) });
}

export function listConversationDevices(conversationId: string): Promise<{ userId: string; deviceIds: string[] }[]> {
  return request(`/e2e/conversations/${conversationId}/devices`);
}

/** Claims one one-time prekey per device, when it has any left. */
export function fetchPreKeyBundles(conversationId: string, devices: { userId: string; deviceId: string }[]): Promise<PreKeyBundleDto[]> {
  return request("/e2e/bundles", { method: "POST", body: JSON.stringify({ conversationId, devices }) });
}

// --- End-to-end encrypted files ---
//
// Only ciphertext sizes/hashes and opaque object ids ever go through these
// calls. The file key, filename and MIME type travel solely inside the E2EE
// message body (see lib/fileMessage.ts).

import type { TransferTargetDto } from "./fileTransfer.js";

export interface FileConfigDto {
  maxFileSize: number;
  maxEncryptedSize: number;
  maxAttachmentsPerMessage: number;
  retentionDays: number;
  transferMode: "proxy" | "direct";
  /** Total storage (bytes) a Pro account may have in use at once — see server/src/services/fileService.ts's storageLimitFor. Free accounts can't send files. */
  proStorageLimitBytes: number;
}

export function getFileConfig(): Promise<FileConfigDto> {
  return request("/v1/files/config");
}

/** Your own storage in use vs. your own tier's limit — never anyone else's. */
export interface FileStorageUsageDto {
  used: number;
  limit: number;
}

export function getFileStorageUsage(): Promise<FileStorageUsageDto> {
  return request("/v1/files/storage");
}

export interface FileUploadAuthorizationDto {
  objectId: string;
  upload: TransferTargetDto;
  maxEncryptedSize: number;
}

export function requestFileUpload(input: {
  conversationId: string;
  encryptedSize: number;
  ciphertextSha256: string;
}): Promise<FileUploadAuthorizationDto> {
  return request("/v1/files/upload-request", { method: "POST", body: JSON.stringify(input) });
}

export interface FileStatusDto {
  objectId: string;
  status: AttachmentStatus;
  encryptedSize: number | null;
}

export function completeFileUpload(objectId: string): Promise<FileStatusDto> {
  return request(`/v1/files/${encodeURIComponent(objectId)}/complete`, { method: "POST", body: "{}" });
}

export interface FileDownloadDto {
  objectId: string;
  encryptedSize: number | null;
  download: TransferTargetDto;
}

export function getFileDownload(objectId: string): Promise<FileDownloadDto> {
  return request(`/v1/files/${encodeURIComponent(objectId)}/download`);
}

export function deleteFile(objectId: string): Promise<void> {
  return request(`/v1/files/${encodeURIComponent(objectId)}`, { method: "DELETE" });
}

/** `deviceId`: this browser's forward-secret device, to get its payload on each protocol-2 message. */
export function listMessages(conversationId: string, options?: { limit?: number; deviceId?: string }): Promise<MessageDto[]> {
  const params = new URLSearchParams();
  if (options?.limit) params.set("limit", String(options.limit));
  if (options?.deviceId) params.set("deviceId", options.deviceId);
  const qs = params.toString();
  return request(`/conversations/${conversationId}/messages${qs ? `?${qs}` : ""}`);
}

export function deleteMessage(messageId: string): Promise<void> {
  return request(`/messages/${messageId}`, { method: "DELETE" });
}

export interface ReceiptDto {
  messageId: string;
  userId: string;
  status: "delivered" | "read";
  statusAt: string;
}

/** Marks every message the caller didn't send as seen by them. 'read' surfaces the "Seen" indicator to the sender; 'delivered' is private bookkeeping only the caller's own client uses to keep its unread count accurate. */
export function markConversationRead(conversationId: string, status: "delivered" | "read"): Promise<void> {
  return request(`/conversations/${conversationId}/read`, {
    method: "POST",
    body: JSON.stringify({ status }),
  });
}

export function listReceipts(conversationId: string): Promise<ReceiptDto[]> {
  return request(`/conversations/${conversationId}/receipts`);
}

// --- Friends (replaces the old per-conversation message-request gate) ---

export type FriendRequestStatus = "pending" | "accepted" | "denied" | "canceled";

export interface FriendRequestDto {
  id: string;
  requesterId: string;
  recipientId: string;
  status: FriendRequestStatus;
  createdAt: string;
  respondedAt: string | null;
}

export function sendFriendRequest(username: string): Promise<FriendRequestDto> {
  return request("/friend-requests", { method: "POST", body: JSON.stringify({ username }) });
}

export function listIncomingFriendRequests(): Promise<FriendRequestDto[]> {
  return request("/friend-requests/incoming");
}

export function listOutgoingFriendRequests(): Promise<FriendRequestDto[]> {
  return request("/friend-requests/outgoing");
}

/** Accepts a friend request — the caller must generate a fresh conversation key and seal it for both sides (see state/actions.ts's acceptFriendRequest). Returns the newly-created conversation. */
export function acceptFriendRequest(
  requestId: string,
  input: { participants: { userId: string; wrappedKey: string }[] }
): Promise<CreateConversationResult> {
  return request(`/friend-requests/${requestId}/accept`, { method: "POST", body: JSON.stringify(input) });
}

export function denyFriendRequest(requestId: string): Promise<void> {
  return request(`/friend-requests/${requestId}/deny`, { method: "POST" });
}

export function cancelFriendRequest(requestId: string): Promise<void> {
  return request(`/friend-requests/${requestId}/cancel`, { method: "POST" });
}

export interface FriendDto {
  userId: string;
  username: string;
  publicKey: string;
  avatarUrl: string | null;
}

/** Simple flat list of accepted friends — e.g. for group-member pickers. Not currently consumed by the UI (NewGroup still sources members from existing DM contacts). */
export function listFriends(): Promise<FriendDto[]> {
  return request("/friends");
}

// --- Reports ---

export function submitReport(reportedUserId: string, reason: string): Promise<void> {
  return request("/reports", { method: "POST", body: JSON.stringify({ reportedUserId, reason }) });
}

// --- Announcements: what an admin sent to you (plain text, not end-to-end encrypted) ---

export interface AnnouncementDto {
  id: string;
  kind: "message" | "poll";
  body: string;
  allowReplies: boolean;
  createdAt: string;
  read: boolean;
  options: { id: string; label: string }[];
  myOptionId: string | null;
}

export interface AnnouncementReplyDto {
  id: string;
  userId: string;
  /** Set on the admin's side only. */
  username?: string;
  body: string;
  createdAt: string;
}

export function listAnnouncements(): Promise<AnnouncementDto[]> {
  return request("/announcements");
}

export function getAnnouncement(id: string): Promise<AnnouncementDto & { replies: AnnouncementReplyDto[] }> {
  return request(`/announcements/${id}`);
}

export function markAnnouncementRead(id: string): Promise<void> {
  return request(`/announcements/${id}/read`, { method: "POST" });
}

export function voteInAnnouncement(id: string, optionId: string): Promise<void> {
  return request(`/announcements/${id}/vote`, { method: "POST", body: JSON.stringify({ optionId }) });
}

export function replyToAnnouncement(id: string, body: string): Promise<AnnouncementReplyDto> {
  return request(`/announcements/${id}/replies`, { method: "POST", body: JSON.stringify({ body }) });
}

// --- Admin (requireAuth + requireSuperAdmin server-side) ---

export interface AdminBroadcastDto {
  id: string;
  kind: "message" | "poll";
  body: string;
  allowReplies: boolean;
  createdAt: string;
  recipients: number;
  read: number;
  replyCount: number;
  options: { id: string; label: string; votes: number }[];
}

export interface SendBroadcastInput {
  kind: "message" | "poll";
  body: string;
  options: string[];
  allowReplies: boolean;
  recipientIds: string[];
}

export function adminSendBroadcast(input: SendBroadcastInput): Promise<AdminBroadcastDto> {
  return request("/admin/broadcasts", { method: "POST", body: JSON.stringify(input) });
}

export function adminListBroadcasts(): Promise<AdminBroadcastDto[]> {
  return request("/admin/broadcasts");
}

export function adminGetBroadcast(id: string): Promise<AdminBroadcastDto & { replies: AnnouncementReplyDto[] }> {
  return request(`/admin/broadcasts/${id}`);
}

export interface AdminUserDto {
  userId: string;
  username: string;
  role: UserRole;
  subscriptionTier: SubscriptionTier;
  createdAt: string;
  deletedAt: string | null;
}

export function adminListUsers(options?: { limit?: number; offset?: number }): Promise<AdminUserDto[]> {
  const params = new URLSearchParams();
  if (options?.limit) params.set("limit", String(options.limit));
  if (options?.offset) params.set("offset", String(options.offset));
  const qs = params.toString() ? `?${params.toString()}` : "";
  return request(`/admin/users${qs}`);
}

export function adminCreateUser(input: RegisterInput): Promise<{ userId: string }> {
  return request("/admin/users", { method: "POST", body: JSON.stringify(input) });
}

export function adminDeleteUser(userId: string): Promise<void> {
  return request(`/admin/users/${userId}`, { method: "DELETE" });
}

export function adminSetTier(userId: string, tier: SubscriptionTier): Promise<void> {
  return request(`/admin/users/${userId}/tier`, { method: "PATCH", body: JSON.stringify({ tier }) });
}

export type ReportStatus = "open" | "reviewed" | "dismissed";

export interface AdminReportDto {
  id: string;
  reporterId: string;
  reportedId: string;
  reason: string;
  status: ReportStatus;
  createdAt: string;
  reviewedAt: string | null;
  reviewedBy: string | null;
  /** Convenience fields the admin listing may denormalize for display — not guaranteed, so callers should fall back to the raw id. */
  reporterUsername?: string;
  reportedUsername?: string;
}

export function adminListReports(status?: ReportStatus): Promise<AdminReportDto[]> {
  const qs = status ? `?status=${encodeURIComponent(status)}` : "";
  return request(`/admin/reports${qs}`);
}

export function adminResolveReport(reportId: string, status: "reviewed" | "dismissed"): Promise<void> {
  return request(`/admin/reports/${reportId}`, { method: "PATCH", body: JSON.stringify({ status }) });
}

// --- Blocking (server-enforced: no messages or friend requests either way) ---

export interface BlockedUserDto {
  userId: string;
  username: string;
  avatarUrl: string | null;
}

export function listBlocked(): Promise<BlockedUserDto[]> {
  return request("/blocks");
}

export function blockUser(userId: string): Promise<void> {
  return request("/blocks", { method: "POST", body: JSON.stringify({ userId }) });
}

export function unblockUser(userId: string): Promise<void> {
  return request(`/blocks/${userId}`, { method: "DELETE" });
}

// --- Groups (server-enforced: friends only; owner/admin manage members) ---

export function addParticipant(conversationId: string, userId: string, wrappedKey: string, keyEpoch?: number, keyNotice?: ChatKeyNotice): Promise<void> {
  return request(`/conversations/${conversationId}/participants`, { method: "POST", body: JSON.stringify({ userId, wrappedKey, keyEpoch, keyNotice }) });
}

/** Remove a member (owner/admin), or leave (your own id). */
export function removeParticipant(conversationId: string, userId: string): Promise<void> {
  return request(`/conversations/${conversationId}/participants/${userId}`, { method: "DELETE" });
}

export function setParticipantRole(conversationId: string, userId: string, role: "admin" | "member"): Promise<void> {
  return request(`/conversations/${conversationId}/participants/${userId}/role`, { method: "PATCH", body: JSON.stringify({ role }) });
}

/** The name is encrypted with the group key; the server stores only ciphertext. */
export function renameConversation(conversationId: string, nameCiphertext: string, nameNonce: string): Promise<void> {
  return request(`/conversations/${conversationId}/name`, { method: "PATCH", body: JSON.stringify({ nameCiphertext, nameNonce }) });
}

// --- Group invite links (see server/src/services/groupInviteService.ts) ---
// The link's secret never reaches the server: it only gets what the secret
// encrypted, and an auth token derived from it (lib/groupInviteCrypto.ts).

export interface GroupInviteLinkDto {
  id: string;
  /** The link's secret, encrypted with the group key. */
  secretCiphertext: string;
  secretNonce: string;
  /** An admin approves each person who asks to join. */
  approval: boolean;
  createdAt: string;
}

export interface GroupInviteLinkInput {
  auth: string;
  secretCiphertext: string;
  secretNonce: string;
  previewCiphertext: string;
  previewNonce: string;
  wrappedKey: string | null;
  wrappedKeyNonce: string | null;
}

export interface GroupInvitePreviewDto {
  conversationId: string;
  previewCiphertext: string;
  previewNonce: string;
  memberCount: number;
  approval: boolean;
  /** The group key under the link's key; null when an admin approves joins. */
  wrappedKey: string | null;
  wrappedKeyNonce: string | null;
  member: boolean;
  requested: boolean;
}

export interface GroupJoinRequestDto {
  userId: string;
  createdAt: string;
}

export function getGroupInviteLink(conversationId: string): Promise<{ link: GroupInviteLinkDto | null }> {
  return request(`/group-invites/groups/${conversationId}/link`);
}

/** Turns the link on, or resets it: a new id and secret, so the old link stops working. */
export function setGroupInviteLink(conversationId: string, input: GroupInviteLinkInput): Promise<{ link: GroupInviteLinkDto }> {
  return request(`/group-invites/groups/${conversationId}/link`, { method: "PUT", body: JSON.stringify(input) });
}

export function setGroupInviteApproval(
  conversationId: string,
  approval: boolean,
  wrapped?: { wrappedKey: string; wrappedKeyNonce: string }
): Promise<{ link: GroupInviteLinkDto }> {
  return request(`/group-invites/groups/${conversationId}/link`, { method: "PATCH", body: JSON.stringify({ approval, ...wrapped }) });
}

export function deleteGroupInviteLink(conversationId: string): Promise<void> {
  return request(`/group-invites/groups/${conversationId}/link`, { method: "DELETE" });
}

export function listGroupJoinRequests(conversationId: string): Promise<GroupJoinRequestDto[]> {
  return request(`/group-invites/groups/${conversationId}/requests`);
}

export function approveGroupJoinRequest(conversationId: string, userId: string, wrappedKey: string, keyEpoch?: number, keyNotice?: ChatKeyNotice): Promise<void> {
  return request(`/group-invites/groups/${conversationId}/requests/${userId}/approve`, { method: "POST", body: JSON.stringify({ wrappedKey, keyEpoch, keyNotice }) });
}

/** An admin turning a request down, or the person who asked taking it back. */
export function denyGroupJoinRequest(conversationId: string, userId: string): Promise<void> {
  return request(`/group-invites/groups/${conversationId}/requests/${userId}`, { method: "DELETE" });
}

export function openGroupInvite(linkId: string, auth: string): Promise<GroupInvitePreviewDto> {
  return request(`/group-invites/links/${encodeURIComponent(linkId)}/open`, { method: "POST", body: JSON.stringify({ auth }) });
}

export function joinGroupInvite(
  linkId: string,
  auth: string,
  wrappedKey: string | null
): Promise<{ status: "joined" | "member" | "requested"; conversationId: string }> {
  return request(`/group-invites/links/${encodeURIComponent(linkId)}/join`, { method: "POST", body: JSON.stringify({ auth, wrappedKey }) });
}

// --- Channels (see server/src/services/channelService.ts) ---
// Plain text by design. Reading goes out WITHOUT the session cookie, so the
// server can't tell which account reads which channel; following is kept on
// this device (lib/localPrefs.ts). A private channel also needs its link's
// access token (lib/channels.ts), sent as X-Channel-Access.

export interface ChannelDto {
  id: string;
  name: string;
  description: string;
  createdAt: string;
  lastPostAt: string | null;
  /** Left out of the directory; readable only with the private link. */
  isPrivate: boolean;
  /** Same-origin API path of the channel's photo, or null. Fetch it with fetchChannelAvatar (no cookie). */
  avatarUrl: string | null;
  /** The owner takes suggested posts from followers. */
  acceptSuggestions: boolean;
}

/** The owner's own channel: also the private link's secret, sealed to their public key. */
export interface OwnChannelDto extends ChannelDto {
  secretSealed: string | null;
}

export type ChannelMediaKind = "photo" | "video" | "file" | "audio" | "voice" | "round";

/** A photo, video, file or recording in a post: `url` is a signed, expiring link (same origin) to its bytes. */
export interface ChannelMediaDto {
  id: string;
  kind: ChannelMediaKind;
  mime: string;
  size: number;
  filename: string | null;
  durationMs: number | null;
  width: number | null;
  height: number | null;
  url: string;
}

/** A poll as readers see it. A quiz's `correct` stays null until it's closed (or in the answer to your own vote). */
export interface ChannelPollDto {
  question: string;
  options: string[];
  multiple: boolean;
  quiz: boolean;
  closed: boolean;
  counts: number[];
  voters: number;
  correct: number | null;
  explanation: string | null;
}

export interface ChannelChecklistDto {
  title: string;
  items: { text: string; done: boolean }[];
  /** Followers may tick items too. */
  collaborative: boolean;
}

export interface ChannelPostDto {
  id: string;
  /** Formatted text (lib/textFormat.ts); may be empty when the post is a photo, a poll and so on. */
  body: string;
  /** When it appeared (or, scheduled, when it will). */
  createdAt: string;
  /** How many devices have seen it, in the channel or forwarded into a chat. */
  views: number;
  /** Came from a follower's suggestion (who sent it is never shown). */
  suggested?: boolean;
  poll?: ChannelPollDto | null;
  checklist?: ChannelChecklistDto | null;
  media?: ChannelMediaDto[];
}

export interface NewChannelPost {
  body: string;
  poll?: { question: string; options: string[]; multiple?: boolean; quiz?: { correct: number; explanation?: string } | null };
  checklist?: { title: string; items: string[]; collaborative?: boolean };
  /** Finished uploads (startChannelMedia + uploadChannelMedia), in order. */
  media?: string[];
  /** ISO time; omitted posts now. */
  publishAt?: string | null;
}

export interface ChannelUploadMeta {
  kind: ChannelMediaKind;
  mime: string;
  size: number;
  filename?: string | null;
  durationMs?: number | null;
  width?: number | null;
  height?: number | null;
}

export interface ChannelSuggestionDto {
  id: string;
  body: string;
  status: "pending" | "approved" | "rejected";
  createdAt: string;
  media: ChannelMediaDto[];
}

/** A request that carries no cookie and no account. */
async function publicRequest<T>(path: string, init: RequestInit = {}, access?: string | null): Promise<T> {
  const headers: Record<string, string> = { ...(init.body ? { "Content-Type": "application/json" } : {}) };
  if (access) headers["X-Channel-Access"] = access;
  const res = await fetch(`${API_BASE}${path}`, { ...init, credentials: "omit", headers });
  if (res.status === 204) return undefined as T;
  const body = await res.json().catch(() => null);
  if (!res.ok) throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  return body as T;
}

export function searchChannels(query: string, offset = 0): Promise<ChannelDto[]> {
  const params = new URLSearchParams({ limit: "30", offset: String(offset) });
  if (query.trim()) params.set("q", query.trim());
  return publicRequest(`/public/channels?${params}`);
}

/** `access`: a private channel's access token. */
export function getChannel(id: string, access?: string | null): Promise<ChannelDto> {
  return publicRequest(`/public/channels/${encodeURIComponent(id)}`, {}, access);
}

/** Newest first; `before` (a post's createdAt) pages back through the whole history. */
export function getChannelPosts(id: string, before?: string, access?: string | null, limit = 30): Promise<ChannelPostDto[]> {
  const params = new URLSearchParams({ limit: String(limit) });
  if (before) params.set("before", before);
  return publicRequest(`/public/channels/${encodeURIComponent(id)}/posts?${params}`, {}, access);
}

/** A channel's photo, fetched without the cookie (so <img> can't give away who looks at it). */
export async function fetchChannelAvatar(avatarUrl: string, access?: string | null): Promise<Blob> {
  const url = resolveApiUrl(avatarUrl);
  const res = await fetch(url, { credentials: "omit", headers: access ? { "X-Channel-Access": access } : {} });
  if (!res.ok) throw new ApiError(res.status, "avatar_unavailable", "Couldn't load the photo");
  return res.blob();
}

/** One anonymous view for each of these posts (lib/channelViews.ts makes sure each device counts a post once). */
export function recordChannelViews(postIds: string[]): Promise<void> {
  return publicRequest("/public/channels/views", { method: "POST", body: JSON.stringify({ postIds }) });
}

export function listMyChannels(): Promise<OwnChannelDto[]> {
  return request("/channels/mine");
}

export function createChannel(name: string, description: string): Promise<OwnChannelDto> {
  return request("/channels", { method: "POST", body: JSON.stringify({ name, description }) });
}

export function updateChannel(id: string, name: string, description: string): Promise<OwnChannelDto> {
  return request(`/channels/${id}`, { method: "PATCH", body: JSON.stringify({ name, description }) });
}

/** Private: with a new link's access token and its secret sealed to you (anyone with an older link loses access). */
export function setChannelVisibility(
  id: string,
  visibility: { isPrivate: false } | { isPrivate: true; access: string; secretSealed: string }
): Promise<OwnChannelDto> {
  return request(`/channels/${id}/visibility`, { method: "PUT", body: JSON.stringify(visibility) });
}

/** A downsized JPEG, base64 (lib/avatar.ts readImageAsAvatar). */
export function setChannelAvatar(id: string, imageBase64: string): Promise<OwnChannelDto> {
  return request(`/channels/${id}/avatar`, { method: "PUT", body: JSON.stringify({ image: imageBase64 }) });
}

export function removeChannelAvatar(id: string): Promise<void> {
  return request(`/channels/${id}/avatar`, { method: "DELETE" });
}

export function deleteChannel(id: string): Promise<void> {
  return request(`/channels/${id}`, { method: "DELETE" });
}

export function postToChannel(id: string, post: NewChannelPost): Promise<ChannelPostDto> {
  return request(`/channels/${id}/posts`, { method: "POST", body: JSON.stringify(post) });
}

/** A private channel's access token goes along with signed-in calls about its posts. */
function accessHeaders(access?: string | null): Record<string, string> {
  return access ? { "X-Channel-Access": access } : {};
}

/** Owner: starts uploading something for a post. Then send its bytes with uploadChannelMedia. */
export function startChannelMedia(id: string, meta: ChannelUploadMeta): Promise<{ id: string; uploadUrl: string }> {
  return request(`/channels/${id}/media`, { method: "POST", body: JSON.stringify(meta) });
}

/** A follower: starts uploading a photo for a suggested post. */
export function startSuggestionMedia(id: string, meta: ChannelUploadMeta, access?: string | null): Promise<{ id: string; uploadUrl: string }> {
  return request(`/channels/${id}/suggestions/media`, { method: "POST", body: JSON.stringify(meta), headers: accessHeaders(access) });
}

/**
 * Sends an upload's bytes (streamed from disk by the browser, so even a 2 GB
 * file never sits in memory), reporting progress from 0 to 1.
 */
export function uploadChannelMedia(uploadUrl: string, file: Blob, onProgress?: (fraction: number) => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", resolveApiUrl(uploadUrl));
    xhr.withCredentials = true;
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) return resolve();
      let body: { error?: string; message?: string } | null = null;
      try {
        body = JSON.parse(xhr.responseText);
      } catch {
        body = null;
      }
      const message = xhr.status === 413 ? "That file is too big for this server." : (body?.message ?? "The upload failed.");
      reject(new ApiError(xhr.status, body?.error ?? "upload_failed", message));
    };
    xhr.onerror = () => reject(new ApiError(0, "network_error", "The upload was interrupted. Check your connection and try again."));
    xhr.send(file);
  });
}

/** Owner: posts waiting for their time. */
export function listScheduledPosts(id: string): Promise<ChannelPostDto[]> {
  return request(`/channels/${id}/scheduled`);
}

export function publishPostNow(id: string, postId: string): Promise<ChannelPostDto> {
  return request(`/channels/${id}/posts/${postId}/publish`, { method: "POST" });
}

/** Your vote (an empty list takes a poll vote back). A quiz's answer comes back with the right one. */
export function voteInChannelPoll(id: string, postId: string, options: number[], access?: string | null): Promise<{ poll: ChannelPollDto; mine: number[] }> {
  return request(`/channels/${id}/posts/${postId}/vote`, { method: "POST", body: JSON.stringify({ options }), headers: accessHeaders(access) });
}

export function closeChannelPoll(id: string, postId: string): Promise<ChannelPostDto> {
  return request(`/channels/${id}/posts/${postId}/close`, { method: "POST" });
}

export function setChannelChecklistItem(id: string, postId: string, index: number, done: boolean, access?: string | null): Promise<ChannelChecklistDto> {
  return request(`/channels/${id}/posts/${postId}/checklist/${index}`, { method: "PUT", body: JSON.stringify({ done }), headers: accessHeaders(access) });
}

export function setChannelSettings(id: string, settings: { acceptSuggestions: boolean }): Promise<OwnChannelDto> {
  return request(`/channels/${id}/settings`, { method: "PUT", body: JSON.stringify(settings) });
}

export function suggestChannelPost(id: string, post: { body: string; media?: string[] }, access?: string | null): Promise<ChannelSuggestionDto> {
  return request(`/channels/${id}/suggestions`, { method: "POST", body: JSON.stringify(post), headers: accessHeaders(access) });
}

/** The suggestions you sent this channel, and what became of them. */
export function listMySuggestions(id: string): Promise<ChannelSuggestionDto[]> {
  return request(`/channels/${id}/suggestions/mine`);
}

/** Owner: suggestions waiting for you. */
export function listChannelSuggestions(id: string): Promise<ChannelSuggestionDto[]> {
  return request(`/channels/${id}/suggestions`);
}

export function approveChannelSuggestion(id: string, suggestionId: string, edit: { body?: string; publishAt?: string | null }): Promise<ChannelPostDto> {
  return request(`/channels/${id}/suggestions/${suggestionId}/approve`, { method: "POST", body: JSON.stringify(edit) });
}

export function rejectChannelSuggestion(id: string, suggestionId: string): Promise<void> {
  return request(`/channels/${id}/suggestions/${suggestionId}/reject`, { method: "POST" });
}

export function deleteChannelPost(id: string, postId: string): Promise<void> {
  return request(`/channels/${id}/posts/${postId}`, { method: "DELETE" });
}

export function reportChannel(id: string, reason: string): Promise<void> {
  return request(`/channels/${id}/report`, { method: "POST", body: JSON.stringify({ reason }) });
}

// --- Billing (Stripe; the webhook, not the browser, is what upgrades an account) ---

export function getBillingConfig(): Promise<{
  paymentLink: string | null;
  premiumPaymentLink?: string | null;
  /** Each plan's Files storage in bytes; null when the server has no Files. */
  filesStorage?: { free: number; pro: number; premium: number } | null;
}> {
  return request("/billing/config");
}

/** Where the account's plan is headed; see server/src/services/billingService.ts's PlanStatus. */
export interface PlanStatus {
  tier: SubscriptionTier;
  source: "stripe" | "crypto" | "granted" | null;
  renews: boolean;
  /** Last day of Pro (YYYY-MM-DD) when nothing renews it. */
  endsOn: string | null;
  canCancel: boolean;
  canResume: boolean;
  /** The card subscription can move to this plan from Settings (Pro to Premium or back). Absent on older servers. */
  canSwitchTo?: "pro" | "premium" | null;
}

export function getPlanStatus(): Promise<PlanStatus> {
  return request("/billing/status");
}

/** Switch to Free: the card subscription stops renewing; Pro lasts until the paid period ends. */
export function cancelPlan(): Promise<PlanStatus> {
  return request("/billing/cancel", { method: "POST" });
}

/** Undoes cancelPlan() before the period ends. */
export function resumePlan(): Promise<PlanStatus> {
  return request("/billing/resume", { method: "POST" });
}

/** Moves a card subscription between Pro and Premium; Stripe charges or credits the difference. */
export function switchPlan(tier: "pro" | "premium"): Promise<PlanStatus> {
  return request("/billing/switch", { method: "POST", body: JSON.stringify({ tier }) });
}

/** The payment link with this account's id attached, so Stripe's webhook can tell the server which account paid. */
export function paymentLinkFor(paymentLink: string, userId: string): string {
  const url = new URL(paymentLink);
  url.searchParams.set("client_reference_id", userId);
  return url.toString();
}

// --- Calls (see server/src/services/callService.ts) ---
// The server only relays; every offer/answer is sealed between the two
// people's identity keys (lib/callCrypto.ts) before it gets here.

export type CallKind = "voice" | "video";
export type CallEndReason = "hangup" | "declined" | "missed" | "busy" | "failed";

export interface SealedSignalDto {
  ciphertext: string;
  nonce: string;
}

export interface IceServerDto {
  urls: string[];
  username: string;
  credential: string;
}

export type CallEventDto =
  | {
      type: "incoming";
      callId: string;
      /** A call between contacts; null for a call link. */
      conversationId: string | null;
      /** Someone asking to join through one of your call links. */
      linkId?: string | null;
      callerId: string;
      kind: CallKind;
      offer: SealedSignalDto;
      iceServers: IceServerDto[];
    }
  | { type: "answered"; callId: string; answer: SealedSignalDto }
  | { type: "signal"; callId: string; signal: SealedSignalDto }
  | { type: "screen"; callId: string; on: boolean }
  | { type: "ended"; callId: string; reason: CallEndReason };

/** Group calls (see server/src/services/groupCallService.ts). */
export type GroupCallEventDto =
  | { type: "group-incoming"; callId: string; conversationId: string; hostId: string; kind: CallKind }
  | {
      type: "group-roster";
      callId: string;
      conversationId: string;
      epoch: number;
      keyholder: string;
      members: { userId: string; identity: string }[];
    }
  | { type: "group-key"; callId: string; epoch: number; from: string; key: SealedSignalDto }
  | { type: "group-ring-stop"; callId: string }
  | { type: "group-ended"; callId: string; conversationId: string };

/** Everything the live channel delivers: call signaling, and typing in your chats. */
export type LiveEventDto =
  | CallEventDto
  | GroupCallEventDto
  | { type: "typing"; conversationId: string; userId: string }
  // Files arrived through one of your share links (content-free): refresh Incoming.
  | { type: "share_upload" };

export function startCall(conversationId: string, kind: CallKind): Promise<{ callId: string; calleeId: string; iceServers: IceServerDto[] }> {
  return request("/calls", { method: "POST", body: JSON.stringify({ conversationId, kind }) });
}

/** Long poll on the live channel: resolves with new events, or none after ~25s. Without `after`, returns at once with any call ringing now. */
export function callEvents(
  after: number | null,
  signal?: AbortSignal
): Promise<{ cursor: number; events: LiveEventDto[]; callsEnabled?: boolean; groupCallsEnabled?: boolean }> {
  return request(`/live/events${after === null ? "" : `?after=${after}`}`, { signal });
}

/** "I'm typing in this chat" (only sent while typing indicators are on). */
export function sendTyping(conversationId: string): Promise<void> {
  return request("/live/typing", { method: "POST", body: JSON.stringify({ conversationId }) });
}

/** Which contacts are online (empty unless you share your own status). */
export function getPresence(): Promise<{ online: string[] }> {
  return request("/live/presence");
}

/** TURN credentials, for Settings' relay check. */
export function getRelays(): Promise<{ iceServers: IceServerDto[] }> {
  return request("/calls/relays");
}

export function sendCallOffer(callId: string, offer: SealedSignalDto): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/offer`, { method: "POST", body: JSON.stringify(offer) });
}

export function sendCallAnswer(callId: string, answer: SealedSignalDto): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/answer`, { method: "POST", body: JSON.stringify(answer) });
}

/** Fresh TURN credentials for a call you're in, before the first ones expire (lib/turnRenewal.ts). */
export function renewCallRelay(callId: string): Promise<{ iceServers: IceServerDto[]; expiresAt: string }> {
  return request(`/calls/${encodeURIComponent(callId)}/relay-credentials`, { method: "POST" });
}

export function sendCallSignal(callId: string, signal: SealedSignalDto): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/signal`, { method: "POST", body: JSON.stringify(signal) });
}

export function declineCall(callId: string): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/decline`, { method: "POST" });
}

export function hangUpCall(callId: string): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/hangup`, { method: "POST" });
}

/** For a tab being closed: fire-and-forget (keepalive outlives the page). */
export function hangUpCallOnUnload(callId: string): void {
  fetch(`${API_BASE}/calls/${encodeURIComponent(callId)}/hangup`, { method: "POST", credentials: "include", keepalive: true }).catch(() => {
    // Best effort; the server ends the call anyway once this app stops listening.
  });
}

/** Screen sharing is Premium: the server checks, then tells the other side. */
export function setCallScreenShare(callId: string, on: boolean): Promise<void> {
  return request(`/calls/${encodeURIComponent(callId)}/screen`, { method: "POST", body: JSON.stringify({ on }) });
}

// --- Group calls (see server/src/services/groupCallService.ts) ---
// Media goes through a LiveKit SFU, encrypted end to end with a room key
// that's sealed to each participant (lib/groupCallCrypto.ts).

export interface GroupCallJoinDto {
  callId: string;
  conversationId: string;
  kind: CallKind;
  /** LiveKit signaling: a path on this site (proxied by nginx) or a wss:// URL. */
  url: string;
  token: string;
  identity: string;
  /** This request started the call (rather than joining one already going on): the starter's app posts the call's link in the chat. */
  created: boolean;
}

/** Starts a call in a group, or joins the one already going on there. */
export function startGroupCall(conversationId: string, kind: CallKind): Promise<GroupCallJoinDto> {
  return request("/group-calls", { method: "POST", body: JSON.stringify({ conversationId, kind }) });
}

export function joinGroupCall(callId: string): Promise<GroupCallJoinDto> {
  return request(`/group-calls/${encodeURIComponent(callId)}/join`, { method: "POST" });
}

export function listActiveGroupCalls(): Promise<{ calls: { callId: string; conversationId: string; kind: CallKind; participants: number }[] }> {
  return request("/group-calls/active");
}

export function declineGroupCall(callId: string): Promise<void> {
  return request(`/group-calls/${encodeURIComponent(callId)}/decline`, { method: "POST" });
}

export function leaveGroupCall(callId: string): Promise<void> {
  return request(`/group-calls/${encodeURIComponent(callId)}/leave`, { method: "POST" });
}

/** Closing the tab: leave without waiting (a normal request may be cancelled). */
export function leaveGroupCallOnUnload(callId: string): void {
  fetch(`${API_BASE}/group-calls/${encodeURIComponent(callId)}/leave`, { method: "POST", credentials: "include", keepalive: true }).catch(() => {
    // Best effort; the server drops people whose app stopped listening anyway.
  });
}

/** The keyholder hands out this epoch's room key, sealed to each other participant. */
export function sendGroupCallKeys(callId: string, epoch: number, keys: { userId: string; key: SealedSignalDto }[]): Promise<void> {
  return request(`/group-calls/${encodeURIComponent(callId)}/keys`, { method: "POST", body: JSON.stringify({ epoch, keys }) });
}

// --- Call links (see server/src/services/callLinkService.ts) ---

export interface CallLinkDto {
  id: string;
  kind: CallKind;
  /** The link's secret, sealed to your own public key. */
  secretSealed: string;
  /** The name, encrypted under the link's secret. */
  nameCiphertext: string;
  nameNonce: string;
  createdOn: string;
}

export function listCallLinks(): Promise<CallLinkDto[]> {
  return request("/call-links");
}

export function createCallLink(input: { kind: CallKind; secretSealed: string; nameCiphertext: string; nameNonce: string }): Promise<CallLinkDto> {
  return request("/call-links", { method: "POST", body: JSON.stringify(input) });
}

export function deleteCallLink(id: string): Promise<void> {
  return request(`/call-links/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function getCallLinkInfo(id: string): Promise<{ id: string; kind: CallKind; ownerId: string; nameCiphertext: string; nameNonce: string }> {
  return request(`/call-links/${encodeURIComponent(id)}`);
}

export function joinCallLink(id: string): Promise<{ callId: string; calleeId: string; kind: CallKind; iceServers: IceServerDto[] }> {
  return request(`/call-links/${encodeURIComponent(id)}/join`, { method: "POST" });
}

// --- Linked devices ---

export interface SessionDto {
  id: string;
  current: boolean;
  /** Password entered, two-factor code not (yet): this sign-in can't do anything until it is. Absent on older servers. */
  awaitingTwoFactor?: boolean;
  expiresOn: string;
  /** The device's name, encrypted by your own devices. */
  label: { ciphertext: string; nonce: string } | null;
}

export function listSessions(): Promise<SessionDto[]> {
  return request("/auth/sessions");
}

export function setSessionLabel(label: { ciphertext: string; nonce: string }): Promise<void> {
  return request("/auth/sessions/current/label", { method: "PUT", body: JSON.stringify(label) });
}

/** Signs out every device but this one. */
export function signOutOtherSessions(): Promise<void> {
  return request("/auth/sessions", { method: "DELETE" });
}

export function signOutSession(id: string): Promise<void> {
  return request(`/auth/sessions/${encodeURIComponent(id)}`, { method: "DELETE" });
}

// --- Verifiable transparency log (public; see server/src/services/transparencyLogService.ts) ---

export interface LogCheckpointDto {
  treeSize: number;
  rootHash: string;
  signedAt: string;
  signature: string;
  publicKey: string;
}

export interface LogEntryDto {
  index: number;
  commitment: string;
  sizeKb: number;
  checkpointSize: number;
  loggedAt: string;
}

export function getLogCheckpoint(): Promise<{ checkpoint: LogCheckpointDto | null }> {
  return request("/public/transparency-log/checkpoint");
}

export function getLogCheckpointAt(treeSize: number): Promise<LogCheckpointDto> {
  return request(`/public/transparency-log/checkpoints/${treeSize}`);
}

export function getLogEntries(options: { before?: number; limit?: number; q?: string } = {}): Promise<LogEntryDto[]> {
  const params = new URLSearchParams();
  if (options.before !== undefined) params.set("before", String(options.before));
  if (options.limit !== undefined) params.set("limit", String(options.limit));
  if (options.q) params.set("q", options.q);
  const qs = params.toString();
  return request(`/public/transparency-log/entries${qs ? `?${qs}` : ""}`);
}

export function getLogProof(index: number, treeSize: number): Promise<{ index: number; treeSize: number; leafHash: string; path: string[] }> {
  return request(`/public/transparency-log/entries/${index}/proof?treeSize=${treeSize}`);
}

export function getLogConsistency(from: number, to: number): Promise<{ from: number; to: number; path: string[] }> {
  return request(`/public/transparency-log/consistency?from=${from}&to=${to}`);
}

// --- Files (encrypted cloud storage) and storage analytics ---
// Everything here is ciphertext plus sizes: names, folders, types and dates
// are inside metaCiphertext, which only this device can open (lib/driveMeta.ts).

export interface BackupItemDto {
  id: string;
  kind: "file" | "folder";
  encryptedSize: number;
  ciphertextSha256: string | null;
  metaCiphertext: string;
  metaNonce: string;
  /** Who the file is shared with directly (user ids). */
  sharedWith: string[];
}

export interface BackupUsageDto {
  used: number;
  limit: number;
  remaining: number;
  tier: SubscriptionTier;
}

export interface BackupLinkDto {
  id: string;
  backupId: string;
  hasPassword: boolean;
  allowDownload: boolean;
  expiresAt: string | null;
}

export function listBackupItems(): Promise<{ items: BackupItemDto[]; usage: BackupUsageDto }> {
  return request("/backup/items");
}

export function listBackupLinks(): Promise<{ links: BackupLinkDto[] }> {
  return request("/backup/links");
}

export function deleteBackupItem(id: string): Promise<void> {
  return request(`/backup/${encodeURIComponent(id)}`, { method: "DELETE" });
}

/** Rename, move, trash or restore: the metadata, re-sealed on this device. */
export function updateBackupMeta(id: string, metaCiphertext: string, metaNonce: string): Promise<BackupItemDto> {
  return request(`/backup/${encodeURIComponent(id)}/meta`, { method: "PUT", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function deleteBackupLink(linkId: string): Promise<void> {
  return request(`/backup/links/${encodeURIComponent(linkId)}`, { method: "DELETE" });
}

export interface BackupTransferDto {
  mode: "proxy" | "direct";
  url: string;
  method: "PUT" | "GET";
  headers: Record<string, string>;
  expiresAt: string;
}

/** Room for one encrypted file in Files: its exact encrypted size, hash and sealed metadata. */
export function requestBackupUpload(input: {
  encryptedSize: number;
  ciphertextSha256: string;
  metaCiphertext: string;
  metaNonce: string;
}): Promise<{ objectId: string; upload: BackupTransferDto }> {
  return request("/backup/upload-request", { method: "POST", body: JSON.stringify(input) });
}

export function createBackupFolder(metaCiphertext: string, metaNonce: string): Promise<BackupItemDto> {
  return request("/backup/folders", { method: "POST", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function completeBackupUpload(id: string): Promise<BackupItemDto> {
  return request(`/backup/${encodeURIComponent(id)}/complete`, { method: "POST", body: "{}" });
}

export function backupDownloadTarget(id: string): Promise<{ download: BackupTransferDto }> {
  return request(`/backup/${encodeURIComponent(id)}/download`);
}

// --- Public share links (docs/public-share-links.md) ---

export interface SharePasswordDto {
  salt: string;
  ops: number;
  mem: number;
}

export interface ShareLinkDto {
  id: string;
  publicId: string;
  /** The shared file or folder (the owner's own storage id; never in the link). */
  backupId: string;
  kind: "file" | "folder";
  permission: "viewer" | "contributor";
  keyEpoch: number;
  password: SharePasswordDto | null;
  envelope: string;
  envelopeNonce: string;
  ownerBox: string;
  ownerBoxNonce: string;
  uploadKeySalt: string;
  expiresAt: string | null;
  expired: boolean;
  revoked: boolean;
  maxDownloads: number | null;
  downloadCount: number;
  openCount: number;
  upload: { maxFileSize: number | null; allowance: number | null; maxFiles: number | null; count: number; bytes: number; inProgress: number };
  /** Files that came in through this link and wait in Incoming. */
  incoming: number;
}

export interface NewShareLinkInput {
  backupId: string;
  permission: "viewer" | "contributor";
  lookupVerifier: string;
  accessVerifier: string;
  password: SharePasswordDto | null;
  envelope: string;
  envelopeNonce: string;
  ownerBox: string;
  ownerBoxNonce: string;
  uploadKeySalt: string;
  expiresAt: string | null;
  maxDownloads: number | null;
  upload: { maxFileSize: number | null; allowance: number | null; maxFiles: number | null } | null;
}

export interface ShareSettingsInput {
  permission?: "viewer" | "contributor";
  expiresAt?: string | null;
  maxDownloads?: number | null;
  upload?: { maxFileSize?: number | null; allowance?: number | null; maxFiles?: number | null };
  ownerBox?: string;
  ownerBoxNonce?: string;
}

export interface ShareOwnerItemDto {
  itemId: string;
  backupId: string;
  kind: "file" | "folder";
  metaCiphertext: string;
  metaNonce: string;
}

export interface IncomingFileDto {
  id: string;
  /** Null once the link it came through was deleted. */
  shareId: string | null;
  encryptedSize: number;
  ciphertextSha256: string;
  sealedMeta: string;
  keySalt: string;
}

export function listShareLinks(): Promise<{ links: ShareLinkDto[]; incoming: number }> {
  return request("/shares");
}

export function createShareLink(input: NewShareLinkInput): Promise<ShareLinkDto> {
  return request("/shares", { method: "POST", body: JSON.stringify(input) });
}

export function updateShareLink(id: string, input: ShareSettingsInput): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}`, { method: "PATCH", body: JSON.stringify(input) });
}

export function setShareLinkPassword(
  id: string,
  input: { password: SharePasswordDto | null; accessVerifier: string; envelope: string; envelopeNonce: string }
): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/password`, { method: "PUT", body: JSON.stringify(input) });
}

export function regenerateShareLink(
  id: string,
  input: {
    lookupVerifier: string;
    accessVerifier: string;
    password: SharePasswordDto | null;
    envelope: string;
    envelopeNonce: string;
    ownerBox: string;
    ownerBoxNonce: string;
  }
): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/regenerate`, { method: "POST", body: JSON.stringify(input) });
}

export function revokeShareLink(id: string): Promise<ShareLinkDto> {
  return request(`/shares/${encodeURIComponent(id)}/revoke`, { method: "POST", body: "{}" });
}

export function deleteShareLink(id: string): Promise<void> {
  return request(`/shares/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function listShareItems(id: string): Promise<{ keyEpoch: number; items: ShareOwnerItemDto[] }> {
  return request(`/shares/${encodeURIComponent(id)}/items`);
}

export function upsertShareItems(
  id: string,
  keyEpoch: number,
  items: { itemId: string; backupId: string; metaCiphertext: string; metaNonce: string }[]
): Promise<{ stored: number }> {
  return request(`/shares/${encodeURIComponent(id)}/items`, { method: "POST", body: JSON.stringify({ keyEpoch, items }) });
}

export function removeShareItems(id: string, itemIds: string[]): Promise<{ removed: number }> {
  return request(`/shares/${encodeURIComponent(id)}/items/remove`, { method: "POST", body: JSON.stringify({ itemIds }) });
}

export function listIncomingFiles(): Promise<{ items: IncomingFileDto[] }> {
  return request("/shares/incoming");
}

export function acceptIncomingFile(id: string, metaCiphertext: string, metaNonce: string): Promise<{ item: BackupItemDto }> {
  return request(`/shares/incoming/${encodeURIComponent(id)}/accept`, { method: "POST", body: JSON.stringify({ metaCiphertext, metaNonce }) });
}

export function rejectIncomingFile(id: string): Promise<void> {
  return request(`/shares/incoming/${encodeURIComponent(id)}/reject`, { method: "POST", body: "{}" });
}

export interface AnalyticsJournalDto {
  ciphertext: string;
  nonce: string;
  revision: number;
}

/** Pro and Premium: 403 pro_required otherwise. */
export function getAnalyticsJournal(): Promise<{ journal: AnalyticsJournalDto | null }> {
  return request("/analytics/journal");
}

/** `revision`: the one last read (0 when there was none). 409 analytics_conflict if another device wrote since. */
export function saveAnalyticsJournal(input: AnalyticsJournalDto): Promise<{ revision: number }> {
  return request("/analytics/journal", { method: "PUT", body: JSON.stringify(input) });
}

export function clearAnalyticsJournal(): Promise<void> {
  return request("/analytics/journal", { method: "DELETE" });
}

// --- Password manager (every plan, unlimited items) ---
// Ciphertext only: each item is sealed on this device (lib/passwordVault.ts).

export interface VaultItemDto {
  id: string;
  ciphertext: string;
  nonce: string;
  revision: number;
}

export function listVaultItems(): Promise<{ items: VaultItemDto[] }> {
  return request("/vault/items");
}

/** Who each of your items is shared with. */
export function listVaultShares(): Promise<{ shares: { itemId: string; recipientId: string }[] }> {
  return request("/vault/shares");
}

export function listVaultSharedWithMe(): Promise<{ items: { itemId: string; ownerId: string; sealed: string }[] }> {
  return request("/vault/shared-with-me");
}

export function createVaultItem(sealed: { ciphertext: string; nonce: string }): Promise<VaultItemDto> {
  return request("/vault/items", { method: "POST", body: JSON.stringify(sealed) });
}

/** 409 vault_conflict (details.revision) when another device changed it since `revision`. */
export function updateVaultItem(id: string, revision: number, sealed: { ciphertext: string; nonce: string }): Promise<VaultItemDto> {
  return request(`/vault/items/${encodeURIComponent(id)}`, { method: "PUT", body: JSON.stringify({ ...sealed, revision }) });
}

export function deleteVaultItem(id: string): Promise<void> {
  return request(`/vault/items/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function shareVaultItem(id: string, recipientId: string, sealed: string): Promise<void> {
  return request(`/vault/items/${encodeURIComponent(id)}/shares/${encodeURIComponent(recipientId)}`, { method: "PUT", body: JSON.stringify({ sealed }) });
}

export function unshareVaultItem(id: string, recipientId: string): Promise<void> {
  return request(`/vault/items/${encodeURIComponent(id)}/shares/${encodeURIComponent(recipientId)}`, { method: "DELETE" });
}

/** The private breach check: only a 5-character SHA-1 prefix is sent. Plain text, one "SUFFIX:COUNT" per line. */
export async function getBreachRange(prefix: string): Promise<string> {
  const res = await fetch(`${API_BASE}/vault/breach-range/${encodeURIComponent(prefix)}`, { credentials: "include" });
  if (!res.ok) {
    const body = await res.json().catch(() => null);
    throw new ApiError(res.status, body?.error ?? "unknown_error", body?.message ?? res.statusText);
  }
  return res.text();
}

// --- Per-device identities (protocol 3 preview: lib/e2eV3, docs/multi-device-threat-model.md) ---
// Public keys, signed lists and ciphertext only. Off unless the server sets E2E_V3_ENABLED.

export interface V3DeviceIdentity {
  userId: string;
  deviceId: string;
  curve25519: string;
  ed25519: string;
}

export interface V3SignedKey {
  keyId: string;
  key: string;
  signature: string;
}

export interface V3SignedDeviceList {
  userId: string;
  version: number;
  devices: V3DeviceIdentity[];
  accountKey: string;
  signature: string;
  legacyCertificate?: { legacySigningKey: string; signature: string };
}

export interface V3OwnDevice {
  deviceId: string;
  curve25519: string;
  ed25519: string;
  createdOn: string;
  /** In the account's signed list (trusted); otherwise only registered, waiting for approval. */
  listed: boolean;
  oneTimeKeyCount: number;
  pendingMessages: number;
}

export interface V3ListView {
  deviceList: V3SignedDeviceList | null;
  pendingReset: { accountKey: string; effectiveAt: string } | null;
}

export interface V3MailboxItem {
  id: string;
  senderUserId: string;
  senderDeviceId: string;
  conversationId: string | null;
  body: string;
  createdOn: string;
}

export interface V3Event {
  id: number;
  kind: string;
  deviceId: string | null;
  createdOn: string;
}

export function getV3Config(): Promise<{ enabled: boolean; resetDelayHours: number }> {
  return request("/e2e/v3/config");
}

export function registerV3Device(input: { identity: V3DeviceIdentity; oneTimeKeys: V3SignedKey[]; fallbackKey: V3SignedKey | null }): Promise<{ oneTimeKeyCount: number }> {
  return request(`/e2e/v3/devices/${encodeURIComponent(input.identity.deviceId)}`, { method: "PUT", body: JSON.stringify(input) });
}

export function listV3Devices(): Promise<V3OwnDevice[]> {
  return request("/e2e/v3/devices");
}

export function removeV3Device(deviceId: string): Promise<void> {
  return request(`/e2e/v3/devices/${encodeURIComponent(deviceId)}`, { method: "DELETE" });
}

export function getV3DeviceList(userId: string, conversationId?: string | null): Promise<V3ListView> {
  const query = conversationId ? `?conversationId=${encodeURIComponent(conversationId)}` : "";
  return request(`/e2e/v3/users/${encodeURIComponent(userId)}/device-list${query}`);
}

export function putV3DeviceList(deviceList: V3SignedDeviceList, rotationProof: string | null = null): Promise<{ status: "accepted" | "pending"; effectiveAt?: string }> {
  return request("/e2e/v3/device-list", { method: "PUT", body: JSON.stringify({ deviceList, rotationProof }) });
}

export function cancelV3Reset(signature: string): Promise<void> {
  return request("/e2e/v3/device-list/cancel-reset", { method: "POST", body: JSON.stringify({ signature }) });
}

export function claimV3Keys(
  conversationId: string | null,
  devices: { userId: string; deviceId: string }[]
): Promise<{ userId: string; deviceId: string; oneTimeKey: V3SignedKey | null; fallbackKey: V3SignedKey | null }[]> {
  return request("/e2e/v3/claim", { method: "POST", body: JSON.stringify({ conversationId, devices }) });
}

export function sendV3Messages(
  senderDeviceId: string,
  conversationId: string | null,
  messages: { userId: string; deviceId: string; body: string }[]
): Promise<{ ids: string[] }> {
  return request("/e2e/v3/messages", { method: "POST", body: JSON.stringify({ senderDeviceId, conversationId, messages }) });
}

export interface V3MemberList extends V3ListView {
  userId: string;
}

/** The signed list of every active participant of a conversation (docs/multi-device-threat-model.md 9.2). */
export function getV3ConversationLists(conversationId: string): Promise<{ members: V3MemberList[] }> {
  return request(`/e2e/v3/conversations/${encodeURIComponent(conversationId)}/device-lists`);
}

/** One body for many member devices of a group (docs/multi-device-threat-model.md 9.5). */
export function sendV3GroupMessage(
  senderDeviceId: string,
  conversationId: string,
  body: string,
  recipients: { userId: string; deviceId: string }[]
): Promise<{ ids: string[] }> {
  return request("/e2e/v3/group-messages", { method: "POST", body: JSON.stringify({ senderDeviceId, conversationId, body, recipients }) });
}

export function getV3Inbox(deviceId: string): Promise<V3MailboxItem[]> {
  return request(`/e2e/v3/devices/${encodeURIComponent(deviceId)}/messages`);
}

export function ackV3Messages(deviceId: string, ids: string[]): Promise<void> {
  return request(`/e2e/v3/devices/${encodeURIComponent(deviceId)}/messages/ack`, { method: "POST", body: JSON.stringify({ ids }) });
}

export function createV3Enrollment(newDeviceId: string): Promise<{ id: string; newDeviceId: string; expiresAt: string }> {
  return request("/e2e/v3/enrollments", { method: "POST", body: JSON.stringify({ newDeviceId }) });
}

export function listV3Enrollments(): Promise<{ id: string; newDeviceId: string; expiresAt: string }[]> {
  return request("/e2e/v3/enrollments");
}

export function postV3EnrollmentMessage(id: string, fromDeviceId: string, body: string): Promise<{ seq: number }> {
  return request(`/e2e/v3/enrollments/${encodeURIComponent(id)}/messages`, { method: "POST", body: JSON.stringify({ fromDeviceId, body }) });
}

export function getV3EnrollmentMessages(id: string, after = 0): Promise<{ seq: number; fromDeviceId: string; body: string }[]> {
  return request(`/e2e/v3/enrollments/${encodeURIComponent(id)}/messages?after=${after}`);
}

export function deleteV3Enrollment(id: string): Promise<void> {
  return request(`/e2e/v3/enrollments/${encodeURIComponent(id)}`, { method: "DELETE" });
}

export function putV3RecoveryBackup(backup: string, signature: string): Promise<void> {
  return request("/e2e/v3/recovery-backup", { method: "PUT", body: JSON.stringify({ backup, signature }) });
}

export function getV3RecoveryBackup(): Promise<{ backup: string; updatedOn: string }> {
  return request("/e2e/v3/recovery-backup");
}

export function getV3Events(after = 0): Promise<V3Event[]> {
  return request(`/e2e/v3/events?after=${after}`);
}
