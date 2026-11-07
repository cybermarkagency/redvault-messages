// An in-memory stand-in for the protocol-3 endpoints of ../lib/api.js
// (server/src/services/v3DeviceService.ts), for lib/e2eV3 tests. It keeps the
// server's compare-and-set on lists and its "own account or shared
// conversation" rule loosely; the real policy is tested in the server. It
// records every request body so tests can check nothing private was sent.
import type { ApiError as ApiErrorClass, V3DeviceIdentity, V3MailboxItem, V3SignedDeviceList, V3SignedKey } from "../lib/api.js";

type ApiErrorConstructor = new (status: number, code: string, message: string) => ApiErrorClass;

interface Device {
  identity: V3DeviceIdentity;
  oneTimeKeys: V3SignedKey[];
  fallbackKey: V3SignedKey | null;
}

export class FakeV3Server {
  /** The real ApiError class (passed in: this file is loaded inside the api module's mock). */
  constructor(private readonly ApiError: ApiErrorConstructor) {}

  currentUser = "";
  readonly sent: string[] = [];
  readonly devices = new Map<string, Device>();
  readonly lists = new Map<string, V3SignedDeviceList>();
  readonly mailbox: (V3MailboxItem & { recipientUserId: string; recipientDeviceId: string })[] = [];
  readonly enrollments = new Map<string, { userId: string; newDeviceId: string; messages: { seq: number; fromDeviceId: string; body: string }[] }>();
  backups = new Map<string, string>();
  /** Group conversations: their active members' user ids. */
  readonly groups = new Map<string, string[]>();
  /** Lets a test play a server that rewrites pairing messages. */
  tamper: ((body: string) => string) | null = null;
  /** Fails the next list upload once (a network error). */
  failNextListUpload = false;

  private record(value: unknown) {
    this.sent.push(JSON.stringify(value));
  }

  private key(userId: string, deviceId: string) {
    return `${userId}/${deviceId}`;
  }

  module() {
    // eslint-disable-next-line @typescript-eslint/no-this-alias
    const self = this;
    return {
      async registerV3Device(input: { identity: V3DeviceIdentity; oneTimeKeys: V3SignedKey[]; fallbackKey: V3SignedKey | null }) {
        self.record(input);
        const key = self.key(input.identity.userId, input.identity.deviceId);
        const existing = self.devices.get(key);
        if (existing) {
          existing.oneTimeKeys.push(...input.oneTimeKeys.filter((k) => !existing.oneTimeKeys.some((o) => o.keyId === k.keyId)));
          existing.fallbackKey = input.fallbackKey ?? existing.fallbackKey;
        } else {
          self.devices.set(key, { identity: input.identity, oneTimeKeys: [...input.oneTimeKeys], fallbackKey: input.fallbackKey });
        }
        return { oneTimeKeyCount: self.devices.get(key)!.oneTimeKeys.length };
      },
      async listV3Devices() {
        const listed = new Set(self.lists.get(self.currentUser)?.devices.map((d) => d.deviceId));
        return [...self.devices.values()]
          .filter((d) => d.identity.userId === self.currentUser)
          .map((d) => ({ ...d.identity, createdOn: "2026-10-20", listed: listed.has(d.identity.deviceId), oneTimeKeyCount: d.oneTimeKeys.length, pendingMessages: 0 }));
      },
      async removeV3Device(deviceId: string) {
        self.devices.delete(self.key(self.currentUser, deviceId));
      },
      async getV3DeviceList(userId: string) {
        return { deviceList: self.lists.get(userId) ?? null, pendingReset: null };
      },
      async putV3DeviceList(deviceList: V3SignedDeviceList) {
        self.record(deviceList);
        if (self.failNextListUpload) {
          self.failNextListUpload = false;
          throw new self.ApiError(0, "network_error", "offline");
        }
        const current = self.lists.get(deviceList.userId);
        if (current && deviceList.version !== current.version + 1) throw new self.ApiError(409, "stale_device_list", "stale");
        self.lists.set(deviceList.userId, deviceList);
        return { status: "accepted" as const };
      },
      async claimV3Keys(_conversationId: string | null, targets: { userId: string; deviceId: string }[]) {
        return targets.map((t) => {
          const device = self.devices.get(self.key(t.userId, t.deviceId))!;
          const oneTimeKey = device.oneTimeKeys.shift() ?? null;
          return { ...t, oneTimeKey, fallbackKey: oneTimeKey ? null : device.fallbackKey };
        });
      },
      async sendV3Messages(senderDeviceId: string, conversationId: string | null, messages: { userId: string; deviceId: string; body: string }[]) {
        self.record({ senderDeviceId, conversationId, messages });
        const listedHere = self.lists.get(self.currentUser)?.devices.some((d) => d.deviceId === senderDeviceId);
        if (!listedHere) throw new self.ApiError(409, "device_not_listed", "not listed");
        const ids = messages.map((m) => {
          const id = crypto.randomUUID();
          self.mailbox.push({
            id,
            recipientUserId: m.userId,
            recipientDeviceId: m.deviceId,
            senderUserId: self.currentUser,
            senderDeviceId,
            conversationId,
            body: m.body,
            createdOn: "2026-10-20",
          });
          return id;
        });
        return { ids };
      },
      async getV3ConversationLists(conversationId: string) {
        const members = self.groups.get(conversationId);
        if (!members?.includes(self.currentUser)) throw new self.ApiError(404, "not_found", "no such conversation");
        return { members: members.map((userId) => ({ userId, deviceList: self.lists.get(userId) ?? null, pendingReset: null })) };
      },
      async sendV3GroupMessage(senderDeviceId: string, conversationId: string, body: string, recipients: { userId: string; deviceId: string }[]) {
        self.record({ senderDeviceId, conversationId, body, recipients });
        const members = self.groups.get(conversationId) ?? [];
        const ids = recipients.map((r) => {
          if (!members.includes(r.userId)) throw new self.ApiError(404, "not_found", "not a member");
          const id = crypto.randomUUID();
          self.mailbox.push({
            id,
            recipientUserId: r.userId,
            recipientDeviceId: r.deviceId,
            senderUserId: self.currentUser,
            senderDeviceId,
            conversationId,
            body,
            createdOn: new Date().toISOString().slice(0, 10),
          });
          return id;
        });
        return { ids };
      },
      async getV3Inbox(deviceId: string) {
        return self.mailbox.filter((m) => m.recipientUserId === self.currentUser && m.recipientDeviceId === deviceId);
      },
      async ackV3Messages(deviceId: string, ids: string[]) {
        for (let i = self.mailbox.length - 1; i >= 0; i--) {
          const m = self.mailbox[i];
          if (m.recipientUserId === self.currentUser && m.recipientDeviceId === deviceId && ids.includes(m.id)) self.mailbox.splice(i, 1);
        }
      },
      async createV3Enrollment(newDeviceId: string) {
        const id = crypto.randomUUID();
        self.enrollments.set(id, { userId: self.currentUser, newDeviceId, messages: [] });
        return { id, newDeviceId, expiresAt: new Date(Date.now() + 600_000).toISOString() };
      },
      async listV3Enrollments() {
        return [...self.enrollments.entries()]
          .filter(([, e]) => e.userId === self.currentUser)
          .map(([id, e]) => ({ id, newDeviceId: e.newDeviceId, expiresAt: new Date(Date.now() + 600_000).toISOString() }));
      },
      async postV3EnrollmentMessage(id: string, fromDeviceId: string, body: string) {
        self.record({ id, fromDeviceId, body });
        const enrollment = self.enrollments.get(id);
        if (!enrollment) throw new self.ApiError(404, "not_found", "no such pairing");
        const seq = enrollment.messages.length + 1;
        enrollment.messages.push({ seq, fromDeviceId, body: self.tamper ? self.tamper(body) : body });
        return { seq };
      },
      async getV3EnrollmentMessages(id: string, after = 0) {
        const enrollment = self.enrollments.get(id);
        if (!enrollment) throw new self.ApiError(404, "not_found", "no such pairing");
        return enrollment.messages.filter((m) => m.seq > after);
      },
      async deleteV3Enrollment(id: string) {
        self.enrollments.delete(id);
      },
      async putV3RecoveryBackup(backup: string, signature: string) {
        self.record({ backup, signature });
        self.backups.set(self.currentUser, backup);
      },
      async getV3RecoveryBackup() {
        const backup = self.backups.get(self.currentUser);
        if (!backup) throw new self.ApiError(404, "not_found", "no backup");
        return { backup, updatedOn: "2026-10-20" };
      },
      async getV3Events() {
        return [];
      },
      async getV3Config() {
        return { enabled: true, resetDelayHours: 72 };
      },
    };
  }
}
